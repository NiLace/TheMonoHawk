// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
// strobe_probe.c -- the strobe phase the plugin publishes must turn at the string's offset.
//
// A strobe display shows the phase of the string against the reference note: still when in tune,
// turning one way when sharp and the other when flat, at 2*pi*(f - f_ref) radians a second. This holds
// A2 (four partials) at 0, +2 and -3 cents, 3 s each, through the loaded .so at real-time pace, records
// the strobe_phase output on every voiced block, unwraps it and fits the slope over the last 1.5 s.
// Pass: within 5 % (+-0.05 rad/s) of 2*pi*(f - f_ref) for each detune, the in-tune one included.
// run: strobe_probe <plugin.so> [rate]      exit 0 = PASS
#include <dlfcn.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
enum { BLOCK = 256 };
enum { P_IN = 0, P_OUT = 1, P_VOICED = 8 };
enum { P_STROBE = 20, P_ENABLED = 22, P_N = 23 };

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <plugin.so> [rate]\n", argv[0]); return 2; }
    const double SR = argc > 2 ? atof(argv[2]) : 48000.0;
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
    const Desc* (*get)(uint32_t);
    *(void**)&get = dlsym(lib, "lv2_descriptor");   // POSIX's way to take a function from dlsym
    const Desc* d = get ? get(0) : NULL;
    if (!d) return 2;
    void* h = d->instantiate(d, SR, "", NULL);
    float ctrl[P_N] = { 0 };
    ctrl[3] = -12.0f; ctrl[9] = 2.0f; ctrl[13] = 440.0f; ctrl[P_ENABLED] = 1.0f; ctrl[19] = 0.5f;
    ctrl[P_STROBE] = NAN;                     // a plugin without the port leaves this alone
    for (int p = 2; p < P_N; ++p) d->connect_port(h, (uint32_t)p, &ctrl[p]);
    static float in[BLOCK], out[BLOCK];
    d->connect_port(h, P_IN, in);
    d->connect_port(h, P_OUT, out);
    d->activate(h);

    const double detune[] = { 0.0, +2.0, -3.0 };
    const double NOTE_S = 3.0, FIT_S = 1.5;
    int bad = 0;
    double ph[4] = { 0 };
    const size_t cap = (size_t)((long)(NOTE_S * SR) / BLOCK + 1);
    double* t = malloc(cap * sizeof *t);
    double* p = malloc(cap * sizeof *p);
    if (!t || !p) return 2;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    for (int ci = 0; ci < 3; ++ci) {
        const double c = detune[ci];
        const double f_ref = 110.0, f0 = f_ref * pow(2.0, c / 1200.0);
        const double want = 2 * M_PI * (f0 - f_ref);
        size_t n_fit = 0;
        const long len = (long)(NOTE_S * SR), fit_from = (long)((NOTE_S - FIT_S) * SR);
        for (long n0 = 0; n0 < len; n0 += BLOCK) {
            for (int i = 0; i < BLOCK; ++i) {
                double s = 0.0;
                for (int k = 0; k < 4; ++k) {
                    s += 0.25 / (k + 1) * sin(ph[k]);
                    ph[k] += 2 * M_PI * (k + 1) * f0 / SR;
                    if (ph[k] > 2 * M_PI) ph[k] -= 2 * M_PI;
                }
                in[i] = (float)s;
            }
            d->run(h, (uint32_t)BLOCK);
            const float v = ctrl[P_STROBE];
            if (n0 >= fit_from && ctrl[P_VOICED] > 0.5f && isfinite(v)) {
                double u = v;                                  // unwrap against the previous value
                if (n_fit > 0) {
                    while (u - p[n_fit - 1] >  M_PI) u -= 2 * M_PI;
                    while (u - p[n_fit - 1] < -M_PI) u += 2 * M_PI;
                }
                t[n_fit] = (double)n0 / SR;
                p[n_fit] = u;
                ++n_fit;
            }
            next.tv_nsec += (long)(BLOCK * 1e9 / SR);
            while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; ++next.tv_sec; }
            while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) != 0) {}
        }
        double slope = NAN;
        if (n_fit >= 5) {
            double st = 0, sp = 0, stt = 0, stp = 0;
            const double n = (double)n_fit;
            for (size_t k = 0; k < n_fit; ++k) { st += t[k]; sp += p[k]; stt += t[k] * t[k]; stp += t[k] * p[k]; }
            slope = (n * stp - st * sp) / (n * stt - st * st);
        }
        const bool ok = fabs(slope - want) <= 0.05 * fabs(want) + 0.05;
        if (!ok) ++bad;
        printf("%.0f Hz  A2 %+4.1f c: %zu blocks, slope %+7.3f rad/s, want %+7.3f %s\n", SR, c, n_fit,
               slope, want, ok ? "" : "<-- FAIL");
    }
    free(t);
    free(p);
    d->deactivate(h);
    d->cleanup(h);
    printf("%s\n", bad == 0 ? "PASS" : "FAIL");
    return bad == 0 ? 0 : 1;
}
