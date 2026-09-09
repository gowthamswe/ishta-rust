#!/usr/bin/env python3
"""min/median/max wall-clock over N runs, with warmups.

The macOS half of this investigation uses hyperfine (see sweep.sh). The Linux
cloud containers this repo also runs in do not have it, and the effects under
study here are 2x-6x rather than percent-level, so a plain min-of-N is enough:

    RUNS=10 WARM=2 python3 bench.py ./decouple2
    KARAC_PAR_WORKERS=3 RUNS=10 python3 bench.py ./decouple2

It echoes the program's stdout as `sink[...]` on purpose -- every probe here
prints a checksum, and a partitioning change that alters it means the run is
measuring a different computation, not a faster one.
"""
import subprocess, sys, time, os, statistics
runs = int(os.environ.get("RUNS", "10"))
warm = int(os.environ.get("WARM", "2"))
cmd = sys.argv[1:]
out = None
ts = []
for i in range(runs + warm):
    t0 = time.perf_counter()
    p = subprocess.run(cmd, capture_output=True)
    t1 = time.perf_counter()
    if p.returncode != 0:
        print(f"FAILED rc={p.returncode}: {p.stderr.decode()[:400]}"); sys.exit(1)
    if i >= warm:
        ts.append((t1 - t0) * 1000)
    out = p.stdout.decode().strip()
print(f"min {min(ts):8.2f}ms  med {statistics.median(ts):8.2f}ms  max {max(ts):8.2f}ms  sink[{out}]")
