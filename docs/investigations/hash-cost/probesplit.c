/* Splitting kara's map lookup into hash and probe, under the REAL hash.
 *
 * The truncation differential in maptest.c replaced the hash with a weak mixer,
 * which changes the spread and therefore the probe-chain lengths -- so its
 * "probe ~30 cycles" was not safe to quote. This passes a PRE-COMPUTED SipHash
 * digest instead, so the walk runs against the real distribution while the
 * permutation is timed separately.
 *
 * RESULT (M5, 32M lookups, capacity 2048, 1024 live, load 0.50, chain 2.243):
 *
 *     loop floor                            5.7 instr   4.4 cyc   IPC 1.30
 *     hash only                            95.7        17.2       IPC 5.58
 *     probe only, byte-at-a-time           41.5        36.0       IPC 1.15
 *     probe only, 8-byte SWAR group scan   54.1        17.7       IPC 3.05
 *
 * The group scan is 2.03x faster in CYCLES while executing 30% MORE
 * instructions: the byte walk's cost is branch mispredicts, not work. Filed for
 * implementation as B-2026-09-09-12; the SWAR core is in README.md.
 *
 * The runtime entry points this drives are MEASUREMENT ONLY and are not in the
 * tree. To re-run, add to runtime/src/map.rs, beside `KaracMap::lookup`:
 *   karac_map_probe_i64_prehashed(map, key, hash, out)  -- lookup's walk, hash
 *                                                          supplied
 *   karac_map_probe_i64_group(map, key, hash, out)      -- same, SWAR group
 *   karac_map_probe_len_x1000(map, keys, n, hashes)     -- average chain length
 *   karac_map_capacity_probe(map)
 * then:
 *   cargo rustc -p karac-runtime --release --crate-type staticlib
 *   cc -O2 probesplit.c target/release/libkarac_runtime.a -o psplit -lc++ -lobjc
 *   KARAC_HASH_SEED=0x2a ./psplit stats
 *   KARAC_HASH_SEED=0x2a ./pmc ./psplit group
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

extern void *karac_map_new(size_t, size_t, uint64_t (*)(const void *),
                           int (*)(const void *, const void *));
extern int karac_map_insert_old(void *, const void *, const void *, void *, int, int);
extern int karac_map_get(const void *, const void *, void *);
extern int karac_map_probe_i64_prehashed(const void *, int64_t, uint64_t, int64_t *);
extern int karac_map_probe_i64_group(const void *, int64_t, uint64_t, int64_t *);
extern uint64_t karac_map_probe_len_x1000(const void *, const int64_t *, size_t, const uint64_t *);
extern uint64_t karac_map_capacity_probe(const void *);
extern uint64_t karac_hash_u64(uint64_t);

const void *KARAC_SPAWN_SITES = 0;
uint64_t KARAC_SPAWN_SITES_LEN = 0;
uint8_t KARAC_SPAWN_SITES_ENABLED = 0;

static uint64_t hash_i64(const void *p) { return karac_hash_u64(*(const int64_t *)p); }
static int eq_i64(const void *a, const void *b) {
    return *(const int64_t *)a == *(const int64_t *)b;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "full";
    long ops = argc > 2 ? atol(argv[2]) : 32000000L;
    const int64_t key_range = 4096, live = 1024;

    void *map = karac_map_new(8, 8, hash_i64, eq_i64);
    for (int64_t i = 0; i < live; i++) {
        int64_t k = i * 4, v = i, old = 0;
        karac_map_insert_old(map, &k, &v, &old, 0, 0);
    }

    /* Pre-hash the whole key range once, outside every measured loop. */
    static int64_t keys[4096];
    static uint64_t hashes[4096];
    for (int64_t k = 0; k < key_range; k++) {
        keys[k] = k;
        hashes[k] = karac_hash_u64((uint64_t)k);
    }

    /* The group scan must agree with the reference walk on every key. */
    {
        long bad = 0;
        for (int64_t k = 0; k < key_range; k++) {
            int64_t a = -1, b = -1;
            int fa = karac_map_probe_i64_prehashed(map, k, hashes[k], &a);
            int fb = karac_map_probe_i64_group(map, k, hashes[k], &b);
            if (fa != fb || (fa && a != b)) bad++;
        }
        if (bad) { fprintf(stderr, "GROUP MISMATCH on %ld keys\n", bad); return 1; }
    }

    if (strcmp(mode, "stats") == 0) {
        printf("capacity %llu  live %lld  load %.2f  avg probe steps %.3f\n",
               (unsigned long long)karac_map_capacity_probe(map), (long long)live,
               (double)live / (double)karac_map_capacity_probe(map),
               karac_map_probe_len_x1000(map, keys, key_range, hashes) / 1000.0);
        return 0;
    }

    int64_t sink = 0, state = 12345;
    if (strcmp(mode, "hash") == 0) {
        for (long t = 0; t < ops; t++) {
            state = (state * 1103515245 + 12345) & 2147483647;
            int64_t key = (state / 65536) % key_range;
            sink += (int64_t)(karac_hash_u64((uint64_t)key) & 0xff);
        }
    } else if (strcmp(mode, "probe") == 0) {
        for (long t = 0; t < ops; t++) {
            state = (state * 1103515245 + 12345) & 2147483647;
            int64_t key = (state / 65536) % key_range;
            int64_t v = -1;
            if (!karac_map_probe_i64_prehashed(map, key, hashes[key], &v)) v = -1;
            sink += v + 1;
        }
    } else if (strcmp(mode, "group") == 0) {
        for (long t = 0; t < ops; t++) {
            state = (state * 1103515245 + 12345) & 2147483647;
            int64_t key = (state / 65536) % key_range;
            int64_t v = -1;
            if (!karac_map_probe_i64_group(map, key, hashes[key], &v)) v = -1;
            sink += v + 1;
        }
    } else if (strcmp(mode, "loop") == 0) {
        for (long t = 0; t < ops; t++) {
            state = (state * 1103515245 + 12345) & 2147483647;
            int64_t key = (state / 65536) % key_range;
            sink += key;
        }
    } else {
        for (long t = 0; t < ops; t++) {
            state = (state * 1103515245 + 12345) & 2147483647;
            int64_t key = (state / 65536) % key_range;
            int64_t v = -1;
            if (!karac_map_get(map, &key, &v)) v = -1;
            sink += v + 1;
        }
    }
    printf("sink %lld\n", (long long)sink);
    return 0;
}
