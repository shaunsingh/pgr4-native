#!/usr/bin/env python3
"""Samples what limits a game run on an AMD APU (written for the Steam Deck): GPU load, GPU clock,
APU power, total CPU load and the CPU time of each of the game's threads.

  deck_sampler.py record <csv> [--match pgr4_recompiled] [--interval 0.5]
      Samples until SIGTERM/SIGINT. Rows: wall time (ISO), gpu_busy %, gpu MHz, APU W, CPU %,
      mean CPU MHz, VRAM and GTT used (MiB, whole device), the game's resident VRAM and GTT, its
      evicted and requested VRAM (MiB) and its GPU time (ms, cumulative) from its DRM clients'
      fdinfo, its resident and virtual memory and the anonymous, file and shared parts of the
      resident memory (MiB, for leaks: host allocations are anonymous, guest memory is a shared
      mapping), then one "tid:name:jiffies" field per game thread (cumulative CPU time) and, every
      PROCESS_EVERY samples, one "P<pid>:name:jiffies" field per other process (its own CPU time
      and its reaped children's, so short-lived tools count towards whoever started them).
      Older files lack the memory, the CPU MHz or the process columns; summary reads them too.
  deck_sampler.py summary <csv> <start HH:MM:SS> [seconds=40]
      Means over the window and the busiest game threads and other processes in it, in % of one
      core. The other processes show what else competes for the APU's power: wineserver and
      winedevice for the game's system calls and devices, compositors, tools left running.

Reads sysfs and procfs only: gpu_busy_percent, mem_info_vram_used, mem_info_gtt_used and the
amdgpu hwmon (freq1_input, power1_average), gpu_metrics (the SMU's CPU/GFX power split and throttle
bits), cpufreq's scaling_cur_freq, /proc/<pid>/fdinfo (the game's and the compositors'). The CPU
and GPU share the APU's power limit (15 W on a Deck), so CPU time the game burns can lower the GPU
clock. The Deck's VRAM is a 1 GB carve-out of system memory; RADV reports a larger device-local
heap, so a game can outgrow the carve-out and the kernel then moves buffers between VRAM and GTT.
"""
import argparse
import datetime as dt
import glob
import os
import signal
import struct
import subprocess
import sys
import time

HZ = os.sysconf("SC_CLK_TCK")
NUMBERS = 24  # numeric fields after the time stamp in the newest format
COMPOSITORS = ("kwin_wayland", "gamescope", "gamescope-wl", "Xwayland")
PROCESS_EVERY = 10  # samples between snapshots of every process's CPU time


def amdgpu_paths():
    for card in sorted(glob.glob("/sys/class/drm/card[0-9]")):
        dev = os.path.join(card, "device")
        busy = os.path.join(dev, "gpu_busy_percent")
        hw = glob.glob(os.path.join(dev, "hwmon", "hwmon*"))
        if os.path.exists(busy) and hw:
            return (busy, os.path.join(hw[0], "freq1_input"), os.path.join(hw[0], "power1_average"),
                    os.path.join(dev, "mem_info_vram_used"), os.path.join(dev, "mem_info_gtt_used"))
    sys.exit("no amdgpu device with gpu_busy_percent")


def apu_metrics(dev: str):
    """The SMU's power split and clocks from gpu_metrics (v2.2-v2.4, APUs): socket, CPU, GFX and SoC
    power (W), the average GFX clock (MHz) and the ASIC-independent throttle bits, whose low 16 bits
    are power limits (PPT/SPL/FPPT/SPPT). Zeros on other layouts."""
    try:
        with open(os.path.join(dev, "gpu_metrics"), "rb") as f:
            b = f.read()
    except OSError:
        return [0.0] * 5 + [0]
    if len(b) < 128 or b[2] != 2 or b[3] < 2:
        return [0.0] * 5 + [0]
    socket, cpu, soc, gfx = struct.unpack_from("<4H", b, 40)
    gfxclk = struct.unpack_from("<H", b, 64)[0]
    throttle = struct.unpack_from("<Q", b, 120)[0]
    return [socket / 1000, cpu / 1000, gfx / 1000, soc / 1000, float(gfxclk), throttle]


def read_int(path: str) -> int:
    try:
        with open(path) as f:
            return int(f.read().split()[0])
    except (OSError, ValueError, IndexError):
        return 0


def cpu_mhz() -> int:
    freqs = [read_int(p) for p in glob.glob("/sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_cur_freq")]
    return sum(freqs) // len(freqs) // 1000 if freqs else 0


def cpu_times():
    with open("/proc/stat") as f:
        v = [int(x) for x in f.readline().split()[1:]]
    idle = v[3] + v[4]
    return sum(v), idle


def game_pid(match: str):
    """The matching process with the most threads: the game, not Proton's launcher or a wrapper."""
    r = subprocess.run(["pgrep", "-f", match], capture_output=True, text=True)
    pids = [p for p in r.stdout.split() if p != str(os.getpid())]
    return max(pids, key=lambda p: len(glob.glob(f"/proc/{p}/task/*")), default=None)


CLIENT_KEYS = ("drm-resident-vram", "drm-resident-gtt", "amd-evicted-vram", "amd-requested-vram",
               "drm-engine-gfx")


def client_stats(pid: str):
    """The process's amdgpu DRM clients (each client id once): resident VRAM, resident GTT, evicted
    VRAM, requested VRAM (MiB) and GPU time on the gfx engine (ms)."""
    clients = {}
    for info in glob.glob(f"/proc/{pid}/fdinfo/*"):
        try:
            with open(info) as f:
                fields = dict(l.split(":", 1) for l in f.read().splitlines() if ":" in l)
        except OSError:
            continue
        if fields.get("drm-driver", "").strip() != "amdgpu":
            continue
        clients[fields.get("drm-client-id", info).strip()] = [
            int((fields.get(k) or "0").split()[0]) for k in CLIENT_KEYS]
    sums = [sum(c[i] for c in clients.values()) for i in range(len(CLIENT_KEYS))]
    return [*(v // 1024 for v in sums[:4]), sums[4] // 1_000_000]


def compositor_pids():
    pids = []
    for comm in glob.glob("/proc/[0-9]*/comm"):
        try:
            with open(comm) as f:
                if f.read().strip() in COMPOSITORS:
                    pids.append(comm.split("/")[2])
        except OSError:
            continue
    return pids


MEMORY_KEYS = ("VmRSS", "VmSize", "RssAnon", "RssFile", "RssShmem")


def process_memory(pid: str):
    """MEMORY_KEYS from /proc/<pid>/status, in MiB."""
    out = dict.fromkeys(MEMORY_KEYS, 0)
    try:
        with open(f"/proc/{pid}/status") as f:
            for line in f:
                key = line.split(":", 1)[0]
                if key in out:
                    out[key] = int(line.split()[1]) // 1024
    except OSError:
        pass
    return [out[k] for k in MEMORY_KEYS]


def stat_times(pattern: str, children: bool):
    """(id, name, jiffies) for each /proc stat file matching `pattern`: utime + stime, plus the
    reaped children's cutime + cstime with `children`."""
    out = []
    for stat in glob.glob(pattern):
        try:
            with open(stat) as f:
                s = f.read()
        except OSError:
            continue
        name = s[s.index("(") + 1:s.rindex(")")].replace(":", "_").replace(",", "_")
        fields = s[s.rindex(")") + 2:].split()
        jiffies = sum(int(x) for x in fields[11:15 if children else 13])
        out.append((stat.split("/")[-2], name, jiffies))
    return out


def thread_times(pid: str):
    return [f"{tid}:{name}:{j}" for tid, name, j in stat_times(f"/proc/{pid}/task/*/stat", False)]


def process_times(game: str):
    return [f"P{pid}:{name}:{j}" for pid, name, j in stat_times("/proc/[0-9]*/stat", True)
            if pid != game]


def record(args) -> None:
    busy, freq, power, vram, gtt = amdgpu_paths()
    stop = False

    def on_signal(*_):
        nonlocal stop
        stop = True

    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)
    prev = cpu_times()
    dev = os.path.dirname(busy)
    compositors, sample = [], 0
    with open(args.csv, "w", buffering=1) as out:
        while not stop:
            time.sleep(args.interval)
            if sample % 20 == 0:
                compositors = compositor_pids()
            sample += 1
            total, idle = cpu_times()
            dt_total, dt_idle = total - prev[0], idle - prev[1]
            prev = (total, idle)
            cpu = 100.0 * (1 - dt_idle / dt_total) if dt_total else 0.0
            # Looked up every time: Proton's launcher matches before the game process exists.
            pid = game_pid(args.match)
            threads = thread_times(pid) if pid else []
            game = (client_stats(pid) + process_memory(pid)) if pid else [0] * (len(CLIENT_KEYS) + len(MEMORY_KEYS))
            *watts, throttle = apu_metrics(dev)
            compositor_ms = sum(client_stats(p)[4] for p in compositors)
            row = [dt.datetime.now().isoformat(timespec="milliseconds"), str(read_int(busy)),
                   str(read_int(freq) // 1_000_000), f"{read_int(power) / 1e6:.2f}", f"{cpu:.1f}",
                   str(cpu_mhz()), str(read_int(vram) >> 20), str(read_int(gtt) >> 20),
                   *map(str, game), *(f"{w:.2f}" for w in watts), str(throttle & 0xFFFF),
                   str(compositor_ms), *threads]
            if sample % PROCESS_EVERY == 1:
                row += process_times(pid)
            out.write(",".join(row) + "\n")


def summary(args) -> None:
    rows = []
    for line in open(args.csv):
        f = line.rstrip("\n").split(",")
        if len(f) >= 5:
            # The numbers end where the threads ("tid:name:jiffies") begin; older files have fewer.
            n = next((i for i, x in enumerate(f) if ":" in x and i > 0), len(f))
            nums = [float(x) for x in f[1:n]] + [0.0] * max(0, NUMBERS - (n - 1))
            rows.append((dt.datetime.fromisoformat(f[0]), *nums[:NUMBERS], f[n:]))
    if not rows:
        sys.exit(f"{args.csv}: no samples")
    t0 = dt.datetime.combine(rows[0][0].date(), dt.datetime.strptime(args.start, "%H:%M:%S").time())
    t1 = t0 + dt.timedelta(seconds=args.seconds)
    sel = [r for r in rows if t0 <= r[0] <= t1]
    if len(sel) < 2:
        sys.exit(f"{args.csv}: fewer than 2 samples in the window")
    n = len(sel)
    gpu, mhz, watts, cpu, cpu_clock = (sum(r[i] for r in sel) / n for i in (1, 2, 3, 4, 5))
    clock = f" at {cpu_clock:.0f} MHz" if cpu_clock else ""
    print(f"{args.csv}: {n} samples, GPU busy {gpu:.0f} % at {mhz:.0f} MHz, APU {watts:.1f} W, "
          f"CPU {cpu:.0f} % (all cores){clock}")
    secs = (sel[-1][0] - sel[0][0]).total_seconds()
    if any(r[6] for r in sel):
        mem = lambda i: f"{min(r[i] for r in sel):.0f}-{max(r[i] for r in sel):.0f}"
        print(f"  memory MiB: device VRAM {mem(6)}, GTT {mem(7)}; game resident VRAM {mem(8)}, "
              f"GTT {mem(9)}, evicted VRAM {mem(10)} of {mem(11)} requested")
        gfx_ms = sel[-1][12] - sel[0][12]
        if gfx_ms > 0:
            print(f"  game GPU time {100.0 * gfx_ms / 1000.0 / secs:.0f} % of the window (fdinfo)")
    if any(r[13] for r in sel):
        print(f"  game RSS MiB {sel[0][13]:.0f} -> {sel[-1][13]:.0f} over the window "
              f"(run peak {max(r[13] for r in rows):.0f}), virtual {sel[-1][14]:.0f}")
    if any(r[15] for r in sel):
        part = lambda i: f"{sel[0][i]:.0f} -> {sel[-1][i]:.0f}"
        print(f"  resident MiB: anonymous {part(15)}, file {part(16)}, shared {part(17)}")
    if any(r[18] for r in sel):
        socket, cpu_w, gfx_w, soc_w, gfxclk = (sum(r[i] for r in sel) / n for i in (18, 19, 20, 21, 22))
        limited = sum(1 for r in sel if int(r[23])) * 100 // n
        print(f"  SMU: socket {socket:.1f} W = CPU {cpu_w:.1f} + GFX {gfx_w:.1f} + SoC {soc_w:.1f} W "
              f"(+rest), GFX clock {gfxclk:.0f} MHz, power-limit throttling in {limited} % of samples")
        comp_ms = sel[-1][24] - sel[0][24]
        if comp_ms > 0:
            print(f"  compositors' GPU time {100.0 * comp_ms / 1000.0 / secs:.0f} % of the window (fdinfo)")

    def parse(fields, processes):
        d = {}
        for f in fields:
            if f.startswith("P") == processes:
                tid, name, j = f.split(":")
                d[tid] = (name, int(j))
        return d

    def busiest(first, last, secs):
        load = []
        for tid, (name, j) in last.items():
            j0 = first.get(tid, (name, j))[1]
            load.append((100.0 * (j - j0) / HZ / secs, tid, name))
        return sorted(load, reverse=True)

    load = busiest(parse(sel[0][-1], False), parse(sel[-1][-1], False), secs)
    print(f"  game threads: {sum(l[0] for l in load):.0f} % of one core in total, {len(load)} threads")
    for pct, tid, name in load[:8]:
        print(f"  {pct:5.1f} %  {name} ({tid})")
    snaps = [r for r in sel if any(x.startswith("P") for x in r[-1])]
    if len(snaps) >= 2:
        span = (snaps[-1][0] - snaps[0][0]).total_seconds()
        load = busiest(parse(snaps[0][-1], True), parse(snaps[-1][-1], True), span)
        print(f"  other processes over {span:.0f} s: {sum(l[0] for l in load):.0f} % of one core in total")
        for pct, pid, name in load[:8]:
            if pct >= 0.5:
                print(f"  {pct:5.1f} %  {name} ({pid[1:]})")


def main() -> None:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("record")
    r.add_argument("csv")
    r.add_argument("--match", default=r"pgr4_recompiled\.exe")
    r.add_argument("--interval", type=float, default=0.5)
    s = sub.add_parser("summary")
    s.add_argument("csv")
    s.add_argument("start")
    s.add_argument("seconds", type=float, nargs="?", default=40.0)
    args = ap.parse_args()
    record(args) if args.cmd == "record" else summary(args)


if __name__ == "__main__":
    main()
