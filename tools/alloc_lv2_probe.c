// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
// alloc_lv2_probe.c -- heap allocations of the WHOLE plugin, worker thread included, while it
// reads a strummed chord.
//
// alloc_probe.c counts analyze() and refine() in isolation; the worker loop lives in the LV2
// wrapper and never runs there. This one loads the .so the way a host does and interposes the
// allocator itself -- malloc, calloc, realloc and the aligned forms: the plugin's calls resolve to the
// executable's definitions, so every allocation made by any thread of the plugin is counted (a C++
// plugin's operator new takes its memory from malloc, so it is counted too). Link with -rdynamic or
// they will not.
//
// The signal is a synthetic strum in Drop D (tuning index 1) -- six strings with a stretched,
// decaying partial series, re-struck every 2 s.
// The first STRUM_WARM seconds are not counted (FFT plans and first-touch buffers).
//
// build: gcc -std=gnu11 -O2 -rdynamic tools/alloc_lv2_probe.c -o alloc_lv2_probe -ldl -lpthread -lm
// run:   alloc_lv2_probe <plugin.so>      (exit 0 = no allocation counted; a test in test/meson.build)

#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

// glibc's own entry points: the definitions below count, then hand the request on to them.
extern void* __libc_malloc(size_t n);
extern void* __libc_calloc(size_t n, size_t size);
extern void* __libc_realloc(void* p, size_t n);
extern void* __libc_memalign(size_t align, size_t n);
extern void  __libc_free(void* p);

static atomic_long g_new = 0, g_bytes = 0;
static atomic_bool g_count = false;

static void count(size_t n)
{
    if (atomic_load_explicit(&g_count, memory_order_relaxed)) {
        atomic_fetch_add(&g_new, 1);
        atomic_fetch_add(&g_bytes, (long)n);
    }
}

void* malloc(size_t n)                { count(n); return __libc_malloc(n); }
void* calloc(size_t n, size_t size)   { count(n * size); return __libc_calloc(n, size); }
void* realloc(void* p, size_t n)      { count(n); return __libc_realloc(p, n); }
void* memalign(size_t align, size_t n)      { count(n); return __libc_memalign(align, n); }
void* aligned_alloc(size_t align, size_t n) { count(n); return __libc_memalign(align, n); }
int posix_memalign(void** p, size_t align, size_t n)
{
    if (align < sizeof(void*) || (align & (align - 1)) != 0) return EINVAL;
    count(n);
    void* q = __libc_memalign(align, n);
    if (!q) return ENOMEM;
    *p = q;
    return 0;
}
void free(void* p) { __libc_free(p); }

typedef struct Desc {
    const char* URI;
    void* (*instantiate)(const void*, double, const char*, const void* const*);
    void  (*connect_port)(void*, uint32_t, void*);
    void  (*activate)(void*);
    void  (*run)(void*, uint32_t);
    void  (*deactivate)(void*);
    void  (*cleanup)(void*);
    const void* (*extension_data)(const char*);
} Desc;

static const double SR         = 48000.0;
enum { BLOCK = 256 };
static const double SECONDS    = 12.0;
static const double STRUM_WARM = 3.0;
enum { P_IN = 0, P_OUT = 1, P_TUNING = 14, P_ENABLED = 22, P_N = 23 };   // from lv2/hawk.ttl

static void sleep_us(long us)
{
    struct timespec ts = { us / 1000000, (us % 1000000) * 1000 };
    while (nanosleep(&ts, &ts) != 0) {}   // interrupted: sleep what is left
}

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <plugin.so>\n", argv[0]); return 2; }
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
    const Desc* (*get)(uint32_t);
    *(void**)&get = dlsym(lib, "lv2_descriptor");   // POSIX's way to take a function from dlsym
    const Desc* d = get ? get(0) : NULL;
    if (!d) { fprintf(stderr, "no descriptor\n"); return 2; }

    void* h = d->instantiate(d, SR, "", NULL);
    float ctrl[P_N] = { 0 };
    ctrl[3]  = -20.0f;  // dim dB
    ctrl[9]  = 2.0f;    // in-tune tolerance
    ctrl[13] = 440.0f; ctrl[P_ENABLED] = 1.0f;  // A4
    ctrl[P_TUNING] = 1.0f;  // Drop D
    ctrl[19] = 0.5f;    // sensitivity
    for (int p = 2; p < P_N; ++p) d->connect_port(h, (uint32_t)p, &ctrl[p]);
    float in[BLOCK] = { 0 }, out[BLOCK] = { 0 };
    d->connect_port(h, P_IN, in);
    d->connect_port(h, P_OUT, out);
    d->activate(h);

    // Drop D, low to high: D2 A2 D3 G3 B3 E4 (MIDI 38 45 50 55 59 64).
    const int midi[6] = { 38, 45, 50, 55, 59, 64 };
    const double B = 1.0e-4;   // inharmonicity, as on a real string
    double phase[6][8] = { { 0 } };
    const long total = (long)(SECONDS * SR);
    for (long n0 = 0; n0 < total; n0 += BLOCK) {
        if (!atomic_load(&g_count) && n0 >= (long)(STRUM_WARM * SR)) atomic_store(&g_count, true);
        for (int i = 0; i < BLOCK; ++i) {
            const double t  = fmod((double)(n0 + i) / SR, 2.0);   // re-strum every 2 s
            const double env = exp(-1.5 * t);
            double s = 0.0;
            for (int k = 0; k < 6; ++k) {
                const double f0 = 440.0 * pow(2.0, (midi[k] - 69) / 12.0);
                for (int m = 1; m <= 8; ++m) {
                    const double fm = m * f0 * sqrt(1.0 + B * m * m);
                    s += env * (0.08 / m) * sin(phase[k][m - 1]);
                    phase[k][m - 1] += 2.0 * M_PI * fm / SR;
                    if (phase[k][m - 1] > 2.0 * M_PI) phase[k][m - 1] -= 2.0 * M_PI;
                }
            }
            in[i] = (float)s;
        }
        d->run(h, (uint32_t)BLOCK);
        // real time, so the worker analyses at the rate it would in a host
        sleep_us((int)(BLOCK * 1e6 / SR));
    }
    atomic_store(&g_count, false);
    const long n_alloc = atomic_load(&g_new), n_bytes = atomic_load(&g_bytes);
    if (d->deactivate) d->deactivate(h);
    d->cleanup(h);

    const double secs = SECONDS - STRUM_WARM;
    printf("%s\n  strummed Drop D, %.0f s counted after %.0f s of warm-up\n", argv[1], secs, STRUM_WARM);
    printf("  allocations: %ld  (%.1f /s)\n", n_alloc, n_alloc / secs);
    printf("  bytes      : %ld  (%.3f MB/s)\n", n_bytes, n_bytes / secs / 1e6);
    printf("\n%s\n", n_alloc == 0
           ? "PASS: no heap traffic in steady state, worker thread included."
           : "FAIL: the plugin allocates while it runs.");
    return n_alloc == 0 ? 0 : 1;
}
