// The loop floor for p_get.rs / p_hash.rs: PRNG, key, sink, nothing else.
fn main() {
    let key_range: i64 = 4096;
    let ops: i64 = 32_000_000;
    let mut sink: i64 = 0;
    let mut state: i64 = 12345;
    for _ in 0..ops {
        state = (state.wrapping_mul(1103515245).wrapping_add(12345)) & 2147483647;
        let key = (state / 65536) % key_range;
        sink = sink.wrapping_add(key);
    }
    println!("{sink}");
}
