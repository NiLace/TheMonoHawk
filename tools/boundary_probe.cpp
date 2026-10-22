// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
// boundary_probe.cpp -- a string near the boundary between two notes is read as the note it is nearest.
//
// Two cases through the loaded .so at real-time pace, A4 = 440:
//   sweep -- E3 swept from +20 c to +80 c and back at 20 c/s (into F and out): no published (note, cents) pair may sit
//            more than 30 c from the input's pitch at that moment (the analysis lags ~0.2 s, ~4 c at this rate). The
//            fault this guards: the cents median carrying the old note's +49 under the new note -- 100 c off.
//   noisy -- E3 +52 c (= F3 -48 c) held 3 s in white noise ~8 dB below the string: every voiced block reads |cents| <= 50.
// PASS (exit 0) iff both hold. Negative arm: the .so built with -DHAWK_OLD_NOTE_SNAP=1 must FAIL (test/meson.build).
// run: boundary_probe <plugin.so>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
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
constexpr int P_IN = 0, P_OUT = 1, P_NOTE = 4, P_CENTS = 5, P_VOICED = 8, P_TOL = 9;
constexpr int P_ENABLED = 22, P_N = 23;
constexpr double E3 = 164.8138;   // MIDI 52 at A4 = 440
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: %s <plugin.so>\n", argv[0]); return 2; }
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { std::fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
    auto get = (const Desc* (*)(uint32_t))dlsym(lib, "lv2_descriptor");
    const Desc* d = get ? get(0) : nullptr;
    if (!d) return 2;
    std::mt19937 rng(11);
    std::normal_distribution<double> noise(0.0, 1.0);
    auto run_case = [&](bool sweep, double& worst_off, double& worst_abs, long& voiced_blocks) {
        void* h = d->instantiate(d, SR, "", nullptr);
        std::vector<float> ctrl(P_N, 0.0f);
        ctrl[3] = -12.0f; ctrl[P_TOL] = 2.0f; ctrl[13] = 440.0f; ctrl[P_ENABLED] = 1.0f; ctrl[19] = 0.5f;
        for (int p = 2; p < P_N; ++p) d->connect_port(h, (uint32_t)p, &ctrl[p]);
        std::vector<float> in(BLOCK), out(BLOCK);
        d->connect_port(h, P_IN, in.data());
        d->connect_port(h, P_OUT, out.data());
        d->activate(h);
        const double dur = sweep ? 7.0 : 4.0;
        double ph[8] = {};
        auto cents_at = [&](double t) {                     // the input's offset from E3, in cents
            if (!sweep) return 52.0;
            if (t < 1.0) return 20.0;
            const double u = t - 1.0;                        // 20 -> 80 in 3 s, back in 3 s
            return u < 3.0 ? 20.0 + 20.0 * u : 80.0 - 20.0 * (u - 3.0);
        };
        auto next = std::chrono::steady_clock::now();
        const auto blk = std::chrono::nanoseconds((long)(BLOCK * 1e9 / SR));
        for (long n0 = 0; n0 < (long)(dur * SR); n0 += BLOCK) {
            for (int i = 0; i < BLOCK; ++i) {
                const double t = (double)(n0 + i) / SR;
                const double f = E3 * std::pow(2.0, cents_at(t) / 1200.0);
                double s = 0.0;
                for (int m = 1; m <= 8; ++m) {
                    s += 0.3 / m * std::sin(ph[m - 1]);
                    ph[m - 1] += 2 * M_PI * m * f / SR;
                    if (ph[m - 1] > 2 * M_PI) ph[m - 1] -= 2 * M_PI;
                }
                if (!sweep) s += 0.10 * noise(rng);           // ~8 dB below the string (rms 0.26)
                in[i] = (float)s;
            }
            d->run(h, (uint32_t)BLOCK);
            const double t = (double)(n0 + BLOCK) / SR;
            if (ctrl[P_VOICED] > 0.5f && t > 0.6) {
                ++voiced_blocks;
                const double published = (ctrl[P_NOTE] - 52.0) * 100.0 + ctrl[P_CENTS];   // cents from E3
                double off = 1e9;                       // nearest input pitch within the analysis's lag (0..0.5 s)
                for (double lag = 0.0; lag <= 0.5; lag += 0.01)
                    off = std::fmin(off, std::fabs(published - cents_at(std::fmax(0.0, t - lag))));
                worst_off = std::fmax(worst_off, off);
                worst_abs = std::fmax(worst_abs, std::fabs((double)ctrl[P_CENTS]));
            }
            next += blk;
            std::this_thread::sleep_until(next);
        }
        d->deactivate(h);
        d->cleanup(h);
    };
    double s_off = 0, s_abs = 0, n_off = 0, n_abs = 0;
    long s_v = 0, n_v = 0;
    run_case(true, s_off, s_abs, s_v);
    run_case(false, n_off, n_abs, n_v);
    const bool ok = s_v > 100 && n_v > 100 && s_off <= 30.0 && n_abs <= 50.0;
    std::printf("sweep across E3/F3: %ld voiced blocks, worst pair %.1f c off the input (want <= 30)\n", s_v, s_off);
    std::printf("E3 +52 c in noise:  %ld voiced blocks, worst |cents| %.2f (want <= 50)\n%s\n", n_v, n_abs,
                ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
