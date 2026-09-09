/* Characterising the ONE-PAIR-PER-STEP collapse for B-2026-09-05-22.
 *
 * `mt_malloc_k.c` at K=1 -- the density cell the earlier sweep never ran --
 * reproduces kara's two-worker collapse IN PURE C: 113.8 ms at one thread,
 * 949.0 ms at two, 49.2 ms at three. kara's compiled probes do exactly one
 * malloc/free pair per inner step (the `s + "x"` concat is dead-code eliminated
 * because only `t.len()` is read), while every earlier C control did two or
 * more -- which is why the control looked healthy where kara did not.
 *
 * This probe varies the three things that could matter, so the ceiling can be
 * stated precisely rather than as "one pair is bad":
 *
 *   pairs  -- blocks held live simultaneously per step (1 collapses, 2 does not)
 *   size   -- 0 = vary 1..8 like kara's substring, or a fixed byte count
 *   spin   -- iterations of a compute loop BETWEEN the malloc and the free,
 *             which decouples the two threads' timing without changing the
 *             allocation stream at all
 *   idle   -- extra threads that are alive but never allocate, separating
 *             "threads in the process" from "concurrent allocators" (kara's
 *             decouple2 collapses at pool=18 with two active workers, so the
 *             control must reproduce that independence too)
 *   split  -- give each allocating thread its own size class (16 << tid), so a
 *             shared per-size-class structure can be told from a shared global
 *             one
 *
 * Total allocation volume is held constant across every configuration, so only
 * the shape changes.
 *
 *   clang -O3 mt_pair1.c -o mt_pair1 -lpthread
 *   ./mt_pair1 <nthreads> [pairs=1] [size=0] [spin=0] [idle=0] [split=0]
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#define TOTAL_PAIRS 8000000L

static long g_steps_per_thread;
static int g_pairs;
static long g_size;
static long g_spin;
static int g_idle;
static int g_split;

/* Alive, parked, never allocating -- stays alive for exactly as long as the
 * allocating threads run, so the process's thread count is raised without
 * adding a second allocator. */
static volatile int g_done;

static void *idle_worker(void *arg) {
    (void)arg;
    struct timespec ts = {0, 1000 * 1000};
    while (!g_done) nanosleep(&ts, NULL);
    return NULL;
}

static void *worker(void *arg) {
    long tid = (long)(intptr_t)arg;
    long seed = tid + 1;
    long sink = 0;
    char *held[64];
    for (long i = 0; i < g_steps_per_thread; i++) {
        for (int j = 0; j < g_pairs; j++) {
            size_t n = g_split ? (size_t)(16L << (tid & 7))
                               : (g_size > 0 ? (size_t)g_size
                                             : (size_t)(seed % 8) + 1);
            char *p = (char *)malloc(n);
            if (!p) abort();
            memset(p, 'a', n);
            sink += (long)n;
            held[j] = p;
            seed = (seed * 7 + (long)n) % 100000;
        }
        /* Optional compute between allocate and free: same allocation stream,
         * different phase relationship between the threads. */
        for (long s = 0; s < g_spin; s++) sink += (sink >> 3) & 1;
        for (int j = 0; j < g_pairs; j++) free(held[j]);
    }
    return (void *)(intptr_t)sink;
}

int main(int argc, char **argv) {
    int nthreads = (argc > 1) ? atoi(argv[1]) : 1;
    g_pairs = (argc > 2) ? atoi(argv[2]) : 1;
    g_size = (argc > 3) ? atol(argv[3]) : 0;
    g_spin = (argc > 4) ? atol(argv[4]) : 0;
    g_idle = (argc > 5) ? atoi(argv[5]) : 0;
    g_split = (argc > 6) ? atoi(argv[6]) : 0;
    if (g_idle < 0) g_idle = 0;
    if (g_idle > 32) g_idle = 32;
    if (nthreads < 1) nthreads = 1;
    if (nthreads > 64) nthreads = 64;
    if (g_pairs < 1) g_pairs = 1;
    if (g_pairs > 64) g_pairs = 64;
    g_steps_per_thread = TOTAL_PAIRS / g_pairs / nthreads;

    pthread_t idle_tid[32];
    for (int t = 0; t < g_idle; t++)
        pthread_create(&idle_tid[t], NULL, idle_worker, NULL);

    pthread_t tid[64];
    long total = 0;
    for (int t = 0; t < nthreads; t++)
        pthread_create(&tid[t], NULL, worker, (void *)(intptr_t)t);
    for (int t = 0; t < nthreads; t++) {
        void *r;
        pthread_join(tid[t], &r);
        total += (long)(intptr_t)r;
    }
    g_done = 1;
    for (int t = 0; t < g_idle; t++) pthread_join(idle_tid[t], NULL);
    printf("sink %ld\n", total);
    return 0;
}
