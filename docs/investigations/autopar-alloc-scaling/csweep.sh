#!/bin/sh
# Thread sweep for the C malloc control. Total allocation count is held
# CONSTANT across thread counts, so wall time should fall roughly as 1/N
# if libmalloc scales.
for t in 1 2 3 4 6 12 18; do
  printf 'threads=%-3s ' "$t"
  hyperfine --warmup 2 --runs 10 --shell=none --style=none \
    "./mt_malloc $t" --export-json "c_$t.json" >/dev/null 2>&1
  python3 -c "
import json
r=json.load(open('c_$t.json'))['results'][0]
print('wall=%9.2f ms  sd=%7.2f  user=%10.2f  sys=%8.2f' % (
    r['mean']*1000, r['stddev']*1000, r['user']*1000, r['system']*1000))
"
done
