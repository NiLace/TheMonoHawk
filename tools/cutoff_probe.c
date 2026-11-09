// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
// cutoff_probe.c -- a note stopped dead (a hand on the strings, a mute switch, an edit) must not
// move the reading on its way out.
//
// A held note (three partials) for 3 s, then digital silence, through the loaded .so at real-time
// pace. A window that straddles the stop holds part note, part silence, and analysing it answered
// about a waveform that was never played: measured before the fix, A2 published -6.69 c and a
// frequency 27.9 c off for up to 171 ms after the stop, with the in-tune lamp still lit.
// Pass when, after the stop: every block still voiced reports the cents it reported before it
// (within 0.5 c) and a frequency within 1 c of the note, and nothing is voiced past 60 ms.
// run: cutoff_probe <plugin.so> <note_hz>      exit 0 = PASS
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
#define SR    48000.0
#define BLOCK 256
enum { P_IN = 0, P_OUT = 1, P_CENTS = 5, P_FREQ = 7, P_VOICED = 8 };
enum { P_ENABLED = 22, P_N = 23 };

// Advances an absolute CLOCK_MONOTONIC deadline by ns nanoseconds and sleeps until it.
static void sleep_until_next(struct timespec* next, long ns)
{
    next->tv_nsec += ns;
    while (next->tv_nsec >= 1000000000L) { next->tv_nsec -= 1000000000L; ++next->tv_sec; }
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, next, NULL) != 0) {}
}

int main(int argc, char** argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <plugin.so> <note_hz>\n", argv[0]); return 2; }
    const double f0 = atof(argv[2]);
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
    const Desc* (*get)(uint32_t);
    *(void**)&get = dlsym(lib, "lv2_descriptor");   // POSIX's way to take a function from dlsym
    const Desc* d = get ? get(0) : NULL;
    if (!d) return 2;
    void* h = d->instantiate(d, SR, "", NULL);
    float ctrl[P_N] = { 0.0f };
    ctrl[3] = -12.0f; ctrl[9] = 2.0f; ctrl[13] = 440.0f; ctrl[P_ENABLED] = 1.0f; ctrl[19] = 0.5f;
    for (int p = 2; p < P_N; ++p) d->connect_port(h, (uint32_t)p, &ctrl[p]);
    float in[BLOCK] = { 0.0f }, out[BLOCK] = { 0.0f };
    d->connect_port(h, P_IN, in);
    d->connect_port(h, P_OUT, out);
    d->activate(h);

    const long cut = (long)(3.0 * SR), total = cut + (long)(0.5 * SR);
    double ph = 0.0, cents_before = 0.0, worst_cents = 0.0, worst_freq = 0.0, last_voiced_ms = -1.0;
    bool voiced_before = false;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    for (long n0 = 0; n0 < total; n0 += BLOCK) {
        for (int i = 0; i < BLOCK; ++i) {
            in[i] = n0 + i < cut ? (float)(0.3 * (sin(ph) + 0.5 * sin(2 * ph) + 0.3 * sin(3 * ph))) : 0.0f;
            ph += 2 * M_PI * f0 / SR;
            if (ph > 2 * M_PI) ph -= 2 * M_PI;
        }
        d->run(h, (uint32_t)BLOCK);
        if (n0 + BLOCK <= cut) {
            voiced_before = ctrl[P_VOICED] > 0.5f;
            cents_before  = ctrl[P_CENTS];
        } else if (ctrl[P_VOICED] > 0.5f) {
            last_voiced_ms = 1000.0 * (double)(n0 + BLOCK - cut) / SR;
            const double dc = fabs(ctrl[P_CENTS] - cents_before);
            const double df = fabs(1200.0 * log2(ctrl[P_FREQ] / f0));
            if (dc > worst_cents) worst_cents = dc;
            if (df > worst_freq)  worst_freq  = df;
        }
        sleep_until_next(&next, (long)(BLOCK * 1e9 / SR));
    }
    d->deactivate(h);
    d->cleanup(h);
    const bool ok = voiced_before && worst_cents <= 0.5 && worst_freq <= 1.0 && last_voiced_ms <= 60.0;
    printf("%.2f Hz stopped dead: voiced before %d; after the stop voiced until %+.0f ms, cents moved "
           "%.2f, frequency off by %.1f c\n%s\n", f0, (int)voiced_before, last_voiced_ms, worst_cents,
           worst_freq, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
