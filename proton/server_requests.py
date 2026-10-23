#!/usr/bin/env python3
"""Wineserver requests per thread, from the trace of `wineserver -d1` (deck-run.sh --server-trace).

  server_requests.py record OUT.json < trace
      counts the requests of every thread per wall-clock second; writes OUT.json at the end of
      the trace and on SIGUSR1
  server_requests.py summary OUT.json FROM SECS
      the threads that call the server most over SECS seconds from FROM (Unix time), each with
      its requests per second; threads are named from /proc, so run it while the game is up

Every server call is a round trip through a pipe to the one wineserver thread: a thread that
makes one per frame waits for the server's attention every frame. Signals the server sends a
thread (SIGUSR1 suspends it or fetches its context) count as "*sent signal*".
"""
import json
import os
import re
import signal
import sys
import time
from collections import Counter, defaultdict

UNIX_TID = re.compile(rb"unix_tid=(\d+)")


def record(out_path):
    seconds = defaultdict(Counter)
    unix_tids = {}

    def write(*_):
        tmp = out_path + ".tmp"
        with open(tmp, "w") as f:
            json.dump({"unix_tids": unix_tids,
                       "seconds": {str(s): {f"{t} {n}": c for (t, n), c in counts.items()}
                                   for s, counts in seconds.items()}}, f)
        os.replace(tmp, out_path)

    signal.signal(signal.SIGUSR1, write)
    for line in sys.stdin.buffer:
        # "0124: name( args )" is a request, "0124: name() = STATUS ..." its reply,
        # "0124: *sent signal* ..." an event of thread 0124.
        colon = line.find(b": ")
        if colon <= 0:
            continue
        rest = line[colon + 2:]
        if rest.startswith(b"*"):
            end = rest.find(b"*", 1)
            name = rest[:end + 1] if end > 0 else rest.strip()
            if name == b"*sent signal*":
                name = rest.strip()
        else:
            paren = rest.find(b"(")
            if paren <= 0 or rest[paren + 1:paren + 2] == b")":
                continue
            name = rest[:paren]
            if name.startswith(b"init_"):
                m = UNIX_TID.search(rest)
                if m:
                    unix_tids[line[:colon].decode()] = int(m.group(1))
        seconds[int(time.time())][(line[:colon].decode(), name.decode(errors="replace"))] += 1
    write()


def thread_name(unix_tid):
    try:
        with open(f"/proc/{unix_tid}/comm") as f:
            comm = f.read().strip()
        with open(f"/proc/{unix_tid}/status") as f:
            tgid = next(int(l.split()[1]) for l in f if l.startswith("Tgid:"))
        with open(f"/proc/{tgid}/comm") as f:
            return f"{comm} [{f.read().strip()} {tgid}]"
    except (OSError, StopIteration):
        return "(exited)"


def summary(path, start, secs):
    with open(path) as f:
        data = json.load(f)
    per_thread = defaultdict(Counter)
    covered = 0
    for s, counts in data["seconds"].items():
        if start <= int(s) < start + secs:
            covered += 1
            for key, c in counts.items():
                tid, name = key.split(" ", 1)
                per_thread[tid][name] += c
    if not covered:
        print("server requests: no trace in the window")
        return
    total = sum(sum(c.values()) for c in per_thread.values())
    print(f"server requests over {covered} s: {total / covered:.0f}/s")
    busiest = sorted(per_thread.items(), key=lambda kv: -sum(kv[1].values()))[:12]
    for tid, counts in busiest:
        unix_tid = data["unix_tids"].get(tid)
        label = thread_name(unix_tid) if unix_tid else "?"
        top = ", ".join(f"{n} {c / covered:.0f}" for n, c in counts.most_common(8))
        print(f"  {tid} {label}: {sum(counts.values()) / covered:.0f}/s  {top}")


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "record":
        record(sys.argv[2])
    elif len(sys.argv) == 5 and sys.argv[1] == "summary":
        summary(sys.argv[2], int(float(sys.argv[3])), int(sys.argv[4]))
    else:
        sys.exit(__doc__)
