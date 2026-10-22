// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
// alloc_lv2_probe.cpp -- heap allocations of the WHOLE plugin, worker thread included, while it
// reads a strummed chord.
//
// alloc_probe.cpp counts analyze() and refine() in isolation; the worker loop lives in the LV2
// wrapper and never runs there. This one loads the .so the way a host does and interposes the
// global operator new: the plugin's calls resolve to the executable's definition, so every
// allocation made by any thread of the plugin is counted. Link with -rdynamic or they will not.
//
// The signal is a synthetic strum in Drop D (tuning index 1) -- six strings with a stretched,
// decaying partial series, re-struck every 2 s.
// The first STRUM_WARM seconds are not counted (FFT plans and first-touch buffers).
//
// build: g++ -std=c++17 -O2 -rdynamic tools/alloc_lv2_probe.cpp -o alloc_lv2_probe -ldl -lpthread
// run:   alloc_lv2_probe <plugin.so>      (exit 0 = no allocation counted; a test in test/meson.build)

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <new>
#include <thread>
#include <vector>

static std::atomic<long> g_new{0}, g_bytes{0};
static std::atomic<bool> g_count{false};
void* operator new(std::size_t n)
{
    if (g_count.load(std::memory_order_relaxed)) { ++g_new; g_bytes += (long)n; }
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n) { return operator new(n); }
// Every operator new above takes its memory from malloc, so free() IS the matching release; GCC
// cannot see that across a replaced global operator and warns.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#pragma GCC diagnostic pop

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

constexpr double SR         = 48000.0;
constexpr int    BLOCK      = 256;
constexpr double SECONDS    = 12.0;
constexpr double STRUM_WARM = 3.0;
constexpr int P_IN = 0, P_OUT = 1, P_TUNING = 14, P_ENABLED = 22, P_N = 23;   // from lv2/hawk.ttl

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: %s <plugin.so>\n", argv[0]); return 2; }
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { std::fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
    auto get = (const Desc* (*)(uint32_t))dlsym(lib, "lv2_descriptor");
    const Desc* d = get ? get(0) : nullptr;
    if (!d) { std::fprintf(stderr, "no descriptor\n"); return 2; }

    void* h = d->instantiate(d, SR, "", nullptr);
    std::vector<float> ctrl(P_N, 0.0f);
    ctrl[3]  = -20.0f;  // dim dB
    ctrl[9]  = 2.0f;    // in-tune tolerance
    ctrl[13] = 440.0f; ctrl[P_ENABLED] = 1.0f;  // A4
    ctrl[P_TUNING] = 1.0f;  // Drop D
    ctrl[19] = 0.5f;    // sensitivity
    for (int p = 2; p < P_N; ++p) d->connect_port(h, (uint32_t)p, &ctrl[p]);
    std::vector<float> in(BLOCK), out(BLOCK);
    d->connect_port(h, P_IN, in.data());
    d->connect_port(h, P_OUT, out.data());
    d->activate(h);

    // Drop D, low to high: D2 A2 D3 G3 B3 E4 (MIDI 38 45 50 55 59 64).
    const int midi[6] = { 38, 45, 50, 55, 59, 64 };
    const double B = 1.0e-4;   // inharmonicity, as on a real string
    double phase[6][8] = {};
    const long total = (long)(SECONDS * SR);
    for (long n0 = 0; n0 < total; n0 += BLOCK) {
        if (!g_count && n0 >= (long)(STRUM_WARM * SR)) g_count = true;
        for (int i = 0; i < BLOCK; ++i) {
            const double t  = std::fmod((double)(n0 + i) / SR, 2.0);   // re-strum every 2 s
            const double env = std::exp(-1.5 * t);
            double s = 0.0;
            for (int k = 0; k < 6; ++k) {
                const double f0 = 440.0 * std::pow(2.0, (midi[k] - 69) / 12.0);
                for (int m = 1; m <= 8; ++m) {
                    const double fm = m * f0 * std::sqrt(1.0 + B * m * m);
                    s += env * (0.08 / m) * std::sin(phase[k][m - 1]);
                    phase[k][m - 1] += 2.0 * M_PI * fm / SR;
                    if (phase[k][m - 1] > 2.0 * M_PI) phase[k][m - 1] -= 2.0 * M_PI;
                }
            }
            in[i] = (float)s;
        }
        d->run(h, (uint32_t)BLOCK);
        // real time, so the worker analyses at the rate it would in a host
        std::this_thread::sleep_for(std::chrono::microseconds((int)(BLOCK * 1e6 / SR)));
    }
    g_count = false;
    if (d->deactivate) d->deactivate(h);
    d->cleanup(h);

    const double secs = SECONDS - STRUM_WARM;
    std::printf("%s\n  strummed Drop D, %.0f s counted after %.0f s of warm-up\n", argv[1], secs, STRUM_WARM);
    std::printf("  allocations: %ld  (%.1f /s)\n", g_new.load(), g_new.load() / secs);
    std::printf("  bytes      : %ld  (%.3f MB/s)\n", g_bytes.load(), g_bytes.load() / secs / 1e6);
    std::printf("\n%s\n", g_new.load() == 0
        ? "PASS: no heap traffic in steady state, worker thread included."
        : "FAIL: the plugin allocates while it runs.");
    return g_new.load() == 0 ? 0 : 1;
}
