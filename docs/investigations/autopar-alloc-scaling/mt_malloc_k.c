/* Third control for B-2026-09-05-22: does ALLOCATION DENSITY trigger it?
 *
 * mt_malloc.c does two malloc + two free per step and speeds up at two
 * threads. The open question is whether kara collapses because it allocates
 * MORE per step. Rather than counting kara's rate (a DYLD interposer made the
 * program ~2500x slower and was abandoned), this sweeps the C control's rate
 * directly: K malloc/free PAIRS per step, total allocation volume held constant
 * so only the density per step changes.
 *
 * If C still speeds up at two threads for every K, allocation density is not
 * sufficient to trigger the collapse and the difference must be in the size
 * class or the free path rather than the count.
 *
 *   clang -O3 mt_malloc_k.c -o mt_malloc_k -lpthread
 *   ./mt_malloc_k <nthreads> <pairs_per_step>
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define TOTAL_PAIRS 8000000L

static long g_steps_per_thread;
static int g_k;

static void *worker(void *arg) {
    long seed = (long)(intptr_t)arg + 1;
    long sink = 0;
    char *held[64];
    for (long i = 0; i < g_steps_per_thread; i++) {
        for (int j = 0; j < g_k; j++) {
            size_t n = (size_t)(seed % 8) + 1;
            char *p = (char *)malloc(n + 1);
            if (!p) abort();
            memset(p, 'a', n);
            p[n] = 0;
            sink += (long)n;
            held[j] = p;
            seed = (seed * 7 + (long)n) % 100000;
        }
        for (int j = 0; j < g_k; j++) free(held[j]);
    }
    return (void *)(intptr_t)sink;
}

int main(int argc, char **argv) {
    int nthreads = (argc > 1) ? atoi(argv[1]) : 1;
    g_k = (argc > 2) ? atoi(argv[2]) : 2;
    if (nthreads < 1) nthreads = 1;
    if (g_k < 1) g_k = 1;
    if (g_k > 64) g_k = 64;
    /* hold total allocations constant across K so only density changes */
    g_steps_per_thread = TOTAL_PAIRS / g_k / nthreads;

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
