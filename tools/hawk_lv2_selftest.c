// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
// Headless host test for the LV2 plugin. Loads hawk.so the way a host would
// (dlopen + lv2_descriptor), feeds it a known tone in audio-sized blocks, lets
// the worker thread run, and reads the output ports back. Proves the whole
// plugin path works end to end, not just that the TTL parses.
//
// This is a pass/fail test: it prints a human-readable report AND returns a
// non-zero exit code if any check fails, so `meson test` reports honest status.
// All input control ports are connected to deterministic "off" defaults (ping
// off, reference tone off) so the run is reproducible and passthrough is
// bit-exact -- a host always connects every port before run().

#include <lv2/core/lv2.h>

#include "hawk_rng.h"

#include <dlfcn.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

// Port indices -- must match PortIndex in lv2/hawk_lv2.c.
enum {
    P_IN = 0, P_OUT, P_ACTIVE, P_DIM_DB, P_NOTE, P_CENTS, P_CLARITY, P_FREQ,
    P_VOICED, P_TOLERANCE, P_RAISE, P_IN_TUNE, P_LOWER, P_A4, P_TUNING,
    P_REF_ACTIVE, P_REF_NOTE, P_PING_ENABLE, P_LEVEL, P_SENSITIVITY
};

static int failures = 0;

static void check(bool ok, const char* what)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

static void sleep_us(long us)
{
    struct timespec ts = { us / 1000000, (us % 1000000) * 1000 };
    while (nanosleep(&ts, &ts) != 0) {}   // interrupted: sleep what is left
}

int main(int argc, char** argv)
{
    const char* so_path = (argc > 1) ? argv[1]
                                     : "build/lv2/hawk.so";
    const double rate  = 48000.0;
    const double freq  = (argc > 2) ? atof(argv[2]) : 110.0; // A2

    void* lib = dlopen(so_path, RTLD_NOW);
    if (!lib) { printf("dlopen failed: %s\n", dlerror()); return 1; }

    LV2_Descriptor_Function entry;
    *(void**)&entry = dlsym(lib, "lv2_descriptor");   // POSIX's way to take a function from dlsym
    if (!entry) { printf("no lv2_descriptor symbol\n"); return 1; }

    const LV2_Descriptor* d = entry(0);
    if (!d) { printf("no descriptor 0\n"); return 1; }
    printf("loaded plugin: %s\n", d->URI);

    const LV2_Feature* features[] = { NULL };
    LV2_Handle h = d->instantiate(d, rate, "", features);
    if (!h) { printf("instantiate failed\n"); return 1; }

    enum { block = 256 };
    float in[block] = { 0 }, out[block] = { 0 };
    // Input control ports, all set to deterministic, neutral defaults.
    float active = 0.0f, dim_db = -12.0f, tolerance = 2.0f;
    float a4 = 440.0f, tuning = 0.0f;
    float ref_active = 0.0f, ref_note = 45.0f;   // reference tone OFF
    float ping_enable = 0.0f;                     // in-tune ping OFF -> bit-exact
    float sensitivity = 0.5f;                     // default behaviour
    // Output control ports the plugin writes back.
    float note = 0, cents = 0, clarity = 0, fhz = 0, voiced = 0;
    float raise = 0, in_tune = 0, lower = 0, level = 0;

    d->connect_port(h, P_IN,          in);
    d->connect_port(h, P_OUT,         out);
    d->connect_port(h, P_ACTIVE,      &active);
    d->connect_port(h, P_DIM_DB,      &dim_db);
    d->connect_port(h, P_NOTE,        &note);
    d->connect_port(h, P_CENTS,       &cents);
    d->connect_port(h, P_CLARITY,     &clarity);
    d->connect_port(h, P_FREQ,        &fhz);
    d->connect_port(h, P_VOICED,      &voiced);
    d->connect_port(h, P_TOLERANCE,   &tolerance);
    d->connect_port(h, P_RAISE,       &raise);
    d->connect_port(h, P_IN_TUNE,     &in_tune);
    d->connect_port(h, P_LOWER,       &lower);
    d->connect_port(h, P_A4,          &a4);
    d->connect_port(h, P_TUNING,      &tuning);
    d->connect_port(h, P_REF_ACTIVE,  &ref_active);
    d->connect_port(h, P_REF_NOTE,    &ref_note);
    d->connect_port(h, P_PING_ENABLE, &ping_enable);
    d->connect_port(h, P_LEVEL,       &level);
    d->connect_port(h, P_SENSITIVITY, &sensitivity);

    d->activate(h);

    // Feed ~1.5 s of a steady tone, in real-time-ish blocks so the worker keeps
    // up, then let it settle.
    double phase = 0.0;
    const double dphase = 2.0 * M_PI * freq / rate;
    const int blocks = (int)(1.5 * rate / block);
    for (int b = 0; b < blocks; ++b) {
        for (uint32_t i = 0; i < block; ++i) {
            in[i] = (float)(0.3 * sin(phase));
            phase += dphase;
        }
        d->run(h, block);
        sleep_us(500);
    }
    sleep_us(50000);
    d->run(h, block);  // one more to latch the latest result into the ports

    printf("input tone: %.2f Hz\n", freq);
    printf("ports -> voiced=%.0f  note(MIDI)=%.0f  cents=%+.2f  "
                "freq=%.3f Hz  clarity=%.3f\n",
                voiced, note, cents, fhz, clarity);
    printf("lamps -> RAISE=%.0f  IN_TUNE=%.0f  LOWER=%.0f  (tol=%.1f c)\n",
                raise, in_tune, lower, tolerance);

    // Expected MIDI note for the input frequency at A4 = 440 (standard formula).
    const long expected_note =
        lround(69.0 + 12.0 * log2(freq / 440.0));

    printf("detection checks:\n");
    check(voiced > 0.5f,                       "voiced (tone detected)");
    check(lroundf(note) == expected_note,      "correct MIDI note");
    check(fabsf(cents) <= 0.5f,                 "sub-cent accuracy (|cents| <= 0.5)");
    check(in_tune > 0.5f,                      "In Tune lamp latched on an on-pitch tone");

    // Passthrough sanity: with dim inactive and ping off, output == input,
    // bit-exact.
    float max_diff = 0.0f;
    for (uint32_t i = 0; i < block; ++i)
        if (max_diff < fabsf(out[i] - in[i])) max_diff = fabsf(out[i] - in[i]);
    printf("passthrough max |out-in| (dim off, ping off) = %.3e\n", max_diff);
    check(max_diff == 0.0f, "bit-exact passthrough");

    // Stability: a note sitting just inside the in-tune band, with harmonics
    // and noise, should latch the In Tune lamp solid -- ideally one transition
    // (off -> on), not a flicker.
    {
        HawkMT rng;
        hawk_mt_seed(&rng, 7);
        HawkNormal gauss = hawk_normal(0.0, 1.0);
        const double base = 110.0 * pow(2.0, 1.0 / 1200.0);  // +1 cent
        double ph = 0.0;
        const double dp = 2.0 * M_PI * base / rate;
        int   transitions = 0;
        float prev = in_tune;
        const int nb     = (int)(2.5 * rate / block);
        // Warm-up: the test jumps abruptly from a clean tone to this different
        // noisy one, and the in-tune latch from the first tone has long expired,
        // so the detector re-locks over a few frames. That synthetic transition
        // isn't what we're testing -- STEADY-STATE lamp stability is. Skip the
        // first ~0.6 s, then require the lamp to sit solid. Feed near real time so
        // the worker has a host-realistic budget (256 frames ~ 5.3 ms of audio).
        const int warmup = (int)(0.6 * rate / block);
        for (int b = 0; b < nb; ++b) {
            for (uint32_t i = 0; i < block; ++i) {
                double s = sin(ph) + 0.5 * sin(2 * ph)
                                   + 0.3 * sin(3 * ph);
                s += 0.06 * hawk_normal_draw(&gauss, &rng);
                in[i] = (float)(0.25 * s);
                ph += dp;
            }
            d->run(h, block);
            if (b == warmup) prev = in_tune;                 // start counting at steady state
            else if (b > warmup && in_tune != prev) { ++transitions; prev = in_tune; }
            sleep_us(4500);
        }
        printf("stability: +1c noisy note (steady state) -> In Tune toggled "
                    "%d time(s) [0 = latched solid, good]\n", transitions);
        check(transitions <= 1, "In Tune lamp stable (<= 1 transition, steady state)");
    }

    d->deactivate(h);
    d->cleanup(h);
    dlclose(lib);

    printf(failures == 0 ? "\nSELFTEST PASSED\n"
                              : "\nSELFTEST FAILED (%d check(s))\n", failures);
    return failures == 0 ? 0 : 1;
}
