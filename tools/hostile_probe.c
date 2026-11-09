// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
// hostile_probe.c -- what a misbehaving host, UI or signal chain can do to the loaded .so.
//
// Each case is one thing the plugin must survive, checked through the real plugin as a host drives it:
//   input-nan / input-inf   one non-finite input sample in the middle of a held note: the reading must
//                           come back within a second, and the level meter must not stay pinned;
//   dim-nan / dim-inf       one block with a non-finite Dim Amount: the output must be finite again
//                           at once, and back at the requested dim;
//   dim-over                Dim Amount above its TTL maximum (0 dB): the plugin must not amplify;
//   activate-twice          activate() twice without deactivate() in between (a host contract breach)
//                           must not abort the process;
//   rate                    a sample rate of 0, negative or absurd must make instantiate() fail
//                           cleanly (return NULL), not abort.
// run: hostile_probe <plugin.so> <case>      exit 0 = PASS
#include <dlfcn.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

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
#define SR    48000.0
#define BLOCK 256
enum { P_IN = 0, P_OUT = 1, P_ACTIVE = 2, P_DIM = 3, P_VOICED = 8, P_LEVEL = 18 };
enum { P_ENABLED = 22, P_N = 23 };

typedef struct Host {
    const Desc* d;
    void* h;
    float ctrl[P_N];
    float in[BLOCK], out[BLOCK];
    double ph[3];
    struct timespec next;
} Host;

static bool host_open(Host* H, const Desc* desc)
{
    memset(H, 0, sizeof *H);
    clock_gettime(CLOCK_MONOTONIC, &H->next);
    H->d = desc;
    H->h = H->d->instantiate(H->d, SR, "", NULL);
    if (!H->h) return false;
    H->ctrl[P_DIM] = -12.0f; H->ctrl[9] = 2.0f; H->ctrl[13] = 440.0f; H->ctrl[P_ENABLED] = 1.0f; H->ctrl[19] = 0.5f;
    for (int p = 2; p < P_N; ++p) H->d->connect_port(H->h, (uint32_t)p, &H->ctrl[p]);
    H->d->connect_port(H->h, P_IN, H->in);
    H->d->connect_port(H->h, P_OUT, H->out);
    return true;
}
// A held A2 (110 Hz, three partials, peak 0.4), one block of it, paced in real time so the
// worker sees the stream as it would in a host.
static void host_tone_block(Host* H)
{
    for (int i = 0; i < BLOCK; ++i) {
        double s = 0.0;
        for (int m = 1; m <= 3; ++m) {
            s += 0.2 / m * sin(H->ph[m - 1]);
            H->ph[m - 1] += 2 * M_PI * m * 110.0 / SR;
            if (H->ph[m - 1] > 2 * M_PI) H->ph[m - 1] -= 2 * M_PI;
        }
        H->in[i] = (float)s;
    }
}
static void host_run(Host* H)
{
    H->d->run(H->h, (uint32_t)BLOCK);
    H->next.tv_nsec += (long)(BLOCK * 1e9 / SR);
    while (H->next.tv_nsec >= 1000000000L) { H->next.tv_nsec -= 1000000000L; ++H->next.tv_sec; }
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &H->next, NULL) != 0) {}
}
static void host_close(Host* H)
{
    if (H->d->deactivate) H->d->deactivate(H->h);
    H->d->cleanup(H->h);
}

static int blocks(double seconds) { return (int)(seconds * SR / BLOCK); }

// One bad input sample at 1.5 s into a held note; afterwards the note must read again.
static int input_case(const Desc* d, float bad)
{
    Host H;
    if (!host_open(&H, d)) { printf("instantiate failed\n"); return 1; }
    d->activate(H.h);
    for (int b = 0; b < blocks(1.5); ++b) { host_tone_block(&H); host_run(&H); }
    host_tone_block(&H);
    H.in[BLOCK / 2] = bad;
    host_run(&H);
    for (int b = 0; b < blocks(1.0); ++b) { host_tone_block(&H); host_run(&H); }   // one second to recover
    int voiced = 0, n = 0;
    float level_max = 0.0f;
    for (int b = 0; b < blocks(2.0); ++b) {
        host_tone_block(&H); host_run(&H);
        ++n;
        if (H.ctrl[P_VOICED] > 0.5f) ++voiced;
        if (!(H.ctrl[P_LEVEL] <= level_max)) level_max = H.ctrl[P_LEVEL];
    }
    host_close(&H);
    // The tone peaks at ~0.33, so a level of 0.99 or more (or NaN) is the meter pinned by the bad sample.
    const bool ok = voiced >= n * 9 / 10 && level_max < 0.99f;
    printf("input %g: voiced %d of %d blocks after recovery, level max %.3f\n%s\n",
           (double)bad, voiced, n, (double)level_max, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// One block with a bad Dim Amount, dim engaged; then -12 dB again.
static int dim_case(const Desc* d, float bad, bool whole_run)
{
    Host H;
    if (!host_open(&H, d)) { printf("instantiate failed\n"); return 1; }
    H.ctrl[P_ACTIVE] = 1.0f;
    d->activate(H.h);
    for (int b = 0; b < blocks(0.3); ++b) { host_tone_block(&H); host_run(&H); }
    H.ctrl[P_DIM] = bad;
    host_tone_block(&H); host_run(&H);
    if (!whole_run) H.ctrl[P_DIM] = -12.0f;
    for (int b = 0; b < blocks(0.2); ++b) { host_tone_block(&H); host_run(&H); }   // the 10 ms smoother settles
    long nonfinite = 0;
    double gain_max = 0.0;
    for (int b = 0; b < blocks(0.5); ++b) {
        host_tone_block(&H); host_run(&H);
        for (int i = 0; i < BLOCK; ++i) {
            if (!isfinite(H.out[i])) { ++nonfinite; continue; }
            if (fabsf(H.in[i]) > 0.05f) {
                const double g = fabs((double)H.out[i] / H.in[i]);
                if (g > gain_max) gain_max = g;
            }
        }
    }
    host_close(&H);
    const double want = whole_run ? 1.0 : pow(10.0, -12.0 / 20.0);
    const bool ok = nonfinite == 0 && gain_max <= want * 1.001 && gain_max >= want * 0.999;
    printf("dim %g%s: %ld non-finite output samples, gain %.4f (want %.4f)\n%s\n", (double)bad,
           whole_run ? " held" : " for one block", nonfinite, gain_max, want, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

static int activate_twice(const Desc* d)
{
    Host H;
    if (!host_open(&H, d)) { printf("instantiate failed\n"); return 1; }
    d->activate(H.h);
    d->activate(H.h);                       // breach: no deactivate() in between
    for (int b = 0; b < blocks(0.5); ++b) { host_tone_block(&H); host_run(&H); }
    d->deactivate(H.h);
    d->deactivate(H.h);                     // and twice the other way
    d->activate(H.h);
    for (int b = 0; b < blocks(0.2); ++b) { host_tone_block(&H); host_run(&H); }
    host_close(&H);
    printf("activate twice, deactivate twice: survived\nPASS\n");
    return 0;
}

static int rate_case(const Desc* d)
{
    const double rates[] = { 0.0, -48000.0, (double)NAN, (double)INFINITY, 1.0e12 };
    int bad = 0;
    for (size_t i = 0; i < sizeof rates / sizeof rates[0]; ++i) {
        const double r = rates[i];
        void* h = d->instantiate(d, r, "", NULL);
        printf("rate %g: %s\n", r, h ? "instantiated (should have refused)" : "refused");
        if (h) { ++bad; d->cleanup(h); }
    }
    // and an ordinary rate still works
    void* h = d->instantiate(d, 44100.0, "", NULL);
    printf("rate 44100: %s\n", h ? "instantiated" : "REFUSED");
    if (!h) ++bad; else d->cleanup(h);
    printf("%s\n", bad == 0 ? "PASS" : "FAIL");
    return bad == 0 ? 0 : 1;
}

int main(int argc, char** argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <plugin.so> <case>\n", argv[0]); return 2; }
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
    const Desc* (*get)(uint32_t);
    *(void**)&get = dlsym(lib, "lv2_descriptor");   // POSIX's way to take a function from dlsym
    const Desc* d = get ? get(0) : NULL;
    if (!d) return 2;
    const float inf = INFINITY;
    const float nan = NAN;
    const char* c = argv[2];
    if (!strcmp(c, "input-nan"))      return input_case(d, nan);
    if (!strcmp(c, "input-inf"))      return input_case(d, inf);
    if (!strcmp(c, "dim-nan"))        return dim_case(d, nan, false);
    if (!strcmp(c, "dim-inf"))        return dim_case(d, inf, false);
    if (!strcmp(c, "dim-over"))       return dim_case(d, 20.0f, true);
    if (!strcmp(c, "activate-twice")) return activate_twice(d);
    if (!strcmp(c, "rate"))           return rate_case(d);
    fprintf(stderr, "unknown case %s\n", c);
    return 2;
}
