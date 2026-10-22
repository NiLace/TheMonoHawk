// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
// tear_probe.cpp -- can the audio thread see HALF of an analysis?
//
// THE INVARIANT THAT MAKES IT VISIBLE. The worker publishes note, cents, clarity, freq and voiced
// from one analysis, and those numbers are not independent: freq is the note's nominal pitch bent
// by cents. So for any reading that came from a SINGLE analysis,
//
//     1200 * log2(freq / note_to_frequency(note)) == cents
//
// holds to within rounding. If the audio thread samples the ports while the worker is midway
// through publishing, it gets fields from two different analyses and the identity breaks. That is
// the whole test: it needs no access to plugin internals, only the output ports a host reads.
//
// WHAT THIS DOES AND DOES NOT CLAIM. The writer's critical section is five stores, so the natural
// tear rate is minute and a probe that reports "0 violations" against the unfixed build would be
// reporting its own blindness, not the plugin's health. So the check is calibrated: a STRESS build
// (a copy of the source with a deliberate stall between the stores) proves the probe can see a
// tear at all. Only then does a clean run on the real build mean anything.
//
// build: g++ -std=c++17 -O2 -I src tools/tear_probe.cpp -o /tmp/tear_probe -ldl -lpthread
// run:   /tmp/tear_probe <plugin.so>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <thread>
#include <vector>

namespace {

struct Desc {
    const char* URI;
    void* (*instantiate)(const void*, double, const char*, const void* const*);
    void  (*connect_port)(void*, uint32_t, void*);
    void  (*activate)(void*);
    void  (*run)(void*, uint32_t);
    void  (*deactivate)(void*);
    void  (*cleanup)(void*);
    const void* (*extension_data)(const char*);
};

constexpr double SR = 48000.0;
// Small blocks on purpose: the reader samples the ports once per run(), so the finer the block the
// more chances it gets to land inside a publication.
constexpr int BLOCK = 32;

// From lv2/hawk.ttl -- copied, not guessed.
enum {
    P_IN = 0, P_OUT = 1, P_ACTIVE = 2, P_DIM = 3, P_NOTE = 4, P_CENTS = 5,
    P_CLARITY = 6, P_FREQ = 7, P_VOICED = 8, P_INTUNE_C = 9, P_RAISE = 10,
    P_IN_TUNE = 11, P_LOWER = 12, P_A4 = 13, P_TUNING = 14, P_REFACT = 15,
    P_REFNOTE = 16, P_PING = 17, P_LEVEL = 18, P_SENS = 19,
    P_ENABLED = 22, P_N = 23
};
double note_hz(double midi, double a4) { return a4 * std::pow(2.0, (midi - 69.0) / 12.0); }

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "use: %s <plugin.so>\n", argv[0]); return 1; }
    void* lib = dlopen(argv[1], RTLD_NOW);
    if (!lib) { std::fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    auto df = (const Desc* (*)(uint32_t))dlsym(lib, "lv2_descriptor");
    const Desc* d = df(0);

    float ctrl[P_N];
    std::memset(ctrl, 0, sizeof ctrl);
    ctrl[P_ACTIVE]   = 1.0f;
    ctrl[P_A4]       = 440.0f; ctrl[P_ENABLED] = 1.0f;   // not bypassed
    ctrl[P_SENS]     = 0.5f;
    ctrl[P_INTUNE_C] = 3.0f;

    std::vector<float> in(BLOCK), out(BLOCK);
    void* h = d->instantiate((const void*)d, SR, "", nullptr);
    for (int i = 0; i < P_N; ++i) d->connect_port(h, (uint32_t)i, &ctrl[i]);
    d->connect_port(h, P_IN, in.data());
    d->connect_port(h, P_OUT, out.data());
    if (d->activate) d->activate(h);

    // The note alternates so that consecutive analyses differ in EVERY field. A tear between two
    // identical analyses would be invisible and also harmless; the interesting case is a change.
    const double tones[2] = { 220.0, 293.6648 };   // A3 and D4, a fourth apart
    double phase = 0.0;
    int    tone  = 0;

    long   blocks = 0, checked = 0, violations = 0, lagging = 0;
    double worst  = 0.0;
    struct { float note, cents, freq; double err; } examples[8];
    int    n_ex = 0;

    const int TOTAL = (int)(12.0 * SR / BLOCK);          // ~12 s
    const int SWAP  = (int)(0.40 * SR / BLOCK);          // change note every 400 ms

    for (int b = 0; b < TOTAL; ++b) {
        if (b % SWAP == 0) tone ^= 1;
        for (int i = 0; i < BLOCK; ++i) {
            in[i]  = 0.4f * (float)std::sin(phase);
            phase += 2.0 * M_PI * tones[tone] / SR;
            if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
        }
        d->connect_port(h, P_IN, in.data());
        d->connect_port(h, P_OUT, out.data());
        d->run(h, (uint32_t)BLOCK);
        ++blocks;

        // ---- the consistency check, on the ports only ----------------------
        if (ctrl[P_VOICED] > 0.5f && ctrl[P_FREQ] > 1.0f && ctrl[P_NOTE] > 0.0f) {
            ++checked;
            const double implied = 1200.0 * std::log2(ctrl[P_FREQ] / note_hz(ctrl[P_NOTE], 440.0));
            const double err     = std::fabs(implied - ctrl[P_CENTS]);
            /* TWO thresholds, because a small disagreement is not a tear and calling it one
             * would make the probe cry wolf. `cents` is a MEDIAN over the last few analyses while
             * `freq` is the current one, so around a note change they legitimately differ by a
             * cent or so -- the first version of this check flagged 1.3-cent lag as a race.
             *
             * The verdict gate is 100 cents. Below it lies every by-design disagreement; above it
             * lies the tear that matters, where the note field comes from one analysis and the
             * frequency from another. The test tones are a fourth apart, so a real mixture lands
             * at 500 cents or more -- and the stress build reaches 2400. Nothing is close to 100
             * from either side, which is what a threshold wants.
             *
             * A tear confined to `cents` alone would be small and is not counted. That is
             * deliberate: it would put a plausible number on the display instead of an impossible
             * one, and this probe is about the impossible ones. */
            if (err > 1.0) ++lagging;
            if (err > 100.0) {
                ++violations;
                if (err > worst) worst = err;
                if (n_ex < 8) { examples[n_ex++] = { ctrl[P_NOTE], ctrl[P_CENTS], ctrl[P_FREQ], err }; }
            }
        }
        std::this_thread::sleep_for(std::chrono::microseconds((int)(BLOCK * 1e6 / SR)));
    }

    if (d->deactivate) d->deactivate(h);
    d->cleanup(h);

    std::printf("Is the published analysis ever seen half-written?\n\n");
    std::printf("  blocks run                    : %ld (%d samples each)\n", blocks, BLOCK);
    std::printf("  blocks with a usable reading  : %ld\n", checked);
    std::printf("  off by >1 cent (median lag)   : %ld  <- by design, not a race\n", lagging);
    std::printf("  internally impossible (>100 c): %ld\n", violations);
    if (violations) {
        std::printf("  worst inconsistency           : %.1f cents\n", worst);
        for (int i = 0; i < n_ex; ++i)
            std::printf("    note %.0f  cents %+7.2f  freq %8.3f Hz   -> off by %.1f cents\n",
                        examples[i].note, examples[i].cents, examples[i].freq, examples[i].err);
    }
    std::printf("\n%s\n", violations == 0
        ? "PASS: every reading was self-consistent -- no mixture of two analyses."
        : "FAIL: the ports showed a reading that no single analysis could produce.");
    return violations == 0 ? 0 : 1;
}
