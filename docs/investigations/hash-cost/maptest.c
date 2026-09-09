/* The typed whole-lookup experiment for B-2026-09-07-53, and its result.
 *
 * BUILT, VALIDATED, MEASURED, AND NOT SHIPPED. A `karac_map_get_i64(map, key,
 * out)` that does the hash and the probe in ONE runtime body -- permutation
 * inlined into the walk, key never leaving a register, one call per lookup --
 * lands at 132.5 instr / 59.5 cycles, against the existing codegen mono probe's
 * 133.1 / 61.0. No win. The two probe functions it needs are therefore NOT in
 * the runtime; this file is kept because the negative result is worth more than
 * the code was.
 *
 * Measured on the M5, 32M lookups, 1024 live entries in a key range of 4096,
 * all four paths answering identically (the harness checks every key in the
 * range before timing, and every mode prints sink 4101321192):
 *
 *     kara  erased  karac_map_get (fn ptrs, key by address)  163.5 instr  71.9 cyc
 *     kara  codegen mono probe + stored hash_fn              133.1        61.0
 *     kara  typed   karac_map_get_i64                        132.5        59.5
 *     kara  keyed   ...with the seed passed in                115.5        58.3
 *     rust  hashbrown behind #[inline(never)]                 114.9        35.6
 *     rust  hashbrown fully inlined                            81.0        32.3
 *
 * WHAT THE TABLE SAYS. At 115.5 against 114.9 kara reaches INSTRUCTION PARITY
 * with Rust's lookup in the same call shape -- and is still 1.64x its cycles
 * (58.3 vs 35.6), IPC 1.98 against 3.23. The remaining gap is STALLS, not work,
 * so nothing that shaves instructions reaches it: not the seed hoist (17 instr
 * but 1.2 cycles), not the typed entry point, not open-coding the permutation.
 *
 * The seed read is also re-measured here and the earlier 0.46-cycle figure
 * SURVIVES. The suspicion it was an artifact -- its `ldapr` acquire load costing
 * nothing in a microbenchmark whose iterations were already serialised -- was
 * itself wrong: in this loop, whose lookups ARE independent, removing the
 * acquire still buys only 1.2 cycles.
 *
 * TO RE-RUN, add to runtime/src/map.rs a `karac_map_get_i64` that inlines
 * `karac_hash::hash_u64` into the probe body of `KaracMap::lookup` (and a
 * `_keyed` variant taking k0/k1 for the seed arm), plus a `karac_hash_seed_pair`
 * in hashing.rs. Then:
 *
 *   cargo rustc -p karac-runtime --release --crate-type staticlib
 *   cc -O2 maptest.c target/release/libkarac_runtime.a -o maptest -lc++ -lobjc
 *   KARAC_HASH_SEED=0x2a ./maptest check          # all 4096 keys must agree
 *   KARAC_HASH_SEED=0x2a ./pmc ./maptest typed
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

extern void *karac_map_new(size_t key_size, size_t val_size,
                           uint64_t (*hash_fn)(const void *),
                           int (*eq_fn)(const void *, const void *));
extern int karac_map_insert_old(void *map, const void *key, const void *val,
                                void *out_old_val, int drop_key, int drop_val);
extern int karac_map_get(const void *map, const void *key, void *out_val);
extern int karac_map_get_i64(const void *map, int64_t key, int64_t *out_val);
extern int karac_map_get_i64_keyed(const void *map, int64_t key, uint64_t k0,
                                   uint64_t k1, int64_t *out_val);
extern void karac_hash_seed_pair(uint64_t *k0, uint64_t *k1);
extern uint64_t karac_hash_u64(uint64_t v);

const void *KARAC_SPAWN_SITES = 0;
uint64_t KARAC_SPAWN_SITES_LEN = 0;
uint8_t KARAC_SPAWN_SITES_ENABLED = 0;

/* Exactly the shape codegen emits for an i64 key: load, then the typed hash. */
static uint64_t hash_i64(const void *p) { return karac_hash_u64(*(const int64_t *)p); }
static int eq_i64(const void *a, const void *b) {
    return *(const int64_t *)a == *(const int64_t *)b;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "typed";
    long ops = argc > 2 ? atol(argv[2]) : 32000000L;
    const int64_t key_range = 4096, live = 1024;

    void *map = karac_map_new(8, 8, hash_i64, eq_i64);
    for (int64_t i = 0; i < live; i++) {
        int64_t k = i * 4, v = i, old = 0;
        karac_map_insert_old(map, &k, &v, &old, 0, 0);
    }

    /* Correctness: every key in the range must agree across the two paths. */
    long disagreements = 0;
    for (int64_t k = 0; k < key_range; k++) {
        int64_t a = -1, b = -1;
        int fa = karac_map_get(map, &k, &a);
        int fb = karac_map_get_i64(map, k, &b);
        if (fa != fb || (fa && a != b)) disagreements++;
    }
    if (disagreements) {
        fprintf(stderr, "MISMATCH on %ld keys\n", disagreements);
        return 1;
    }
    if (strcmp(mode, "check") == 0) {
        printf("agree on all %lld keys\n", (long long)key_range);
        return 0;
    }

    /* The mode decision is hoisted OUT of the loop: a strcmp per iteration
     * would be charged to both arms and buries what is being measured. */
    int erased = strcmp(mode, "erased") == 0;
    int keyed = strcmp(mode, "keyed") == 0;
    uint64_t k0 = 0, k1 = 0;
    karac_hash_seed_pair(&k0, &k1);
    int64_t sink = 0, state = 12345;
    if (keyed) {
        for (long t = 0; t < ops; t++) {
            state = (state * 1103515245 + 12345) & 2147483647;
            int64_t key = (state / 65536) % key_range;
            int64_t v = -1;
            if (!karac_map_get_i64_keyed(map, key, k0, k1, &v)) v = -1;
            sink += v + 1;
        }
    } else if (erased) {
        for (long t = 0; t < ops; t++) {
            state = (state * 1103515245 + 12345) & 2147483647;
            int64_t key = (state / 65536) % key_range;
            int64_t v = -1;
            if (!karac_map_get(map, &key, &v)) v = -1;
            sink += v + 1;
        }
    } else {
        for (long t = 0; t < ops; t++) {
            state = (state * 1103515245 + 12345) & 2147483647;
            int64_t key = (state / 65536) % key_range;
            int64_t v = -1;
            if (!karac_map_get_i64(map, key, &v)) v = -1;
            sink += v + 1;
        }
    }
    printf("sink %lld\n", (long long)sink);
    return 0;
}
