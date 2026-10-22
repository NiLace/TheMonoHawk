// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
// noise_probe.cpp -- does the tuner invent notes from noise?
//
// Sensitivity lowers the coarse clarity gate (0.85 at the default, 0.65 at the top), and a lower
// gate lets noise through as a "note". This feeds the loaded .so noise only -- white, and a low,
// room-like noise (white through a one-pole low-pass at 300 Hz) -- at -30 and -50 dBFS, for 6 s
// each, at Sensitivity 0.5 and 1.0, and counts the blocks that publish voiced = 1.
// PASS (exit 0) iff no block is voiced.
//
// build: g++ -std=c++17 -O2 tools/noise_probe.cpp -o noise_probe -ldl -lpthread
// run:   noise_probe <plugin.so>

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
constexpr int BLOCK = 256;
constexpr int P_IN = 0, P_OUT = 1, P_NOTE = 4, P_VOICED = 8, P_SENS = 19, P_ENABLED = 22, P_N = 23;
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: %s <plugin.so>\n", argv[0]); return 2; }
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { std::fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
    auto get = (const Desc* (*)(uint32_t))dlsym(lib, "lv2_descriptor");
    const Desc* d = get ? get(0) : nullptr;
    if (!d) { std::fprintf(stderr, "no descriptor\n"); return 2; }
    long total_voiced = 0;
    std::printf("%s -- blocks publishing a note from noise alone (6 s = %d blocks each)\n", argv[1], (int)(6.0 * SR / BLOCK));
    for (double sens : {0.5, 1.0})
        for (int kind = 0; kind < 2; ++kind)
            for (double dbfs : {-30.0, -50.0}) {
                void* h = d->instantiate(d, SR, "", nullptr);
                std::vector<float> ctrl(P_N, 0.0f);
                ctrl[3] = -20.0f; ctrl[9] = 2.0f; ctrl[13] = 440.0f; ctrl[P_ENABLED] = 1.0f; ctrl[P_SENS] = (float)sens;
                for (int p = 2; p < P_N; ++p) d->connect_port(h, (uint32_t)p, &ctrl[p]);
                std::vector<float> in(BLOCK), out(BLOCK);
                d->connect_port(h, P_IN, in.data());
                d->connect_port(h, P_OUT, out.data());
                d->activate(h);
                std::mt19937 rng(11); std::normal_distribution<double> g(0.0, 1.0);
                const double a_lp = 1.0 - std::exp(-2.0 * M_PI * 300.0 / SR);
                const double rms = std::pow(10.0, dbfs / 20.0);
                double lp = 0.0, lp_rms = std::sqrt(a_lp / (2.0 - a_lp));   // rms of the one-pole output
                long voiced = 0; int notes[128] = {};
                auto next = std::chrono::steady_clock::now();
                const auto blk = std::chrono::nanoseconds((long)(BLOCK * 1e9 / SR));
                for (long n0 = 0; n0 < (long)(6.0 * SR); n0 += BLOCK) {
                    for (int i = 0; i < BLOCK; ++i) {
                        const double w = g(rng);
                        lp += a_lp * (w - lp);
                        in[i] = (float)(rms * (kind == 0 ? w : lp / lp_rms));
                    }
                    d->run(h, (uint32_t)BLOCK);
                    if (ctrl[P_VOICED] > 0.5f) { ++voiced; const int nt = (int)ctrl[P_NOTE]; if (nt >= 0 && nt < 128) ++notes[nt]; }
                    next += blk;
                    std::this_thread::sleep_until(next);
                }
                if (d->deactivate) d->deactivate(h);
                d->cleanup(h);
                int distinct = 0; for (int k = 0; k < 128; ++k) if (notes[k]) ++distinct;
                std::printf("  sensitivity %.1f  %-5s noise %4.0f dBFS: %5ld voiced blocks, %d distinct notes\n",
                            sens, kind == 0 ? "white" : "low", dbfs, voiced, distinct);
                total_voiced += voiced;
            }
    std::printf("%s\n", total_voiced == 0 ? "PASS: no note invented from noise." : "FAIL: notes published from noise alone.");
    return total_voiced == 0 ? 0 : 1;
}
