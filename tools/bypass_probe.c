// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
// bypass_probe.c -- the plugin's own bypass (the lv2:enabled port, behind the hawk in the GUI).
//
// A held A2 through the loaded .so at real-time pace, with the dim engaged (-12 dB) and the reference tone on, so
// the processed output differs from the input. Then enabled = 0 for 1 s, then 1 again. Pass when:
//   - processing: the output differs from the input (the probe is not blind);
//   - bypassed, after a 150 ms fade (the reference tone's tail): the output IS the input, bit for bit, and the tuner shows nothing (voiced,
//     the three lamps, the level, and the reading -- note, cents, frequency, strobe phase -- all 0);
//   - re-enabled: no block shows a reading from before the bypass (voiced stays 0 until a fresh analysis), and
//     the note reads again within 0.6 s.
// run: bypass_probe <plugin.so>      exit 0 = PASS
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
enum { P_IN = 0, P_OUT = 1, P_ACTIVE = 2, P_DIM = 3, P_NOTE = 4, P_CENTS = 5, P_FREQ = 7, P_VOICED = 8, P_RAISE = 10, P_INTUNE = 11, P_LOWER = 12,
       P_REFACT = 15, P_REFNOTE = 16, P_LEVEL = 18 };
enum { P_STROBE = 20, P_ENABLED = 22, P_N = 23 };

static const Desc* d;
static void* h;
static float ctrl[P_N], in[BLOCK], out[BLOCK];
static double ph[3];
static struct timespec next;

// One block of the held A2 through the plugin, then sleep to the block's real-time deadline.
static void block(void)
{
    for (int i = 0; i < BLOCK; ++i) {
        double s = 0.0;
        for (int k = 0; k < 3; ++k) {
            s += 0.2 / (k + 1) * sin(ph[k]);
            ph[k] += 2 * M_PI * (k + 1) * 110.0 / SR;
            if (ph[k] > 2 * M_PI) ph[k] -= 2 * M_PI;
        }
        in[i] = (float)s;
    }
    d->run(h, (uint32_t)BLOCK);
    next.tv_nsec += (long)(BLOCK * 1e9 / SR);
    while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; ++next.tv_sec; }
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) != 0) {}
}

static int blocks(double s) { return (int)(s * SR / BLOCK); }

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <plugin.so>\n", argv[0]); return 2; }
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
    const Desc* (*get)(uint32_t);
    *(void**)&get = dlsym(lib, "lv2_descriptor");   // POSIX's way to take a function from dlsym
    d = get ? get(0) : NULL;
    if (!d) return 2;
    h = d->instantiate(d, SR, "", NULL);
    ctrl[P_ACTIVE] = 1.0f; ctrl[P_DIM] = -12.0f; ctrl[9] = 2.0f; ctrl[13] = 440.0f; ctrl[19] = 0.5f;
    ctrl[P_REFACT] = 1.0f; ctrl[P_REFNOTE] = 45.0f; ctrl[P_ENABLED] = 1.0f;
    for (int p = 2; p < P_N; ++p) d->connect_port(h, (uint32_t)p, &ctrl[p]);
    d->connect_port(h, P_IN, in);
    d->connect_port(h, P_OUT, out);
    d->activate(h);

    clock_gettime(CLOCK_MONOTONIC, &next);
    bool differs = false, voiced_before = false;
    for (int b = 0; b < blocks(1.5); ++b) {
        block();
        for (int i = 0; i < BLOCK; ++i) if (out[i] != in[i]) differs = true;
        voiced_before = ctrl[P_VOICED] > 0.5f;
    }
    ctrl[P_ENABLED] = 0.0f;
    long not_exact = 0, shows = 0;
    for (int b = 0; b < blocks(1.0); ++b) {
        block();
        if (b < blocks(0.15)) continue;                    // the dim and the tone fade out first
        for (int i = 0; i < BLOCK; ++i) if (out[i] != in[i]) ++not_exact;
        if (ctrl[P_VOICED] > 0.5f || ctrl[P_RAISE] > 0.5f || ctrl[P_INTUNE] > 0.5f || ctrl[P_LOWER] > 0.5f ||
            ctrl[P_LEVEL] > 0.0f) ++shows;
        // and the reading itself: a host's generic UI or an automation lane shows these ports as they are
        if (ctrl[P_NOTE] != 0.0f || ctrl[P_CENTS] != 0.0f || ctrl[P_FREQ] != 0.0f || ctrl[P_STROBE] != 0.0f) ++shows;
    }
    ctrl[P_ENABLED] = 1.0f;
    long stale = 0, back_at = -1;
    for (int b = 0; b < blocks(1.0); ++b) {
        block();
        if (ctrl[P_VOICED] > 0.5f) {
            if (back_at < 0) back_at = b;
        }
        if (b == 0 && ctrl[P_VOICED] > 0.5f) ++stale;      // the very first block can only be stale
    }
    d->deactivate(h);
    d->cleanup(h);
    const double back_ms = back_at < 0 ? -1.0 : 1000.0 * back_at * BLOCK / SR;
    const bool ok = differs && voiced_before && not_exact == 0 && shows == 0 && stale == 0 && back_at >= 0 &&
                    back_ms <= 600.0;
    printf("processing: output %s the input, voiced %d | bypassed: %ld samples not equal to the input, %ld blocks "
           "showing a reading | re-enabled: stale first block %ld, reads again after %.0f ms\n%s\n",
           differs ? "differs from" : "EQUALS", (int)voiced_before, not_exact, shows, stale, back_ms,
           ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
