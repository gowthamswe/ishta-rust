#!/bin/sh
# Sample a par run at a given worker count.
#   $1 = KARAC_PAR_WORKERS   $2 = binary   $3 = output file
KARAC_PAR_WORKERS="$1" "./$2" >/dev/null 2>&1 &
pid=$!
sleep 1
sample "$pid" 2 -mayDie -f "$3" >/dev/null 2>&1
wait "$pid"
sed -n '/Sort by top of stack/,/^$/p' "$3" | head -18
