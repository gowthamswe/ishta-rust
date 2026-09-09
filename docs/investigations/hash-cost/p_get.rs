// Rust twin of p_get.kara — identical PRNG, identical key range, identical
// live-entry count, identical sink. `HashMap<i64,i64>` with the default hasher,
// which is the comparator kata:146's rust_ovf row uses.
use std::collections::HashMap;

fn main() {
    let key_range: i64 = 4096;
    let live: i64 = 1024;
    let ops: i64 = 32_000_000;

    let mut map: HashMap<i64, i64> = HashMap::new();
    for i in 0..live {
        map.insert(i * 4, i);
    }

    let mut sink: i64 = 0;
    let mut state: i64 = 12345;
    for _ in 0..ops {
        state = (state.wrapping_mul(1103515245).wrapping_add(12345)) & 2147483647;
        let key = (state / 65536) % key_range;
        let idx = *map.get(&key).unwrap_or(&-1);
        sink += idx + 1;
    }
    println!("{sink}");
}
