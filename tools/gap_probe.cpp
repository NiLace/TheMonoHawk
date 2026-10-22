// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
// gap_probe.cpp -- a ring overflow end to end, against the real .so.
//
// THE QUESTION. When the ring overflows, samples vanish from the middle of the stream. The
// detector's window is then a SPLICE: two pieces of audio that never touched, joined at an
// arbitrary phase. The correlation does not know that, so it answers -- confidently, and about a
// waveform that was never played. The fix is not to recover the lost audio (impossible), it is to
// refuse to answer until the window is clean again.
//
// HOW IT IS OBSERVED, and the first two attempts at this both failed to see anything:
//
//   1. Measuring "how long until it reports the new tone" does not discriminate, because both the
//      fixed and the broken build get there at almost the same moment. The delay is set by how
//      fast fresh audio arrives, not by the fix.
//   2. Putting the NEW tone in the oversized block does not discriminate either: what the ring
//      keeps is the first 65536 samples of that block, so the splice ends up between the new tone
//      and itself -- a phase jump, but not a change of pitch, and the detector shrugs it off.
//
// What the fix actually prevents is a reading published from a window that straddles the seam. So
// the oversized block carries the OLD tone (it fills the ring with 220 Hz and loses its tail), and
// the audio that arrives afterwards is the NEW one. When the worker reaches the seam its window is
// part 220 and part 330 -- audio that was never played. The discriminator is therefore WHAT IT
// SAYS in between: any published frequency that is neither 220 nor 330 came from the splice.
//
// ⚠ The worker is a real thread on its own clock, so the probe must be paced in WALL time -- a
// previous bench in this repo fed 2 s of audio in 10 ms and read a detector that had not yet run
// once. Every block here sleeps for its own duration.
//
// build: g++ -std=c++17 -O2 -I src tools/gap_probe.cpp -o /tmp/gap_probe -ldl
// run:   /tmp/gap_probe <plugin.so>

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

constexpr double SR    = 48000.0;
constexpr int    BLOCK = 256;

// Port map, copied from the lv2:index lines of lv2/hawk.ttl -- NOT guessed. Getting this wrong is
// not a small error and it has already happened twice here: an earlier bench assumed port 2 was
// the note OUTPUT when it is the `active` INPUT and so measured a plugin that was switched off,
// and the first version of THIS probe invented a 14-port layout for a 20-port plugin, which left
// six ports pointing at nothing and segfaulted on the first run().
enum {
    P_IN = 0, P_OUT = 1, P_ACTIVE = 2, P_DIM = 3, P_NOTE = 4, P_CENTS = 5,
    P_CLARITY = 6, P_FREQ = 7, P_VOICED = 8, P_INTUNE_C = 9, P_RAISE = 10,
    P_IN_TUNE = 11, P_LOWER = 12, P_A4 = 13, P_TUNING = 14, P_REFACT = 15,
    P_REFNOTE = 16, P_PING = 17, P_LEVEL = 18, P_SENS = 19,
    P_ENABLED = 22, P_N = 23
};
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
    ctrl[P_ACTIVE] = 1.0f;
    ctrl[P_A4]     = 440.0f; ctrl[P_ENABLED] = 1.0f;   // not bypassed
    ctrl[P_SENS]   = 0.5f;
    ctrl[P_INTUNE_C] = 3.0f;

    std::vector<float> in(BLOCK), out(BLOCK);
    void* h = d->instantiate((const void*)d, SR, "", nullptr);
    for (int i = 0; i < P_N; ++i) d->connect_port(h, (uint32_t)i, &ctrl[i]);
    d->connect_port(h, P_IN,  in.data());
    d->connect_port(h, P_OUT, out.data());
    if (d->activate) d->activate(h);

    long  n     = 0;      // sample index, drives the phase so the tone is continuous across blocks
    double phase = 0.0;

    auto feed = [&](double f0, int nblocks) {
        for (int b = 0; b < nblocks; ++b) {
            for (int i = 0; i < BLOCK; ++i) {
                in[i]  = 0.4f * (float)std::sin(phase);
                phase += 2.0 * M_PI * f0 / SR;
                if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
            }
            d->connect_port(h, P_IN,  in.data());
            d->connect_port(h, P_OUT, out.data());
            d->run(h, (uint32_t)BLOCK);
            n += BLOCK;
            std::this_thread::sleep_for(std::chrono::microseconds((int)(BLOCK * 1e6 / SR)));
        }
    };

    // ---- settle on 220 Hz -------------------------------------------------
    feed(220.0, (int)(1.5 * SR / BLOCK));
    const float f_before = ctrl[P_FREQ];
    std::printf("A forced ring overflow, and what the plugin says afterwards\n\n");
    std::printf("  settled on 220 Hz     : reported %.2f Hz, note %.0f, voiced %.0f\n",
                ctrl[P_FREQ], ctrl[P_NOTE], ctrl[P_VOICED]);

    // ---- force the gap ----------------------------------------------------
    // One run() bigger than the ring (65536). push() sizes itself from the free space it saw on
    // entry, so the overflow is deterministic: 65536 samples of 220 Hz go in and the rest is lost.
    // That is the same stream damage a 1.4 s worker stall would do, without having to stall a
    // thread and without depending on scheduling.
    const int BIG = 100000;
    std::vector<float> bin(BIG), bout(BIG);
    for (int i = 0; i < BIG; ++i) {
        bin[i] = 0.4f * (float)std::sin(phase);
        phase += 2.0 * M_PI * 220.0 / SR;
        if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
    }
    d->connect_port(h, P_IN,  bin.data());
    d->connect_port(h, P_OUT, bout.data());
    d->run(h, (uint32_t)BIG);
    n += BIG;
    std::printf("  forced a %d-sample block into a 65536 ring: %d samples of 220 Hz were lost\n",
                BIG, BIG - 65536);
    std::printf("  everything that arrives from here on is 330 Hz, so the seam joins two pitches\n");

    // ---- watch everything it says from here on ----------------------------
    // Sampled once per block. A reading is "off-tone" if it is neither of the two frequencies that
    // were actually played; the only place such a value can come from is a window spanning the seam.
    int  n_off = 0;
    float worst_off = 0.0f;
    std::vector<float> offs;
    for (int b = 0; b < (int)(3.0 * SR / BLOCK); ++b) {
        feed(330.0, 1);
        const float f = ctrl[P_FREQ];
        if (ctrl[P_VOICED] > 0.5f && f > 1.0f &&
            std::fabs(f - 220.0f) > 6.0f && std::fabs(f - 330.0f) > 6.0f) {
            ++n_off;
            if (offs.size() < 12) offs.push_back(f);
            const float dev = std::fmin(std::fabs(f - 220.0f), std::fabs(f - 330.0f));
            if (dev > worst_off) worst_off = dev;
        }
    }

    std::printf("\n  blocks reporting a pitch that was never played : %d\n", n_off);
    if (n_off) {
        std::printf("  those readings (Hz)                           :");
        for (float f : offs) std::printf(" %.1f", f);
        std::printf("%s\n", n_off > (int)offs.size() ? " ..." : "");
        std::printf("  furthest from any played tone                 : %.1f Hz\n", worst_off);
    }
    std::printf("  final reading                                 : %.2f Hz, note %.0f, cents %+.1f\n",
                ctrl[P_FREQ], ctrl[P_NOTE], ctrl[P_CENTS]);
    std::printf("  (was %.2f Hz before the gap)\n", f_before);

    if (d->deactivate) d->deactivate(h);
    d->cleanup(h);

    const bool ok = (n_off == 0) && std::fabs(ctrl[P_FREQ] - 330.0f) < 8.0f;
    std::printf("\n%s\n", ok ? "PASS: nothing was published from the spliced window, and it recovered."
                             : "FAIL: it reported a pitch that was never played.");
    return ok ? 0 : 1;
}
