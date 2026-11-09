// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
// noise_probe.c -- does the tuner invent notes from noise?
//
// Sensitivity lowers the coarse clarity gate (0.85 at the default, 0.65 at the top), and a lower
// gate lets noise through as a "note". This feeds the loaded .so noise only -- white, and a low,
// room-like noise (white through a one-pole low-pass at 300 Hz) -- at -30 and -50 dBFS, for 6 s
// each, at Sensitivity 0.5 and 1.0, and counts the blocks that publish voiced = 1.
// PASS (exit 0) iff no block is voiced.
//
// build: gcc -std=gnu11 -O2 -Itools tools/noise_probe.c -o noise_probe -ldl -lpthread -lm
// run:   noise_probe <plugin.so>

#include "hawk_rng.h"

#include <dlfcn.h>
#include <math.h>
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
#define SR    48000.0
#define BLOCK 256
enum { P_IN = 0, P_OUT = 1, P_NOTE = 4, P_VOICED = 8, P_SENS = 19, P_ENABLED = 22, P_N = 23 };

// Advances an absolute CLOCK_MONOTONIC deadline by ns nanoseconds and sleeps until it.
static void sleep_until_next(struct timespec* next, long ns)
{
    next->tv_nsec += ns;
    while (next->tv_nsec >= 1000000000L) { next->tv_nsec -= 1000000000L; ++next->tv_sec; }
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, next, NULL) != 0) {}
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
    long total_voiced = 0;
    printf("%s -- blocks publishing a note from noise alone (6 s = %d blocks each)\n", argv[1], (int)(6.0 * SR / BLOCK));
    const double senses[2] = { 0.5, 1.0 }, levels[2] = { -30.0, -50.0 };
    for (int si = 0; si < 2; ++si)
        for (int kind = 0; kind < 2; ++kind)
            for (int li = 0; li < 2; ++li) {
                const double sens = senses[si], dbfs = levels[li];
                void* h = d->instantiate(d, SR, "", NULL);
                float ctrl[P_N] = { 0.0f };
                ctrl[3] = -20.0f; ctrl[9] = 2.0f; ctrl[13] = 440.0f; ctrl[P_ENABLED] = 1.0f; ctrl[P_SENS] = (float)sens;
                for (int p = 2; p < P_N; ++p) d->connect_port(h, (uint32_t)p, &ctrl[p]);
                float in[BLOCK] = { 0.0f }, out[BLOCK] = { 0.0f };
                d->connect_port(h, P_IN, in);
                d->connect_port(h, P_OUT, out);
                d->activate(h);
                HawkMT rng;
                hawk_mt_seed(&rng, 11);
                HawkNormal g = hawk_normal(0.0, 1.0);
                const double a_lp = 1.0 - exp(-2.0 * M_PI * 300.0 / SR);
                const double rms = pow(10.0, dbfs / 20.0);
                double lp = 0.0, lp_rms = sqrt(a_lp / (2.0 - a_lp));   // rms of the one-pole output
                long voiced = 0; int notes[128] = { 0 };
                struct timespec next;
                clock_gettime(CLOCK_MONOTONIC, &next);
                const long blk = (long)(BLOCK * 1e9 / SR);
                for (long n0 = 0; n0 < (long)(6.0 * SR); n0 += BLOCK) {
                    for (int i = 0; i < BLOCK; ++i) {
                        const double w = hawk_normal_draw(&g, &rng);
                        lp += a_lp * (w - lp);
                        in[i] = (float)(rms * (kind == 0 ? w : lp / lp_rms));
                    }
                    d->run(h, (uint32_t)BLOCK);
                    if (ctrl[P_VOICED] > 0.5f) { ++voiced; const int nt = (int)ctrl[P_NOTE]; if (nt >= 0 && nt < 128) ++notes[nt]; }
                    sleep_until_next(&next, blk);
                }
                if (d->deactivate) d->deactivate(h);
                d->cleanup(h);
                int distinct = 0; for (int k = 0; k < 128; ++k) if (notes[k]) ++distinct;
                printf("  sensitivity %.1f  %-5s noise %4.0f dBFS: %5ld voiced blocks, %d distinct notes\n",
                       sens, kind == 0 ? "white" : "low", dbfs, voiced, distinct);
                total_voiced += voiced;
            }
    printf("%s\n", total_voiced == 0 ? "PASS: no note invented from noise." : "FAIL: notes published from noise alone.");
    return total_voiced == 0 ? 0 : 1;
}
