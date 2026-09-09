// LD_PRELOAD malloc/free counter + size histogram for B-2026-09-05-22.
//
// The macOS DYLD interposer this row records as ABANDONED hit two hazards
// (self-interposition recursion, and allocations before the constructor
// runs) and cost ~2500x once fixed. On glibc neither applies the same way:
// dlsym(RTLD_NEXT) resolves the real symbol, and the pre-constructor window
// is covered by a static bootstrap arena. Counters are THREAD-LOCAL and
// summed only at exit, so the hot path is one TLS increment -- no atomics,
// no lock, nothing that perturbs the contention behaviour under study.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

static void *(*real_malloc)(size_t);
static void  (*real_free)(void *);
static void *(*real_calloc)(size_t, size_t);
static void *(*real_realloc)(void *, size_t);

// Bootstrap arena: dlsym itself calls calloc on first use.
static char boot[1 << 16];
static size_t boot_off = 0;
static int in_boot(void *p) { return (char *)p >= boot && (char *)p < boot + sizeof boot; }

#define NBUCK 24            // bucket i = requests with 2^(i-1) < size <= 2^i
static __thread unsigned long t_malloc, t_free, t_calloc, t_realloc;
static __thread unsigned long t_hist[NBUCK];
static __thread unsigned long t_bytes;
static __thread int t_registered;

// Threads are few (<= 32 here) and register once, so a fixed table with a
// mutex costs nothing on the hot path.
#define MAXT 64
static struct { unsigned long *hist, *nm, *nf, *nc, *nr, *by; } reg[MAXT];
static int nreg;
static pthread_mutex_t reglock = PTHREAD_MUTEX_INITIALIZER;

static void reg_self(void) {
    t_registered = 1;
    pthread_mutex_lock(&reglock);
    if (nreg < MAXT) {
        reg[nreg].hist = t_hist; reg[nreg].nm = &t_malloc; reg[nreg].nf = &t_free;
        reg[nreg].nc = &t_calloc; reg[nreg].nr = &t_realloc; reg[nreg].by = &t_bytes;
        nreg++;
    }
    pthread_mutex_unlock(&reglock);
}

static void note(size_t n) {
    if (!t_registered) reg_self();
    int b = 0; size_t v = n;
    while (v && b < NBUCK - 1) { v >>= 1; b++; }
    t_hist[b]++;
    t_bytes += n;
}

static void init(void) {
    real_malloc  = dlsym(RTLD_NEXT, "malloc");
    real_calloc  = dlsym(RTLD_NEXT, "calloc");
    real_free    = dlsym(RTLD_NEXT, "free");
    real_realloc = dlsym(RTLD_NEXT, "realloc");
}

void *malloc(size_t n) {
    if (!real_malloc) init();
    t_malloc++; note(n);
    return real_malloc(n);
}
void *calloc(size_t c, size_t n) {
    if (!real_calloc) {
        // dlsym's own bootstrap allocation lands here.
        size_t need = (c * n + 15) & ~(size_t)15;
        if (boot_off + need <= sizeof boot) {
            void *p = boot + boot_off; boot_off += need; memset(p, 0, c * n); return p;
        }
        init();
    }
    t_calloc++; note(c * n);
    return real_calloc(c, n);
}
void *realloc(void *p, size_t n) {
    if (!real_realloc) init();
    if (in_boot(p)) { void *q = real_malloc(n); if (q) memcpy(q, p, n); return q; }
    t_realloc++; note(n);
    return real_realloc(p, n);
}
void free(void *p) {
    if (in_boot(p)) return;
    if (!real_free) init();
    if (p) t_free++;
    real_free(p);
}

static int dumped;
__attribute__((destructor)) static void dump(void) {
    if (dumped) return; dumped = 1;
    unsigned long hist[NBUCK] = {0}, nm = 0, nf = 0, nc = 0, nr = 0, by = 0;
    pthread_mutex_lock(&reglock);
    for (int i = 0; i < nreg; i++) {
        for (int b = 0; b < NBUCK; b++) hist[b] += reg[i].hist[b];
        nm += *reg[i].nm; nf += *reg[i].nf; nc += *reg[i].nc; nr += *reg[i].nr;
        by += *reg[i].by;
    }
    pthread_mutex_unlock(&reglock);
    char buf[4096]; int o = 0;
    o += snprintf(buf + o, sizeof buf - o,
                  "MCOUNT threads=%d malloc=%lu calloc=%lu realloc=%lu free=%lu bytes=%lu\n",
                  nreg, nm, nc, nr, nf, by);
    for (int b = 0; b < NBUCK; b++)
        if (hist[b] && o < (int)sizeof buf - 64)
            o += snprintf(buf + o, sizeof buf - o, "MCOUNT  size<=2^%-2d %12lu\n", b, hist[b]);
    ssize_t w = write(2, buf, o); (void)w;
}


