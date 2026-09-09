/* Fourth language-neutral control for B-2026-09-05-22 — the KARA-MEASURED shape.
 *
 * The three earlier controls (mt_malloc.c, mt_malloc_main.c, mt_malloc_k.c)
 * all guessed kara's allocation shape from the probe SOURCE: two Strings per
 * inner step, so two malloc/free pairs of 1..10 bytes. That guess is wrong,
 * and the counting probe the row records as ABANDONED is what shows it — on
 * Linux, where an LD_PRELOAD counter is tractable in a way the macOS DYLD
 * interposer was not (dlsym(RTLD_NEXT) + a static bootstrap arena for dlsym's
 * own calloc + THREAD-LOCAL counters summed at exit; no atomics on the hot
 * path, so the contention behaviour under study is not perturbed).
 *
 * MEASURED on decouple2 @ pool=2, 432000 inner steps:
 *
 *     malloc=432021  free=432016  bytes=1303647   -> 1.000 malloc/step, 3.0 B
 *     size histogram: 432000 requests in the 2^2 bucket, everything else < 30
 *
 * So kara does ONE malloc per step, not two, and every one of them is exactly
 * THREE BYTES.
 *
 * WHICH call allocates was measured, not inferred — three variants over the
 * same 100000-step loop with a loop-carried (non-foldable) length n = 1..4:
 *
 *     substring only                 malloc=100033  1 @ n bytes per step
 *     substring + one concat         malloc=100033  identical histogram
 *     substring + two concats        malloc=100033  identical histogram
 *
 * Byte-identical across all three, so the SUBSTRING's buffer is the only libc
 * allocation and the concats grow it without ever reaching the allocator (the
 * request is n bytes; malloc's usable size covers the appended bytes). Adding
 * concats to a kara loop does not add allocator traffic.
 *
 * One probe limitation this exposed, worth knowing before reading anything
 * into size-class effects: decouple2's `acc` reaches a fixed point at
 * acc % 8 == 2, so the intended 1..8 length spread collapses to a constant and
 * the probe exercises exactly ONE size class rather than eight.
 *
 * This control reproduces that measured shape exactly: one malloc + free of
 * SIZE bytes per step, N threads, no shared state. The size is an argument so
 * the size-class candidate can be swept rather than argued about.
 *
 *   clang -O3 mt_malloc_shape.c -o mt_malloc_shape -lpthread
 *   ./mt_malloc_shape <nthreads> [size_bytes]
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define TOTAL_ALLOCS 4000000L

static long g_per_thread;
static size_t g_size;

static void *worker(void *arg) {
    long seed = (long)(intptr_t)arg + 1;
    long sink = 0;
    for (long i = 0; i < g_per_thread; i++) {
        char *p = (char *)malloc(g_size);
        if (!p) abort();
        memset(p, 'a', g_size);
        p[g_size - 1] = 0;
        sink += (long)strlen(p);
        free(p);
        seed = (seed * 7 + 1) % 100000;
    }
    return (void *)(intptr_t)(sink + (seed & 1));
}

int main(int argc, char **argv) {
    int nthreads = (argc > 1) ? atoi(argv[1]) : 1;
    g_size = (argc > 2) ? (size_t)atol(argv[2]) : 3;
    if (nthreads < 1) nthreads = 1;
    if (nthreads > 64) nthreads = 64;
    if (g_size < 1) g_size = 1;
    g_per_thread = TOTAL_ALLOCS / nthreads;

    pthread_t tid[64];
    long total = 0;
    for (int t = 0; t < nthreads; t++)
        pthread_create(&tid[t], NULL, worker, (void *)(intptr_t)t);
    for (int t = 0; t < nthreads; t++) {
        void *r;
        pthread_join(tid[t], &r);
        total += (long)(intptr_t)r;
    }
    printf("sink %ld\n", total);
    return 0;
}
