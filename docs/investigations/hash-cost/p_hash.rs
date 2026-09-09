// Rust's integer hash IN THE SAME LOOP CONTEXT as p_get.rs, so the difference
// against p_get_rust is the probe and nothing else.
//
// This exists because a standalone `hash_one` harness measured 102.8
// instr/hash on this host while the WHOLE lookup in p_get.rs measures 81 --
// which cannot both be true. One of the two is not measuring what its name
// says, and the in-context form is the one to trust.
use std::collections::hash_map::RandomState;
use std::hash::BuildHasher;

fn main() {
    let key_range: i64 = 4096;
    let ops: i64 = 32_000_000;
    let state_h = RandomState::new();

    let mut sink: i64 = 0;
    let mut state: i64 = 12345;
    for _ in 0..ops {
        state = (state.wrapping_mul(1103515245).wrapping_add(12345)) & 2147483647;
        let key = (state / 65536) % key_range;
        sink = sink.wrapping_add((state_h.hash_one(key) & 0xff) as i64);
    }
    println!("{sink}");
}
