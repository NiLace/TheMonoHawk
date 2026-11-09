// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
// boundary_probe.c -- a string near the boundary between two notes is read as the note it is nearest.
//
// Two cases through the loaded .so at real-time pace, A4 = 440:
//   sweep -- E3 swept from +20 c to +80 c and back at 20 c/s (into F and out): no published (note, cents) pair may sit
//            more than 30 c from the input's pitch at that moment (the analysis lags ~0.2 s, ~4 c at this rate). The
//            fault this guards: the cents median carrying the old note's +49 under the new note -- 100 c off.
//   noisy -- E3 +52 c (= F3 -48 c) held 3 s in white noise ~8 dB below the string: every voiced block reads |cents| <= 50.
// PASS (exit 0) iff both hold. Negative arm: the .so built with -DHAWK_OLD_NOTE_SNAP=1 must FAIL (test/meson.build).
// run: boundary_probe <plugin.so>
#include "hawk_rng.h"

#include <dlfcn.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
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
#define SR 48000.0
enum { BLOCK = 256 };
enum { P_IN = 0, P_OUT = 1, P_NOTE = 4, P_CENTS = 5, P_VOICED = 8, P_TOL = 9 };
enum { P_ENABLED = 22, P_N = 23 };
#define E3 164.8138   // MIDI 52 at A4 = 440

static const Desc* d;
static HawkMT     rng;
static HawkNormal noise;

// The input's offset from E3, in cents.
static double cents_at(bool sweep, double t)
{
    if (!sweep) return 52.0;
    if (t < 1.0) return 20.0;
    const double u = t - 1.0;                        // 20 -> 80 in 3 s, back in 3 s
    return u < 3.0 ? 20.0 + 20.0 * u : 80.0 - 20.0 * (u - 3.0);
}

static void run_case(bool sweep, double* worst_off, double* worst_abs, long* voiced_blocks)
{
    void* h = d->instantiate(d, SR, "", NULL);
    float ctrl[P_N] = { 0 };
    ctrl[3] = -12.0f; ctrl[P_TOL] = 2.0f; ctrl[13] = 440.0f; ctrl[P_ENABLED] = 1.0f; ctrl[19] = 0.5f;
    for (int p = 2; p < P_N; ++p) d->connect_port(h, (uint32_t)p, &ctrl[p]);
    float in[BLOCK] = { 0 }, out[BLOCK] = { 0 };
    d->connect_port(h, P_IN, in);
    d->connect_port(h, P_OUT, out);
    d->activate(h);
    const double dur = sweep ? 7.0 : 4.0;
    double ph[8] = { 0 };
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    const long blk = (long)(BLOCK * 1e9 / SR);
    for (long n0 = 0; n0 < (long)(dur * SR); n0 += BLOCK) {
        for (int i = 0; i < BLOCK; ++i) {
            const double t = (double)(n0 + i) / SR;
            const double f = E3 * pow(2.0, cents_at(sweep, t) / 1200.0);
            double s = 0.0;
            for (int m = 1; m <= 8; ++m) {
                s += 0.3 / m * sin(ph[m - 1]);
                ph[m - 1] += 2 * M_PI * m * f / SR;
                if (ph[m - 1] > 2 * M_PI) ph[m - 1] -= 2 * M_PI;
            }
            if (!sweep) s += 0.10 * hawk_normal_draw(&noise, &rng);   // ~8 dB below the string (rms 0.26)
            in[i] = (float)s;
        }
        d->run(h, (uint32_t)BLOCK);
        const double t = (double)(n0 + BLOCK) / SR;
        if (ctrl[P_VOICED] > 0.5f && t > 0.6) {
            ++*voiced_blocks;
            const double published = (ctrl[P_NOTE] - 52.0) * 100.0 + ctrl[P_CENTS];   // cents from E3
            double off = 1e9;                       // nearest input pitch within the analysis's lag (0..0.5 s)
            for (double lag = 0.0; lag <= 0.5; lag += 0.01)
                off = fmin(off, fabs(published - cents_at(sweep, fmax(0.0, t - lag))));
            *worst_off = fmax(*worst_off, off);
            *worst_abs = fmax(*worst_abs, fabs((double)ctrl[P_CENTS]));
        }
        next.tv_nsec += blk;
        while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; ++next.tv_sec; }
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) != 0) {}
    }
    d->deactivate(h);
    d->cleanup(h);
}

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <plugin.so>\n", argv[0]); return 2; }
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
    const Desc* (*get)(uint32_t);
    *(void**)&get = dlsym(lib, "lv2_descriptor");   // POSIX's way to take a function from dlsym
    d = get ? get(0) : NULL;
    if (!d) return 2;
    hawk_mt_seed(&rng, 11);
    noise = hawk_normal(0.0, 1.0);
    double s_off = 0, s_abs = 0, n_off = 0, n_abs = 0;
    long s_v = 0, n_v = 0;
    run_case(true, &s_off, &s_abs, &s_v);
    run_case(false, &n_off, &n_abs, &n_v);
    const bool ok = s_v > 100 && n_v > 100 && s_off <= 30.0 && n_abs <= 50.0;
    printf("sweep across E3/F3: %ld voiced blocks, worst pair %.1f c off the input (want <= 30)\n", s_v, s_off);
    printf("E3 +52 c in noise:  %ld voiced blocks, worst |cents| %.2f (want <= 50)\n%s\n", n_v, n_abs,
           ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
