/* Language-neutral control for B-2026-09-05-22.
 *
 * N threads, each allocating and freeing short-lived small blocks in a
 * tight loop with NO shared state whatsoever. If this shows the same
 * two-thread collapse the Kara probe does, the effect belongs to macOS
 * libmalloc (xzone) and not to Kara's runtime or its dispatch path.
 *
 *   clang -O3 mt_malloc.c -o mt_malloc -lpthread
 *   ./mt_malloc <nthreads>
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
