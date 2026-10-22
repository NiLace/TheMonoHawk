// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
// wrongnote_probe.cpp -- how often does the plugin publish a WRONG note with confidence?
//
// A string held under 9 Hz rumble (the case that made the coarse stage pick another note, measured
// in tools/rumble_bench.cpp), re-plucked every 2 s, through the loaded .so. Counts the blocks that
// publish voiced = 1, and among them those whose note is not the one played.
// run: wrongnote_probe <plugin.so> <note_hz> <rumble> [min_voiced]
//      exit 0 = no wrong-note block, and at least min_voiced (fraction, default 0) of the blocks
//      voiced -- which also catches the other failure under rumble: going mute.
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
constexpr int BLOCK = 256;
constexpr int P_IN = 0, P_OUT = 1, P_NOTE = 4, P_VOICED = 8, P_ENABLED = 22, P_N = 23;
} // namespace
int main(int argc, char** argv)
{
    if (argc < 4) { std::fprintf(stderr, "usage: %s <plugin.so> <note_hz> <rumble> [min_voiced]\n", argv[0]); return 2; }
    const double f0 = std::atof(argv[2]), rumble = std::atof(argv[3]);
    const double min_voiced = argc > 4 ? std::atof(argv[4]) : 0.0;
    long blocks = 0;
    const int midi = (int)std::lround(69 + 12 * std::log2(f0 / 440.0));
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { std::fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
    auto get = (const Desc* (*)(uint32_t))dlsym(lib, "lv2_descriptor");
    const Desc* d = get ? get(0) : nullptr;
    if (!d) return 2;
    void* h = d->instantiate(d, SR, "", nullptr);
    std::vector<float> ctrl(P_N, 0.0f);
    ctrl[3] = -20.0f; ctrl[9] = 2.0f; ctrl[13] = 440.0f; ctrl[P_ENABLED] = 1.0f; ctrl[19] = 0.5f;
    for (int p = 2; p < P_N; ++p) d->connect_port(h, (uint32_t)p, &ctrl[p]);
    std::vector<float> in(BLOCK), out(BLOCK);
    d->connect_port(h, P_IN, in.data());
    d->connect_port(h, P_OUT, out.data());
    d->activate(h);
    long voiced = 0, wrong = 0;
    double ph[10] = {};
    auto next = std::chrono::steady_clock::now();
    const auto blk = std::chrono::nanoseconds((long)(BLOCK * 1e9 / SR));
    for (long n0 = 0; n0 < (long)(12.0 * SR); n0 += BLOCK) {
        for (int i = 0; i < BLOCK; ++i) {
            const double T = (double)(n0 + i) / SR, t = std::fmod(T, 2.0);
            double s = rumble * std::sin(2 * M_PI * 9.0 * T);
            for (int m = 1; m <= 10; ++m) {
                s += 0.4 / m * std::exp(-(0.8 + 0.3 * m) * t) * std::sin(ph[m - 1]);
                ph[m - 1] += 2 * M_PI * m * f0 * std::sqrt(1 + 1e-4 * m * m) / SR;
                if (ph[m - 1] > 2 * M_PI) ph[m - 1] -= 2 * M_PI;
            }
            in[i] = (float)s;
        }
        d->run(h, (uint32_t)BLOCK);
        ++blocks;
        if (ctrl[P_VOICED] > 0.5f) { ++voiced; if ((int)std::lround(ctrl[P_NOTE]) != midi) ++wrong; }
        next += blk;
        std::this_thread::sleep_until(next);
    }
    if (d->deactivate) d->deactivate(h);
    d->cleanup(h);
    std::printf("%s %.2f Hz, rumble %.2f: %ld voiced blocks of %ld, %ld of them the WRONG note\n", argv[1], f0, rumble, voiced, blocks, wrong);
    const bool ok = wrong == 0 && voiced >= (long)(min_voiced * blocks);
    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
