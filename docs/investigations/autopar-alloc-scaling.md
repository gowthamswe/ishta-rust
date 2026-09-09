# Auto-par scaling on the M5: it is allocation, not partitioning

Measured 2026-09-06, Apple M5 Pro (6P+12E), `karac 0.1.0-dev.8222+g2923cb4f2`
(runtime source byte-identical to `main` at the time of writing — 0 commits
touched `runtime/src/lib.rs` between that revision and `4dba34463`).

Probes live in [`autopar-alloc-scaling/`](autopar-alloc-scaling/). Every one is
a **single admitted parallel region** (verified with `KARAC_COST_DEBUG=1`) with
**uniform per-iteration cost**, so load imbalance cannot explain any result
here. Each was A/B-verified identical across `karac run`, `KARAC_AUTO_PAR=0
karac build` and the default auto-par build before being timed.

## Why this exists

`B-2026-08-28-76`, `B-2026-09-05-22` and `B-2026-09-05-23` were all filed off
**kata:282** and **kata:288**. Both katas are allocation-heavy *and*
non-uniform: 282 is an exponential recursive search that builds two Strings per
node, 288 allocates a String per punch. Filing from those two alone confounded
four separate variables — the dispatch machinery, the static partition, core
heterogeneity, and allocation. These probes vary one at a time.

## The four results

### 1. The dispatch machinery and static partition are fine

`uniform.kara` — arithmetic only, no allocation, one region:

| N | wall | user | sys |
|---|---|---|---|
| 1 | 45.60 ms | 43.38 | 1.27 |
| 2 | 23.78 ms | 43.87 | 0.98 |
| 4 | 13.28 ms | 45.19 | 1.05 |
| 18 | 4.76 ms | 45.14 | 1.27 |

**9.58× at N=18 on a 6P+12E host, with user CPU flat to 4%.** N=2 gives a clean
1.92×. 9.58× is close to the ~10.2 an asymmetric 6P+12E machine can offer if an
E core retires roughly a third of a P core, so the static equal-count split is
*not* leaving meaningful parallelism on the table.

This refutes `B-2026-08-28-76`'s leading hypothesis — "a static equal-count
partition sized to `available_parallelism()` meeting 12 efficiency cores".
The same partition, on the same host, scales essentially as well as the hardware
allows the moment the workload stops allocating.

Under background QoS (`taskpolicy -b`, all-E placement) the same probe is also
healthy — N=2 → 1.96×. So background QoS is not itself pathological, which
retires a confound in `B-2026-09-05-22`'s original evidence.

### 2. Allocation is what collapses it — and N=2 specifically

`alloc3.kara` — identical shape and uniform cost, but two short-lived Strings
per inner step via `substring` + concat, **no formatting**. 30 runs:

| N | wall | user | sys |
|---|---|---|---|
| 1 | 8.72 ms | 7.64 | 0.61 |
| **2** | **52.11 ms** | **100.25** | 1.14 |
| 3 | 5.55 ms | 10.48 | 0.62 |
| 18 | 3.96 ms | 22.97 | 1.57 |

**N=2 is 6.0× slower than N=1 and 9.4× slower than N=3**, burning 13.1× the
user CPU with essentially no system time — pure user-space burn. N=3 recovers
completely. `sample` at N=2 puts `_xzm_free` (libsystem_malloc) at the top of
stack with 2383 samples against 770 at N=3.

This is `B-2026-09-05-22`, reproduced without formatting, without imbalance,
without core-placement games, at normal QoS, in 22 lines.

### 3. It is Kāra-specific, but libmalloc bounds everyone

`mt_malloc.c` — N pthreads, same allocate/copy/free pattern, no shared state,
total allocation count held constant:

| threads | wall | user |
|---|---|---|
| 1 | 117.26 ms | 114.46 |
| 2 | 73.13 ms | 140.50 |
| 4 | 52.65 ms | 183.50 |
| 18 | 57.51 ms | 870.68 |

**Two threads in C is 1.60× faster, not 6× slower** — so the N=2 collapse
belongs to Kāra, not to macOS malloc, and `B-2026-09-05-22` stands as a real
bug with a corrected mechanism.

But note the right-hand column: past 4 threads C's wall time goes **flat** while
user CPU grows **linearly** (114 → 871 ms). macOS libmalloc does not scale this
pattern in *any* language. That is a ceiling on what auto-par can ever return
for allocation-heavy work on this host, and it should be stated as a limit
rather than chased as a Kāra defect.

### 4. The system-time explosion is `snprintf`, not page churn

`alloc2.kara` is `alloc3.kara` with the Strings built by f-string
interpolation (`f"n{acc}-{k}"`) instead of `substring`:

| N | wall | user | **sys** |
|---|---|---|---|
| 1 | 22.98 ms | 21.63 | 0.78 |
| 2 | 58.93 ms | 111.93 | 2.63 |
| 6 | 67.12 ms | 146.03 | 227.22 |
| 18 | 47.15 ms | 86.66 | **579.80** |

`alloc3`, which differs *only* in not formatting, holds sys at 1.57 ms at N=18.

`sample` on the f-string version: `__ulock_wait2` 15070, `__ulock_wake` 7516,
`__psynch_cvwait` 5195, then `__vfprintf` 840, `os_unfair_lock_lock` 539,
`_os_unfair_lock_lock_slow` 464, `__v2printf` 186, `__ultoa` 173,
`localeconv_l` 140. `/usr/bin/time -l` shows page reclaims **flat** (267 → 299)
and page faults **falling** (10 → 2).

So `B-2026-09-05-23`'s recorded mechanism — "allocator arena / page churn …
per-thread magazine growth and madvise / mach_vm traffic" — is **refuted**. The
cause is that **f-string interpolation lowers to libc `snprintf`**, which
serializes on locale and lock state across threads. Every parallel Kāra program
that formats a string pays this.

This is the most tractable of the three: a lock-free integer/float formatter in
the runtime, used by the f-string lowering instead of `snprintf`, removes it
without touching the pool or the partition.

### 5. The same bug survived in the SPEC'D f-string path

`B-2026-09-05-23` moved `f"{n}"` off `snprintf` but left `f"{n:5}"` on it, so
the two spellings sat ~23x apart. `spec.kara` is `alloc2.kara` with width specs
on the holes and nothing else changed:

| N | snprintf | via `karac_runtime_fmt_int` | allocation-free fast path |
|---|---:|---:|---:|
| 1 | 23.71 ms | 97.73 ms | **6.58 ms** |
| 18 | 46.71 ms | 35.32 ms | **2.16 ms** |
| sys @ 18 | 553.46 ms | 4.78 ms | **1.12 ms** |

The middle column is the obvious fix that does not work, and it is worth
keeping: routing spec'd holes to the EXISTING runtime formatter removes the
lock but is **4.1x slower single-threaded**, because that entry point re-parses
the spec string and returns a `String` from `FormatSpec::apply_int` on every
call. Codegen knows the spec at compile time, so the fix that works passes the
decoded fields as constants and renders straight into the caller's buffer —
no parse, no allocation, no lock. Fixed in `B-2026-09-07-24`.

## The Linux control: the collapse is macOS-only

Measured 2026-09-09, x86_64 Linux container (4 cores), **glibc 2.39**, `karac`
at `3f2342ca9` with the runtime archives rebuilt at that revision. Same probe
sources, same `KARAC_PAR_WORKERS` sweep, `bench.py` (min of 10 after 2 warmups)
in place of hyperfine.

This is the "homogeneous many-core Linux control" the section below lists as
missing. It answers a question the M5 cannot: whether the two-active-worker
collapse belongs to Kāra's allocation shape as such, or to that shape meeting
**macOS libmalloc** in particular. glibc's ptmalloc2 is a different allocator
with a different threading model (per-thread arenas rather than magazines), so
running the identical Kāra binaries against it isolates exactly that variable.

**It does not reproduce. Two active workers are a SPEEDUP on glibc.**

| probe | active workers | N=1 | pool=2 | pool=3 | pool=4 | pool=8 | pool=18 |
|---|---|---|---|---|---|---|---|
| `decouple2` | exactly 2 | 8.40 ms | 5.35 | 5.10 | 5.19 | 5.73 | 5.97 |
| `decouple3` | exactly 3 | 8.15 ms | — | 4.04 | 4.12 | 4.74 | 4.75 |
| `alloc3` | pool | 8.61 ms | 5.43 | 4.37 | 4.50 | 3.71 | — |
| `uniform` | pool | 86.47 ms | 44.47 | 30.36 | 23.15 | 24.20 | — |

`decouple2` runs **0.61-0.71x of N=1 at every pool size** — against
**5.83-5.90x SLOWER** at pool 2, 3 and 18 on the M5. The `decouple2` /
`decouple3` ratio, which is the sharpest form of the anomaly, is **1.26x** here
(5.10 vs 4.04 at pool=3) against **8.0x** there (54.61 vs 6.81); and 1.26x is
just the expected cost of splitting the same work two ways instead of three, on
a box with cores to spare.

The C controls replicate on glibc too, so the negative control travels rather
than being a macOS artifact: `mt_malloc` is 0.51x at two threads (127.7 → 65.1
ms) and `mt_malloc_main` 0.53x (121.8 → 64.1).

**Consequence for the row.** The trigger is not Kāra's allocation shape on its
own — glibc is untroubled by the identical shape at the identical worker
counts. It is that shape *meeting macOS libmalloc*, which is also consistent
with the M5 signature (`mach_absolute_time` top-of-stack out of
`libsystem_malloc`, absent at three workers). The two surviving candidates —
size class and free path — are therefore **allocator-interaction** questions to
settle on macOS, not portable defects.

## Kāra's actual allocation shape, measured

The counting probe `B-2026-09-05-22` records as ABANDONED is tractable on
Linux, and [`autopar-alloc-scaling/mcount.c`](autopar-alloc-scaling/mcount.c)
is it. The macOS DYLD interposer hit self-interposition recursion and a
pre-constructor null pointer, and cost ~2500x once both were fixed — enough to
change the contention behaviour under study. `LD_PRELOAD` avoids all three:
`dlsym(RTLD_NEXT, ...)` for the real symbols, a static bootstrap arena for
dlsym's own `calloc`, and **thread-local** counters summed only at exit, so the
hot path is one TLS increment with no atomic and no lock. Verified against a
known-answer program before use.

`decouple2` at `pool=2`, 432000 inner steps:

```
malloc=432021  calloc=6  realloc=3  free=432016  bytes=1303647
432000 requests in one bit-length bucket; mean 3.0 B
```

**One malloc per inner step, every one of them exactly 3 bytes** — not the two
per step of 1-10 bytes that `mt_malloc.c`, `mt_malloc_main.c` and
`mt_malloc_k.c` all assumed from reading the probe source. Kāra allocates
*half* as often as the control it is being compared against, which refutes
allocation density from the opposite direction to `mt_malloc_k.c`'s sweep.

Which call allocates was measured rather than inferred — three variants of one
100000-step loop with a loop-carried (non-foldable) length `n = 1..4`:

| body | mallocs | size histogram |
|---|---|---|
| `substring` only | 100033 | 25000 @ 1 B · 50000 @ 2-3 B · 25000 @ 4 B |
| `substring` + one concat | 100033 | identical |
| `substring` + two concats | 100033 | identical |

Byte-identical across all three: the **substring's buffer is the only libc
allocation**, and the concats grow it without ever reaching the allocator (the
request is `n` bytes and malloc's usable size covers the appended bytes). Two
consequences worth carrying: adding concats to a Kāra loop adds no allocator
traffic at all, and `alloc3`/`decouple2`'s "two short-lived Strings per step"
comment describes the source, not the behaviour.

One probe limitation this exposed: `decouple2`'s `acc` reaches a fixed point at
`acc % 8 == 2`, so the intended 1..8 length spread collapses to a constant. The
probe exercises exactly **one** size class, not eight — which matters before
reading anything into size-class effects.

[`autopar-alloc-scaling/mt_malloc_shape.c`](autopar-alloc-scaling/mt_malloc_shape.c)
is the control that matches the measured shape: one malloc + free per step at a
settable size, defaulting to the measured 3 bytes. On glibc it scales cleanly
(N=1 74.7 ms → N=2 38.1 → N=3 26.7 → N=4 20.3). **Running it on the M5 is the
decisive next step**: if 3-byte single-allocation steps are fast there at two
threads, size class and density are both fully refuted and the free path is
what remains.

## What this leaves open

- **`B-2026-09-05-22`** — why exactly two active workers, and not three,
  drives `_xzm_free` to dominate. Still not answered, but narrowed twice since
  this was written. The `iter_total = 2` decoupling attempt (`alloc4.kara`, not
  kept) was inconclusive because the runtime cost gate skipped the dispatch
  entirely; `decouple2.kara` / `decouple3.kara` score the body high enough to
  clear it and confirm the trigger is the ACTIVE worker count, not the pool.
  And the Linux control above shows the collapse is macOS-only, so what is left
  to explain is an interaction with libmalloc rather than a portable defect.
- **`B-2026-08-28-76`** — its stated hypothesis is refuted, but the katas do
  collapse. On this evidence the cause is (2) and (4) above plus the libmalloc
  ceiling in (3), not the partition. The homogeneous Linux control this listed
  as missing has now been run (see above) — on 4 container cores, which settles
  the allocator question but not the many-core scaling one.
- The `order_free` path already has heterogeneity-aware dynamic chunking
  (`karac_par_reduce_pooled`, `KARAC_PAR_CHUNK_FACTOR`, default 8). Neither
  kata is order-free, so neither uses it. Whether extending it to ordinary
  reductions helps is untested — and on this evidence it would not address the
  actual cause.

## Reproducing

```sh
cd docs/investigations/autopar-alloc-scaling
karac build uniform.kara -o u_par
sh sweep.sh ./u_par "" 10          # add "taskpolicy -b" as $2 for all-E
clang -O3 mt_malloc.c -o mt_malloc -lpthread && sh csweep.sh
sh prof.sh 2 a3l s_n2.txt          # top-of-stack profile at a worker count
```

On Linux (no hyperfine, no `taskpolicy`, no `sample`):

```sh
karac build decouple2.kara -o d2
KARAC_PAR_WORKERS=3 RUNS=10 python3 bench.py ./d2

gcc -shared -fPIC -O2 -o mcount.so mcount.c -ldl
KARAC_PAR_WORKERS=2 LD_PRELOAD=./mcount.so ./d2      # malloc/free counts + sizes

clang -O3 mt_malloc_shape.c -o mt_malloc_shape -lpthread
./mt_malloc_shape 2 3                                 # 2 threads, 3-byte steps
```
