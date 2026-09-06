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

## What this leaves open

- **`B-2026-09-05-22`** — why exactly two active workers, and not three,
  drives `_xzm_free` to dominate. Not answered here. The `iter_total = 2`
  decoupling attempt (`alloc4.kara`, not kept) was inconclusive: the runtime
  cost gate skipped the dispatch entirely, so it never ran two workers.
- **`B-2026-08-28-76`** — its stated hypothesis is refuted, but the katas do
  collapse. On this evidence the cause is (2) and (4) above plus the libmalloc
  ceiling in (3), not the partition. A homogeneous many-core Linux control is
  still the missing measurement, and it is container-only.
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
