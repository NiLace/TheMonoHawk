// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
// rate_probe.c -- the chromatic reading at the host's sample rate.
//
// Every other probe runs at 48 kHz, and the analysis does not run at the host's rate: above 50 kHz
// the input is decimated (88.2/96 kHz by 2, 176.4/192 kHz by 4). This plays, at one rate, a held note
// (four partials) for each of B0, E2, A2, D3, G3, B3, E4, each detuned by +10 c and then by -7 c, through
// the loaded .so at real-time pace, and reads the published note and cents over the last 0.5 s of
// each. Pass: the right note in at least 90 % of those blocks, and their median cents within 0.3 c of
// the detune.
// With "sweep" as third argument it is the high-notes sweep instead: E2 to F#6 (82-1480 Hz) every three semitones, the 2nd partial at -6 dB.
// run: rate_probe <plugin.so> <rate> [sweep]      exit 0 = PASS
#include <dlfcn.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
enum { BLOCK = 256 };
enum { P_IN = 0, P_OUT = 1, P_NOTE = 4, P_CENTS = 5, P_VOICED = 8 };
enum { P_ENABLED = 22, P_N = 23 };

// Advances an absolute deadline on the monotonic clock by ns and sleeps until it.
static void sleep_until_next(struct timespec* next, long ns)
{
    next->tv_nsec += ns;
    while (next->tv_nsec >= 1000000000L) { next->tv_nsec -= 1000000000L; ++next->tv_sec; }
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, next, NULL) != 0) {}
}

static int cmp_double(const void* a, const void* b)
{
    const double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}

int main(int argc, char** argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <plugin.so> <rate> [sweep]\n", argv[0]); return 2; }
    const double SR = atof(argv[2]);
    const int sweep = argc > 3 && strcmp(argv[3], "sweep") == 0;
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
    const Desc* (*get)(uint32_t);
    *(void**)&get = dlsym(lib, "lv2_descriptor");   // POSIX's way to take a function from dlsym
    const Desc* d = get ? get(0) : NULL;
    if (!d) return 2;
    void* h = d->instantiate(d, SR, "", NULL);
    if (!h) { printf("instantiate refused %.0f Hz\nFAIL\n", SR); return 1; }
    float ctrl[P_N] = { 0 };
    ctrl[3] = -12.0f; ctrl[9] = 2.0f; ctrl[13] = 440.0f; ctrl[P_ENABLED] = 1.0f; ctrl[19] = 0.5f;
    for (int p = 2; p < P_N; ++p) d->connect_port(h, (uint32_t)p, &ctrl[p]);
    static float in[BLOCK], out[BLOCK];
    d->connect_port(h, P_IN, in);
    d->connect_port(h, P_OUT, out);
    d->activate(h);

    int midi[32] = { 23, 40, 45, 50, 55, 59, 64 };
    int n_midi = 7;
    if (sweep) { n_midi = 0; for (int m = 40; m <= 88; m += 3) midi[n_midi++] = m; midi[n_midi++] = 90; }
    // partial amplitudes: 1/k, or for the sweep the 2nd at -6 dB and a 3rd and 4th below it
    const double amp_default[4] = { 0.25, 0.125, 0.0833, 0.0625 };
    const double amp_sweep[4]   = { 0.25, 0.125, 0.06, 0.03 };
    const double* amp = sweep ? amp_sweep : amp_default;
    const double detune[] = { +10.0, -7.0 };
    const double NOTE_S = 1.4, JUDGE_S = 0.5;
    int bad = 0;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    double ph[4] = { 0 };
    const long len_max = (long)(NOTE_S * SR) / BLOCK + 1;
    double* got = malloc((size_t)len_max * sizeof *got);
    if (!got) return 2;
    for (int mi = 0; mi < n_midi; ++mi)
        for (int ci = 0; ci < 2; ++ci) {
            const int m = midi[mi];
            const double c = detune[ci];
            const double f0 = 440.0 * pow(2.0, (m - 69 + c / 100.0) / 12.0);
            const long len = (long)(NOTE_S * SR), judge_from = (long)((NOTE_S - JUDGE_S) * SR);
            size_t n_got = 0;
            long blocks = 0, right = 0;
            for (long n0 = 0; n0 < len; n0 += BLOCK) {
                for (int i = 0; i < BLOCK; ++i) {
                    double s = 0.0;
                    for (int k = 0; k < 4; ++k) {
                        s += amp[k] * sin(ph[k]);
                        ph[k] += 2 * M_PI * (k + 1) * f0 / SR;
                        if (ph[k] > 2 * M_PI) ph[k] -= 2 * M_PI;
                    }
                    in[i] = (float)s;
                }
                d->run(h, (uint32_t)BLOCK);
                if (n0 >= judge_from) {
                    ++blocks;
                    if (ctrl[P_VOICED] > 0.5f && (int)lroundf(ctrl[P_NOTE]) == m) {
                        ++right;
                        got[n_got++] = ctrl[P_CENTS];
                    }
                }
                sleep_until_next(&next, (long)(BLOCK * 1e9 / SR));
            }
            double med = NAN;
            if (n_got > 0) {
                qsort(got, n_got, sizeof *got, cmp_double);
                med = got[n_got / 2];
            }
            const int ok = right * 10 >= blocks * 9 && fabs(med - c) <= 0.3;
            if (!ok) ++bad;
            printf("%.0f Hz  midi %d %+5.1f c: right note %3ld/%3ld blocks, median %+6.2f c %s\n", SR, m, c,
                   right, blocks, med, ok ? "" : "<-- FAIL");
        }
    free(got);
    d->deactivate(h);
    d->cleanup(h);
    printf("%s\n", bad == 0 ? "PASS" : "FAIL");
    return bad == 0 ? 0 : 1;
}
