// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
// attack_probe.cpp -- what does the published cents reading do right after a pluck?
//
// A real string starts SHARP (tension rises with amplitude) and settles within tens of ms, and the
// pick adds a click. The cents shown is a median of the last 5 analyses, and the analysis windows
// overlap 75 %, so one attack can reach several of them in a row. This plays an in-tune string
// (truth 0 c) plucked every 2 s -- the first from silence, the rest RE-plucked while it still rings,
// which is how one tunes -- with +ATT_C cents at onset decaying with tau ATT_TAU and a 5 ms noise
// click, and reads the plugin's ports block by block. Per pluck, from onset to +1 s:
//   excursion -- the largest |cents| published while voiced
//   settle    -- the last moment |cents| > 1 c (ms after onset)
//   flicker   -- in-tune lamp transitions after it first lit
// PASS (exit 0) iff every re-pluck moves the reading by <= 3 c (the acceptance criterion) with no
// lamp flicker, and the first reading from silence comes within 400 ms. Without the attack gating the re-plucks
// moved it 9-15.5 c and blinked the lamp twice each, so the old build FAILS here.
//
// build: g++ -std=c++17 -O2 tools/attack_probe.cpp -o attack_probe -ldl -lpthread
// run:   attack_probe <plugin.so> [note_hz] [decay]   (default A2, 110 Hz; decay 0.7 /s)
//         the decay sets how loud the string still is when re-plucked 2 s later: 0.7 -> a quarter of
//         the amplitude (a 4x jump), 0.35 -> half (a 2x jump: a soft re-pluck, the hard case)

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <random>
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
constexpr int    BLOCK = 256;
constexpr int    PLUCKS = 7;
constexpr double PERIOD = 2.0, ATT_C = 20.0, ATT_TAU = 0.080;
constexpr int P_IN = 0, P_OUT = 1, P_CENTS = 5, P_VOICED = 8, P_TOL = 9, P_INTUNE = 11, P_ENABLED = 22, P_N = 23;
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: %s <plugin.so> [note_hz]\n", argv[0]); return 2; }
    const double f0 = argc > 2 ? std::atof(argv[2]) : 110.0;
    const double decay = argc > 3 ? std::atof(argv[3]) : 0.7;
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { std::fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
    auto get = (const Desc* (*)(uint32_t))dlsym(lib, "lv2_descriptor");
    const Desc* d = get ? get(0) : nullptr;
    if (!d) { std::fprintf(stderr, "no descriptor\n"); return 2; }
    void* h = d->instantiate(d, SR, "", nullptr);
    std::vector<float> ctrl(P_N, 0.0f);
    ctrl[3] = -20.0f; ctrl[P_TOL] = 2.0f; ctrl[13] = 440.0f; ctrl[P_ENABLED] = 1.0f; ctrl[14] = 0.0f; ctrl[19] = 0.5f;
    for (int p = 2; p < P_N; ++p) d->connect_port(h, (uint32_t)p, &ctrl[p]);
    std::vector<float> in(BLOCK), out(BLOCK);
    d->connect_port(h, P_IN, in.data());
    d->connect_port(h, P_OUT, out.data());
    d->activate(h);

    std::mt19937 rng(7);
    std::normal_distribution<double> noise(0.0, 1.0);
    const long total = (long)((PLUCKS * PERIOD + 0.5) * SR);
    double phase[8] = {};
    struct Res { double exc = 0, settle = 0; int flick = 0; bool lit = false; int last_lamp = 0; double first = -1; };
    std::vector<Res> res(PLUCKS);
    auto next = std::chrono::steady_clock::now();
    const auto blk = std::chrono::nanoseconds((long)(BLOCK * 1e9 / SR));
    for (long n0 = 0; n0 < total; n0 += BLOCK) {
        for (int i = 0; i < BLOCK; ++i) {
            const double T = (double)(n0 + i) / SR;
            const int k = (int)(T / PERIOD);
            const double t = T - k * PERIOD;                       // time since the latest pluck
            const double env = (k < PLUCKS) ? std::exp(-decay * t) : 0.0;
            // a re-pluck REPLACES the vibration (the string is struck again); the old one is gone
            const double a = env, dev = ATT_C * std::exp(-t / ATT_TAU);
            double s = 0.0;
            for (int m = 1; m <= 8; ++m) {
                s += a * (0.3 / m) * std::sin(phase[m - 1]);
                phase[m - 1] += 2 * M_PI * m * f0 * std::pow(2.0, dev / 1200.0) / SR;
                if (phase[m - 1] > 2 * M_PI) phase[m - 1] -= 2 * M_PI;
            }
            if (k < PLUCKS && t < 0.005) s += 0.3 * noise(rng) * (1.0 - t / 0.005);
            in[i] = (float)s;
        }
        d->run(h, (uint32_t)BLOCK);
        const double T = (double)(n0 + BLOCK) / SR;
        const int k = (int)(T / PERIOD);
        const double t = T - k * PERIOD;
        if (k < PLUCKS && t <= 1.0) {
            Res& r = res[k];
            if (ctrl[P_VOICED] > 0.5f) {
                if (r.first < 0) r.first = t;
                const double c = std::fabs(ctrl[P_CENTS]);
                if (c > r.exc) r.exc = c;
                if (c > 1.0) r.settle = t;
            }
            const int lamp = ctrl[P_INTUNE] > 0.5f ? 1 : 0;
            if (r.lit && lamp != r.last_lamp) ++r.flick;
            if (lamp) r.lit = true;
            r.last_lamp = lamp;
        }
        next += blk;
        std::this_thread::sleep_until(next);
    }
    if (d->deactivate) d->deactivate(h);
    d->cleanup(h);

    std::printf("%s  %.1f Hz, +%.0f c at onset (tau %.0f ms) + click, in tune; pluck 1 from silence, 2..%d re-plucked (a %.1fx jump)\n",
                argv[1], f0, ATT_C, ATT_TAU * 1e3, PLUCKS, std::exp(decay * PERIOD));
    double worst_exc = 0, worst_settle = 0; int flick = 0;
    for (int k = 0; k < PLUCKS; ++k) {
        const Res& r = res[k];
        std::printf("  pluck %d: first reading %4.0f ms  excursion %5.2f c  settle %4.0f ms  lamp flicker %d\n",
                    k + 1, r.first * 1e3, r.exc, r.settle * 1e3, r.flick);
        if (k > 0) { worst_exc = std::max(worst_exc, r.exc); worst_settle = std::max(worst_settle, r.settle); flick += r.flick; }
    }
    std::printf("re-plucks: worst excursion %.2f c, worst settle %.0f ms, lamp flicker %d\n",
                worst_exc, worst_settle * 1e3, flick);
    const bool ok = worst_exc <= 3.0 && flick == 0 && res[0].first >= 0 && res[0].first <= 0.400;
    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
