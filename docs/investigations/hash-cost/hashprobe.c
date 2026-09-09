/* Per-hash cost of kara's integer hash, in the shape the map actually uses.
 *
 * B-2026-09-07-53 sizes the hash in INSTRUCTIONS on x86 and its own rule says
 * to size anything on the M5 in CYCLES. This is the cycles harness, and its
 * twin `rusthash.rs` is the comparator. Both drive the same loop shape, so the
 * numbers difference cleanly against each other.
 *
 * kara's map does NOT inline its hash: `KaracMap` calls a codegen-emitted
 * `hash_fn` through a FUNCTION POINTER, so nothing about the call can be
 * hoisted, CSE'd or folded. This probe calls through a volatile pointer for
 * exactly that reason -- measuring an inlined hash would flatter kara by
 * measuring a path no map takes.
 *
 * BUILD (macOS; the runtime archive must exist):
 *   cargo rustc -p karac-runtime --release --crate-type staticlib
 *   cc -O2 hashprobe.c target/release/libkarac_runtime.a -o hashprobe -lc++ -lobjc
 *   cc -O2 -o pmc scripts/pmc.c                # exact instr/cycle counts
 *   KARAC_HASH_SEED=0x2a ./pmc ./hashprobe seed 20000000
 *
 * THE `key` MODE IS OPT-IN and needs three functions the runtime deliberately
 * does NOT carry -- B-2026-09-07-53 measured what they would buy and the answer
 * was 13 instructions but 0.46 CYCLES per hash, so they were reverted rather
 * than shipped as dead weight. To re-measure (e.g. on x86, where the cycle cost
 * is still unmeasured), add to runtime/src/hashing.rs:
 *
 *   #[no_mangle] pub unsafe extern "C"
 *   fn karac_hash_u64_with_key(v: u64, k0: u64, k1: u64) -> u64 {
 *       karac_hash::hash_int_with_key(v, 8, k0, k1)
 *   }
 *   #[no_mangle] pub unsafe extern "C"
 *   fn karac_hash_seed_pair(out_k0: *mut u64, out_k1: *mut u64) {
 *       let (k0, k1) = karac_hash::seed();
 *       unsafe { *out_k0 = k0; *out_k1 = k1; }
 *   }
 *
 * and build this with -DKARAC_HASH_WITH_KEY.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

extern uint64_t karac_hash_u64(uint64_t v);
#ifdef KARAC_HASH_WITH_KEY
extern uint64_t karac_hash_u64_with_key(uint64_t v, uint64_t k0, uint64_t k1);
extern void karac_hash_seed_pair(uint64_t *k0, uint64_t *k1);
#endif

/* Codegen normally stamps these; a hand-written C main must supply them. */
const void *KARAC_SPAWN_SITES = 0;
uint64_t KARAC_SPAWN_SITES_LEN = 0;
uint8_t KARAC_SPAWN_SITES_ENABLED = 0;

typedef uint64_t (*fp_seed)(uint64_t);

/* volatile so the indirection survives -O2, exactly as a map's stored
 * `hash_fn` pointer does. */
static volatile fp_seed g_seed = karac_hash_u64;
#ifdef KARAC_HASH_WITH_KEY
typedef uint64_t (*fp_key)(uint64_t, uint64_t, uint64_t);
static volatile fp_key g_key = karac_hash_u64_with_key;
#endif

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "seed";
    long iters = argc > 2 ? atol(argv[2]) : 20000000L;
    uint64_t acc = 0;

    if (strcmp(mode, "key") == 0) {
#ifdef KARAC_HASH_WITH_KEY
        /* The map would read its pair ONCE, at construction. */
        uint64_t k0, k1;
        karac_hash_seed_pair(&k0, &k1);
        fp_key f = g_key;
        for (long i = 0; i < iters; i++) acc += f((uint64_t)i ^ acc, k0, k1);
#else
        fprintf(stderr, "build with -DKARAC_HASH_WITH_KEY (see the header)\n");
        return 2;
#endif
    } else if (strcmp(mode, "seed") == 0) {
        fp_seed f = g_seed;
        for (long i = 0; i < iters; i++) acc += f((uint64_t)i ^ acc);
    } else {
        fprintf(stderr, "mode must be seed|key\n");
        return 2;
    }
    printf("sink %llu\n", (unsigned long long)acc);
    return 0;
}
