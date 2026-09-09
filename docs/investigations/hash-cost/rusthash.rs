// SUPERSEDED (2026-09-09). Read README.md first, and do not quote these numbers.
//
// Every mode here measures Rust's hasher in a shape Rust never uses: `inline`
// still feeds its result to a black-boxed accumulator rather than to a bucket
// index, so LLVM keeps the keys and the four SipHash constants inside the loop
// instead of hoisting them. That inflates Rust's hash from an in-situ 54.5
// instructions to 102.8, and briefly produced the conclusion that kara's hash
// was cheaper than Rust's.
//
// The measurement that replaces it is `p_get.rs` minus `p_hash.rs` -- the same
// loop with and without the lookup, so the difference is the hash and nothing
// else. Kept only because the correction is easier to follow next to it.

// Rust's side of the same measurement, in the same call shape.
//
// B-2026-09-07-53 compares kara's integer hash to Rust's in INSTRUCTIONS
// (74 vs 94-95) and its own rule says to size anything on this host in CYCLES.
// It runs Rust's `RandomState` hash of a u64 in three shapes. `inline` is the
// one to quote: hashbrown inlines its hasher, so an `inline(never)` measurement
// charges Rust for a call it never makes. `random` / `default` are kept because
// they bound the call overhead itself.
//
// A NOTE ON THE ROTATE TRAP the row records: SipHash-1-3 on a u64 is ~30
// rotates, and two permutations in one body reads as ~60 and inflates the
// per-call figure by ~1.8x. On arm64 most rotates FOLD INTO the xor
// (`eor xN, xM, xK, ror #13`), so the count to check there is the `eor`/`add`
// pair -- kara's `karac_hash_u64` disassembles to 30 eor + 22 add + 9 ror,
// which is the full five rounds.
//
//   rustc -O rusthash.rs -o rusthash
//   ./rusthash random <iters>     # RandomState — what a HashMap actually holds
//   ./rusthash default <iters>    # DefaultHasher — keys 0,0
//   ./rusthash inline <iters>     # RandomState, INLINED — how hashbrown really
//                                 # hashes, and the only fair comparison for a
//                                 # cost kara pays across an FFI boundary
use std::collections::hash_map::{DefaultHasher, RandomState};
use std::hash::{BuildHasher, Hash, Hasher};

#[inline(never)]
fn hash_random(state: &RandomState, v: u64) -> u64 {
    let mut h = state.build_hasher();
    v.hash(&mut h);
    h.finish()
}

#[inline(never)]
fn hash_default(v: u64) -> u64 {
    let mut h = DefaultHasher::new();
    v.hash(&mut h);
    h.finish()
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let mode = args.get(1).map(|s| s.as_str()).unwrap_or("random");
    let iters: u64 = args.get(2).and_then(|s| s.parse().ok()).unwrap_or(20_000_000);
    let mut acc: u64 = 0;

    match mode {
        "random" => {
            let state = RandomState::new();
            // Through a pointer, so it is a call and not an inlined body.
            let f: fn(&RandomState, u64) -> u64 = hash_random;
            let f = std::hint::black_box(f);
            for i in 0..iters {
                acc = acc.wrapping_add(f(&state, i ^ acc));
            }
        }
        "default" => {
            let f: fn(u64) -> u64 = hash_default;
            let f = std::hint::black_box(f);
            for i in 0..iters {
                acc = acc.wrapping_add(f(i ^ acc));
            }
        }
        "inline" => {
            // No function pointer and no `inline(never)`: the hasher is built
            // and driven straight in the loop, which is what a `HashMap`
            // lookup gets after inlining.
            let state = RandomState::new();
            for i in 0..iters {
                acc = acc.wrapping_add(state.hash_one(i ^ acc));
            }
            acc = std::hint::black_box(acc);
        }
        _ => {
            eprintln!("mode must be random|default|inline");
            std::process::exit(2);
        }
    }
    println!("sink {acc}");
}
