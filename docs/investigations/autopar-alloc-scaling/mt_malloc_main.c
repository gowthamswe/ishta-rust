/* Second language-neutral control for B-2026-09-05-22.
 *
 * mt_malloc.c spawns N fresh pthreads and leaves main idle in pthread_join.
 * Kara's reduce dispatch does NOT do that: `sample` on the two-worker probe
 * shows __karac_reduce_worker_0 running on the main thread as well as on pool
 * threads, so kara's "two active workers" is really MAIN + ONE POOL THREAD.
 *
 * This control isolates exactly that difference: main performs one share of
 * the work itself and spawns only N-1 helpers, so N total allocators with the
 * main thread among them. Everything else — the allocation pattern, the size
 * distribution, the total allocation count — is identical to mt_malloc.c.
 *
 * If N=2 collapses HERE but not in mt_malloc.c, the trigger is the main
 * thread participating rather than the worker count, and it is a platform
 * property kara happens to step on. If N=2 is fine here too, the main thread
 * is exonerated and the trigger is something kara's runtime does.
 *
 *   clang -O3 mt_malloc_main.c -o mt_malloc_main -lpthread
 *   ./mt_malloc_main <n_total_allocators>
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define TOTAL_ALLOCS 4000000L

static long g_per_thread;

static void *worker(void *arg) {
    long seed = (long)(intptr_t)arg + 1;
    long sink = 0;
    for (long i = 0; i < g_per_thread; i++) {
        /* size varies 1..8 like the Kara probe's substring length */
        size_t n = (size_t)(seed % 8) + 1;
        char *p = (char *)malloc(n + 1);
        if (!p) abort();
        memset(p, 'a', n);
        p[n] = 0;
        sink += (long)n;
        char *q = (char *)malloc(n + 2);
        if (!q) abort();
        memcpy(q, p, n + 1);
        q[n] = 'x';
        q[n + 1] = 0;
        sink += (long)strlen(q);
        free(p);
        free(q);
        seed = (seed * 7 + (long)n) % 100000;
    }
    return (void *)(intptr_t)sink;
}

int main(int argc, char **argv) {
    int nthreads = (argc > 1) ? atoi(argv[1]) : 1;
    if (nthreads < 1) nthreads = 1;
    g_per_thread = TOTAL_ALLOCS / nthreads;

    pthread_t tid[64];
    long total = 0;

    /* Spawn N-1 helpers, then run share 0 on the MAIN thread — so main is one
     * of the N concurrent allocators, which is the shape kara's dispatch has. */
    for (int t = 1; t < nthreads; t++)
        pthread_create(&tid[t], NULL, worker, (void *)(intptr_t)t);

    total += (long)(intptr_t)worker((void *)(intptr_t)0);

    for (int t = 1; t < nthreads; t++) {
        void *r;
        pthread_join(tid[t], &r);
        total += (long)(intptr_t)r;
    }
    printf("sink %ld\n", total);
    return 0;
}
