# Where a `Map[i64, i64]` lookup's cost goes, on the M5

Measured 2026-09-09, Apple M5 Pro, `karac` at `89516aca9` with a matched archive
pair, exact counters via [`scripts/pmc.c`](../../../scripts/pmc.c), 32,000,000
lookups, `KARAC_AUTO_PAR=0`. Every probe here prints the same sink
(`4101321192`) in both languages, so the two sides do the same work.

Context: `B-2026-09-07-53`, which asks why kāra's map katas sit 1.2-2.4× behind
equal-safety Rust.

## The correction this directory exists to record

An earlier pass the same day measured Rust's hasher in a **standalone harness**
— `hash_one` in a loop, reached through `black_box` or a function pointer, its
result feeding only an accumulator — and got 102.8-118.8 instructions per hash
against kāra's 96.5. It concluded that kāra's integer hash was *cheaper* than
Rust's on arm64.

**That was wrong, and the harness is why.** Rust's hasher never runs in that
shape: in a real lookup it is inlined into the probe, which lets LLVM hoist the
two keys and the four SipHash constants out of the loop entirely and interleave
the permutation with the probe's loads. Measured in that shape instead — the
same loop, with and without the lookup — Rust's hash costs **54.5 instructions
and 10.6 cycles**, not 102.8/38.3.

The rule this earns: **measure a hash in the shape its caller actually uses it.**
An opaque call is a different measurement, not a conservative one.

## The numbers

Per lookup, 1024 live entries in a key range of 4096 (~25% hit rate, matching
kata:146's), nothing mutating during the measured loop:

| | instr | cycles |
|---|---:|---:|
| kāra `Map[i64, i64].get_or` | **132.0** | **61.5** |
| Rust `HashMap<i64, i64>.get`, `-C overflow-checks=on` | **81.0** | **32.3** |
| ratio | 1.63× | 1.90× |

Split, each side measured in its own real shape:

| | kāra | Rust |
|---|---:|---:|
| loop floor (PRNG + sink, no map) | 5.9 instr / 5.3 cyc | 4.0 / 4.3 |
| the hash | **~89.6 / ~26.8** | **54.5 / 10.6** |
| the probe | ~36.4 / ~29.6 | 22.5 / 17.4 |

- **Rust's hash** is `p_get.rs − p_hash.rs`: the identical loop with and without
  the lookup, differencing out everything but the hash.
- **kāra's hash** is a codegen-truncation differential — a throwaway `karac`
  that emits an inline multiply-xor where `emit_hash_int_call` would emit the
  call, so `full − truncated` is the hash's cost in the map's real call shape.
  Both builds answer identically (a map is correct under any hash), which is
  what makes the difference clean. 131.5-132.0 against 45.3-45.4, twice.
- **kāra's probe** is what remains. It reads **one control byte per step**
  (`ldrb w16, [x24, x15]` — linear probing, ~12 instructions per step) where
  hashbrown tests sixteen slots at once.

## What it means

**The permutation is not the problem.** Forced into kāra's shape — through a
function pointer, nothing hoisted — Rust's own hasher costs 118.8 instructions
and 39.5 cycles, *worse* than kāra's 89.6/26.8. kāra's SipHash-1-3 is fine.

**The boundary is the problem, and it is much larger than it looked.**
`B-2026-09-07-53` measured the FFI boundary at "~1 instruction" by comparing an
inlined build against an archive-linked one — but both of those were standalone
harnesses, so both were already denied the hoisting. In situ the boundary costs
**~35 instructions and ~16 cycles per hash**, because across it nothing can be
hoisted out of the caller's loop and nothing can overlap with the probe.

That also resizes the per-call seed read, measured separately at 13 instructions
and **0.46 cycles**: it is roughly a third of the instruction gap and almost
none of the cycle gap. Hoisting the seed alone does not reach this.

**Candidate 2 was built and measured. It buys nothing** — see
[`maptest.c`](maptest.c). `karac_map_get_i64`, with the permutation inlined into
the walk and the key in a register, lands at 132.5 instr / 59.5 cycles against
the existing mono probe's 133.1 / 61.0.

That is the useful result, because of what it forces:

| | instr | cycles | IPC |
|---|---:|---:|---:|
| kāra, keyed (no seed read) | 115.5 | 58.3 | 1.98 |
| Rust, behind `#[inline(never)]` | 114.9 | 35.6 | 3.23 |

**At instruction parity kāra is still 1.64× the cycles.** The gap is stalls, not
work, so no instruction-shaving fix reaches it — not the seed hoist (17
instructions, 1.2 cycles), not the typed entry point, not open-coding the
permutation.

The suspect for the stall is layout: `KaracMap` holds `status` and `kv` as **two
separate allocations**, so a lookup touches two independent cache lines in two
arrays, where hashbrown keeps control bytes and buckets in one allocation with
the controls adjacent to the data. Not measured — a hypothesis with a mechanism,
and the first thing to test.

## Where the kata's gap actually is

Ablation on kata:146's 32M-op workload, holding the PRNG, the branch structure
and the sink fixed (the full and no-map variants print the same sink):

| variant | instr | cycles |
|---|---:|---:|
| full | 13.662 G | 7.850 G |
| map replaced by a direct-indexed slot table | 0.925 G | 0.702 G |
| recency-list splices removed, map kept | 13.145 G | 7.714 G |
| PRNG + branch + sink only | 0.413 G | 0.429 G |

**The map is 93% of the instructions and 91% of the cycles.** The kata's own
doubly-linked-list splicing — four parallel `Vec`s, three pointer updates per
touch — is 4% and 2%. Ablation locates; it does not size, and the sizes above
stay on the real kata.

## Reproducing

```sh
cargo rustc -p karac-runtime --release --crate-type staticlib
cc -O2 -o pmc scripts/pmc.c

KARAC_AUTO_PAR=0 karac build p_get.kara  -o p_get_kara
KARAC_AUTO_PAR=0 karac build p_loop.kara -o p_loop_kara
rustc -O -C overflow-checks=on p_get.rs  -o p_get_ovf
rustc -O -C overflow-checks=on p_hash.rs -o p_hash_ovf
rustc -O -C overflow-checks=on p_loop.rs -o p_loop_ovf

./pmc ./p_get_kara     # 132.0 instr, 61.5 cyc per lookup
./pmc ./p_get_ovf      #  81.0 instr, 32.3 cyc
```

`hashprobe.c` and `rusthash.rs` are the SUPERSEDED standalone harnesses, kept
because the correction above is only legible next to them.
