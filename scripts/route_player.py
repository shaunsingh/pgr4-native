#!/usr/bin/env python3
"""Replays a timed key script into the game under Proton on Linux, through a uinput keyboard.

Script format (the routes/*.keys routes); deck-run.sh, ab_compare.py and perf_fps.py read its log:
  <at_ms> <key> [hold_ms]   press <key> at <at_ms> after time zero, hold it hold_ms (default 250)
  +<ms>   <key> [hold_ms]   same, <ms> after the previous event's start time
  <at_ms> mark <label>      write "mark <label>" to the log
Time zero is when the game's window first exists. Keys: return, space, escape, backspace, tab,
up/down/left/right, f1-f12, a letter or digit, chords like shift+e.

Keys go through a uinput keyboard, which needs write access to /dev/uinput (SteamOS grants it to
the desktop user; elsewhere run sudo setfacl -m u:$USER:rw /dev/uinput), or with --input xtest
through the X server in $DISPLAY (xdotool), as for a headless gamescope. Focus checks go through
hyprctl on Hyprland and through xdotool on X11 or XWayland (KDE Plasma on SteamOS); gamescope
manages focus itself, and --wm none turns them off. No Python packages: the uinput device is set
up with raw ioctls.

usage: route_player.py --script route.keys --log input.log [--match pgr4_recompiled] [--timeout 120]
                       [--width 1280 --height 720] [--wm auto|hyprland|x11|gamescope|none]
                       [--input uinput|xtest] [--stretch 1.0]
"""
import argparse
import datetime as dt
import fcntl
import glob
import json
import os
import re
import shutil
import struct
import subprocess
import time

EV_SYN, EV_KEY, SYN_REPORT = 0, 1, 0
UI_SET_EVBIT, UI_SET_KEYBIT = 0x40045564, 0x40045565
UI_DEV_CREATE, UI_DEV_DESTROY = 0x5501, 0x5502

KEYS = {"escape": 1, "backspace": 14, "tab": 15, "return": 28, "enter": 28, "space": 57,
        "shift": 42, "ctrl": 29, "alt": 56, "up": 103, "left": 105, "right": 106, "down": 108}
for i, ch in enumerate("1234567890"):
    KEYS[ch] = 2 + i
for i in range(1, 13):
    KEYS[f"f{i}"] = 58 + i if i <= 10 else 76 + i  # F11 and F12 are 87 and 88
for row, start in (("qwertyuiop", 16), ("asdfghjkl", 30), ("zxcvbnm", 44)):
    for i, ch in enumerate(row):
        KEYS[ch] = start + i


class Keyboard:
    def __init__(self) -> None:
        self.fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
        fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_KEY)
        for code in set(KEYS.values()):
            fcntl.ioctl(self.fd, UI_SET_KEYBIT, code)
        # Legacy uinput_user_dev: name[80], id (bustype, vendor, product, version), ff_effects_max,
        # absmax/absmin/absfuzz/absflat[64] each.
        dev = struct.pack("80sHHHHi", b"pgr4-route-player", 0x03, 0x1234, 0x5678, 1, 0) + b"\0" * (4 * 64 * 4)
        os.write(self.fd, dev)
        fcntl.ioctl(self.fd, UI_DEV_CREATE)
        time.sleep(1.0)  # let the compositor pick up the new device

    def send(self, code: int, down: bool) -> None:
        now = time.time()
        sec, usec = int(now), int((now % 1) * 1e6)
        os.write(self.fd, struct.pack("llHHi", sec, usec, EV_KEY, code, 1 if down else 0))
        os.write(self.fd, struct.pack("llHHi", sec, usec, EV_SYN, SYN_REPORT, 0))

    def close(self) -> None:
        fcntl.ioctl(self.fd, UI_DEV_DESTROY)
        os.close(self.fd)


class XTestKeyboard:
    """Key events through the X server's XTEST extension (xdotool), for an X server that reads
    no kernel input devices, such as the Xwayland of a headless gamescope."""

    SYMS = {1: "Escape", 14: "BackSpace", 15: "Tab", 28: "Return", 57: "space", 42: "Shift_L",
            29: "Control_L", 56: "Alt_L", 103: "Up", 105: "Left", 106: "Right", 108: "Down"}

    def __init__(self) -> None:
        self.syms = dict(self.SYMS)
        for name, code in KEYS.items():
            if len(name) == 1:
                self.syms[code] = name
            elif name[0] == "f" and name[1:].isdigit():
                self.syms[code] = name.upper()

    def send(self, code: int, down: bool) -> None:
        subprocess.run(["xdotool", "keydown" if down else "keyup", self.syms[code]], timeout=10)

    def close(self) -> None:
        pass


class Hyprland:
    """Window lookup and focus through hyprctl. Windows are identified by their address."""

    def __init__(self) -> None:
        env = dict(os.environ)
        run = env.get("XDG_RUNTIME_DIR") or f"/run/user/{os.getuid()}"
        env["XDG_RUNTIME_DIR"] = run
        if not env.get("HYPRLAND_INSTANCE_SIGNATURE"):
            socks = sorted(glob.glob(f"{run}/hypr/*/.socket.sock"), key=os.path.getmtime)
            if socks:
                env["HYPRLAND_INSTANCE_SIGNATURE"] = socks[-1].split("/")[-2]
        self.env = env

    @staticmethod
    def available() -> bool:
        run = os.environ.get("XDG_RUNTIME_DIR") or f"/run/user/{os.getuid()}"
        return shutil.which("hyprctl") is not None and bool(glob.glob(f"{run}/hypr/*/.socket.sock"))

    def _ctl(self, *args: str):
        out = subprocess.run(["hyprctl", "-j", *args], capture_output=True, text=True, env=self.env, timeout=10).stdout
        try:
            return json.loads(out)
        except json.JSONDecodeError:
            return None

    def _dispatch(self, lua: str) -> None:
        """Hyprland 0.56 with a Lua config takes dispatchers as Lua (hl.dsp.*), not as words."""
        subprocess.run(["hyprctl", "dispatch", lua], capture_output=True, env=self.env, timeout=10)

    def find(self, match: str):
        for c in self._ctl("clients") or []:
            if re.search(match, f"{c.get('class', '')} {c.get('title', '')}", re.I) and c.get("mapped", True):
                return c["address"], c.get("pid")
        return None

    def focused(self, win: str) -> bool:
        w = self._ctl("activewindow")
        return bool(w) and w.get("address") == win

    def focus(self, win: str) -> bool:
        self._dispatch(f'hl.dsp.focus({{ window = "address:{win}" }})')
        time.sleep(0.2)
        return self.focused(win)

    def place(self, win: str, w: int, h: int) -> None:
        """Float the game at its own size, centred, like the Windows runs (no tiling resize)."""
        window = f'window = "address:{win}"'
        self._dispatch(f'hl.dsp.window.float({{ {window}, action = "on" }})')
        self._dispatch(f"hl.dsp.window.resize({{ {window}, x = {w}, y = {h} }})")
        self._dispatch(f"hl.dsp.window.center({{ {window} }})")


class X11:
    """Window lookup and focus through xdotool: Wine's windows are X11 clients, also under
    XWayland (KDE Plasma's Wayland session on SteamOS). The game sizes its own window."""

    @staticmethod
    def available() -> bool:
        return shutil.which("xdotool") is not None and bool(os.environ.get("DISPLAY"))

    @staticmethod
    def _xdo(*args: str) -> str:
        r = subprocess.run(["xdotool", *args], capture_output=True, text=True, timeout=10)
        return r.stdout.strip() if r.returncode == 0 else ""

    def find(self, match: str):
        # Wine names WM_CLASS after the executable, Proton outside Steam names it steam_proton;
        # the SDK's window title carries the executable's name either way.
        for field in ("--name", "--class"):
            for wid in self._xdo("search", "--onlyvisible", field, match).split():
                return wid, (self._xdo("getwindowpid", wid) or None)
        return None

    def windows(self) -> str:
        """Every named window, mapped or not, for the log when none matched."""
        ids = self._xdo("search", "--name", ".").split()
        return "; ".join(f"{w} {self._xdo('getwindowname', w)!r}" for w in ids[:40])

    def focused(self, win: str) -> bool:
        return self._xdo("getactivewindow") == win

    def focus(self, win: str) -> bool:
        self._xdo("windowactivate", win)
        time.sleep(0.2)
        return self.focused(win)

    def place(self, win: str, w: int, h: int) -> None:
        pass


class Gamescope(X11):
    """gamescope sizes and focuses the game itself. In about one start in five the game's
    top-level window is never mapped (seen with GE-Proton 11 and gamescope 3.16): gamescope then
    has no window to focus, X focus stays None and XTEST keys are dropped. Windows are therefore
    found mapped or not, and focusing maps the window first when it has to."""

    def find(self, match: str):
        for field in ("--name", "--class"):
            for wid in self._xdo("search", field, match).split():
                return wid, (self._xdo("getwindowpid", wid) or None)
        return None

    def focused(self, win: str) -> bool:
        focus = self._xdo("getwindowfocus", "-f")
        if focus in ("", "0", "1"):
            return False
        if focus == win:
            return True
        # Wine may focus a child X window of the game's top-level window. The native renderer's
        # debug window (--mode oracle) is the same process but does not take the game's input.
        return (self._xdo("getwindowpid", focus) == self._xdo("getwindowpid", win) != "" and
                not self._xdo("getwindowname", focus).startswith("PGR4 native"))

    def focus(self, win: str) -> bool:
        if win not in self._xdo("search", "--onlyvisible", "--name", ".").split():
            self._xdo("windowmap", win)
            time.sleep(0.5)
        self._xdo("windowfocus", win)
        time.sleep(0.1)
        return self.focused(win)

    def focus_name(self) -> str:
        """The window XTEST keys go to, for the log."""
        w = self._xdo("getwindowfocus", "-f")
        return f"{w} {self._xdo('getwindowname', w)!r}" if w else "none"


class NoWindowManager:
    """No window manager control: the uinput keyboard types into whatever has focus."""

    def __init__(self, match: str) -> None:
        self.match = match

    @staticmethod
    def available() -> bool:
        return True

    def find(self, match: str):
        r = subprocess.run(["pgrep", "-f", match], capture_output=True, text=True)
        pids = r.stdout.split()
        return ("process", pids[0]) if pids else None

    def focused(self, win: str) -> bool:
        return True

    def focus(self, win: str) -> bool:
        return True

    def place(self, win: str, w: int, h: int) -> None:
        pass


def window_manager(kind: str, match: str):
    if kind == "auto":
        kind = "hyprland" if Hyprland.available() else "x11" if X11.available() else "none"
    return {"hyprland": Hyprland, "x11": X11, "gamescope": Gamescope}.get(kind, lambda: NoWindowManager(match))()


def parse(text: str):
    events, prev = [], 0
    for raw in re.split(r"[\r\n;]+", text):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        f = line.split()
        at = prev + int(f[0][1:]) if f[0].startswith("+") else int(f[0])
        if at < prev:
            raise SystemExit(f"events out of order at: {raw}")
        prev = at
        if f[1] == "mark":
            events.append({"at": at, "mark": " ".join(f[2:])})
        else:
            codes = [KEYS[k] for k in f[1].lower().split("+")]
            events.append({"at": at, "keys": f[1], "codes": codes, "hold": int(f[2]) if len(f) > 2 else 250})
    return events


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--script", required=True)
    ap.add_argument("--log", required=True)
    ap.add_argument("--match", default="pgr4_recompiled")
    ap.add_argument("--timeout", type=int, default=120)
    ap.add_argument("--width", type=int, default=1280)
    ap.add_argument("--height", type=int, default=720)
    ap.add_argument("--wm", choices=("auto", "hyprland", "x11", "gamescope", "none"), default="auto")
    ap.add_argument("--input", choices=("uinput", "xtest"), default="uinput")
    # Scales every delay of the script, for machines that load slower than the one it was timed on.
    ap.add_argument("--stretch", type=float, default=1.0)
    args = ap.parse_args()
    logf = open(args.log, "a", buffering=1)

    def say(msg: str) -> None:
        logf.write(f"{dt.datetime.now().astimezone().isoformat()} {msg}\n")

    events = parse(open(args.script).read())
    for e in events:
        e["at"] = round(e["at"] * args.stretch)
    wm = window_manager(args.wm, args.match)
    kb = XTestKeyboard() if args.input == "xtest" else Keyboard()
    held = []
    try:
        deadline = time.monotonic() + args.timeout
        win = None
        while not win:
            win = wm.find(args.match)
            if time.monotonic() > deadline:
                seen = wm.windows() if hasattr(wm, "windows") else ""
                say(f"error: no window matching {args.match} after {args.timeout} s"
                    f" (DISPLAY={os.environ.get('DISPLAY', '')}; windows: {seen or 'none'})")
                raise SystemExit(1)
            time.sleep(0.1)
        t0 = time.monotonic()
        clock = lambda: int((time.monotonic() - t0) * 1000)
        addr, pid = win
        wm.place(addr, args.width, args.height)
        say(f"start pid={pid} window={addr} wm={type(wm).__name__} events={len(events)} script={args.script}")
        actions = []
        for n, e in enumerate(events):
            if "mark" in e:
                actions.append((e["at"], 1, n, "mark", e))
            else:
                actions.append((e["at"], 1, n, "down", e))
                actions.append((e["at"] + e["hold"], 0, n, "up", e))
        for at, _, _, kind, e in sorted(actions, key=lambda a: a[:3]):
            while (wait := at - clock()) > 0:
                time.sleep(min(wait, 500) / 1000)
                if not wm.focused(addr):
                    ok = wm.focus(addr)
                    if ok:
                        for h in held:
                            for c in h["codes"]:
                                kb.send(c, True)
                    say(f"refocus t={clock()} ok={ok} held={','.join(h['keys'] for h in held)}")
            if kind == "up":
                for c in reversed(e["codes"]):
                    kb.send(c, False)
                if e in held:
                    held.remove(e)
                continue
            if not wm.find(args.match):
                say(f"error: game window gone at {clock()} ms")
                raise SystemExit(1)
            if kind == "mark":
                say(f"mark {e['mark']} t={clock()}")
                continue
            if not wm.focused(addr) and not wm.focus(addr):
                say(f"error: could not focus the game for {e['keys']} at {clock()} ms")
                raise SystemExit(1)
            late = clock() - e["at"]
            for c in e["codes"]:
                kb.send(c, True)
            held.append(e)
            focus = f" focus={wm.focus_name()}" if hasattr(wm, "focus_name") else ""
            say(f"key {e['keys']} hold={e['hold']} at={e['at']} late={late}{focus}")
        say("done")
    finally:
        for e in held:
            for c in reversed(e["codes"]):
                kb.send(c, False)
        kb.close()


if __name__ == "__main__":
    main()
