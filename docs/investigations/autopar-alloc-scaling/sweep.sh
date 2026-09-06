#!/bin/sh
# Worker sweep for B-2026-09-05-22.
#   $1 = binary to run
#   $2 = optional launcher prefix (e.g. "taskpolicy -b")
#   $3 = runs (default 10)
BIN="$1"
PREFIX="$2"
RUNS="${3:-10}"
for w in 1 2 3 4 6 12 18; do
  printf 'N=%-3s ' "$w"
  KARAC_PAR_WORKERS=$w hyperfine --warmup 2 --runs "$RUNS" --shell=none --style=none \
    "$PREFIX $BIN" --export-json "res_$w.json" >/dev/null 2>&1
  python3 -c "
import json
r=json.load(open('res_$w.json'))['results'][0]
print('wall=%9.2f ms  sd=%7.2f  user=%10.2f  sys=%9.2f' % (
    r['mean']*1000, r['stddev']*1000, r['user']*1000, r['system']*1000))
"
done
