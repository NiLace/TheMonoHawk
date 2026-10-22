// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
// hostile_probe.cpp -- what a misbehaving host, UI or signal chain can do to the loaded .so.
//
// Each case is one thing the plugin must survive, checked through the real plugin as a host drives it:
//   input-nan / input-inf   one non-finite input sample in the middle of a held note: the reading must
//                           come back within a second, and the level meter must not stay pinned;
//   dim-nan / dim-inf       one block with a non-finite Dim Amount: the output must be finite again
//                           at once, and back at the requested dim;
//   dim-over                Dim Amount above its TTL maximum (0 dB): the plugin must not amplify;
//   activate-twice          activate() twice without deactivate() in between (a host contract breach)
//                           must not abort the process;
//   rate                    a sample rate of 0, negative or absurd must make instantiate() fail
//                           cleanly (return NULL), not abort.
// run: hostile_probe <plugin.so> <case>      exit 0 = PASS
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <limits>
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
constexpr int P_IN = 0, P_OUT = 1, P_ACTIVE = 2, P_DIM = 3, P_VOICED = 8, P_LEVEL = 18;
constexpr int P_ENABLED = 22, P_N = 23;

struct Host {
    const Desc* d = nullptr;
    void* h = nullptr;
    std::vector<float> ctrl = std::vector<float>(P_N, 0.0f);
    std::vector<float> in = std::vector<float>(BLOCK), out = std::vector<float>(BLOCK);
    double ph[3] = {};
    std::chrono::steady_clock::time_point next = std::chrono::steady_clock::now();

    bool open(const Desc* desc)
    {
        d = desc;
        h = d->instantiate(d, SR, "", nullptr);
        if (!h) return false;
        ctrl[P_DIM] = -12.0f; ctrl[9] = 2.0f; ctrl[13] = 440.0f; ctrl[P_ENABLED] = 1.0f; ctrl[19] = 0.5f;
        for (int p = 2; p < P_N; ++p) d->connect_port(h, (uint32_t)p, &ctrl[p]);
        d->connect_port(h, P_IN, in.data());
        d->connect_port(h, P_OUT, out.data());
        return true;
    }
    // A held A2 (110 Hz, three partials, peak 0.4), one block of it, paced in real time so the
    // worker sees the stream as it would in a host.
    void tone_block()
    {
        for (int i = 0; i < BLOCK; ++i) {
            double s = 0.0;
            for (int m = 1; m <= 3; ++m) {
                s += 0.2 / m * std::sin(ph[m - 1]);
                ph[m - 1] += 2 * M_PI * m * 110.0 / SR;
                if (ph[m - 1] > 2 * M_PI) ph[m - 1] -= 2 * M_PI;
            }
            in[i] = (float)s;
        }
    }
    void run()
    {
        d->run(h, (uint32_t)BLOCK);
        next += std::chrono::nanoseconds((long)(BLOCK * 1e9 / SR));
        std::this_thread::sleep_until(next);
    }
    void close()
    {
        if (d->deactivate) d->deactivate(h);
        d->cleanup(h);
    }
};

int blocks(double seconds) { return (int)(seconds * SR / BLOCK); }

// One bad input sample at 1.5 s into a held note; afterwards the note must read again.
int input_case(const Desc* d, float bad)
{
    Host H;
    if (!H.open(d)) { std::printf("instantiate failed\n"); return 1; }
    d->activate(H.h);
    for (int b = 0; b < blocks(1.5); ++b) { H.tone_block(); H.run(); }
    H.tone_block();
    H.in[BLOCK / 2] = bad;
    H.run();
    for (int b = 0; b < blocks(1.0); ++b) { H.tone_block(); H.run(); }   // one second to recover
    int voiced = 0, n = 0;
    float level_max = 0.0f;
    for (int b = 0; b < blocks(2.0); ++b) {
        H.tone_block(); H.run();
        ++n;
        if (H.ctrl[P_VOICED] > 0.5f) ++voiced;
        if (!(H.ctrl[P_LEVEL] <= level_max)) level_max = H.ctrl[P_LEVEL];
    }
    H.close();
    // The tone peaks at ~0.33, so a level of 0.99 or more (or NaN) is the meter pinned by the bad sample.
    const bool ok = voiced >= n * 9 / 10 && level_max < 0.99f;
    std::printf("input %g: voiced %d of %d blocks after recovery, level max %.3f\n%s\n",
                (double)bad, voiced, n, (double)level_max, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// One block with a bad Dim Amount, dim engaged; then -12 dB again.
int dim_case(const Desc* d, float bad, bool whole_run)
{
    Host H;
    if (!H.open(d)) { std::printf("instantiate failed\n"); return 1; }
    H.ctrl[P_ACTIVE] = 1.0f;
    d->activate(H.h);
    for (int b = 0; b < blocks(0.3); ++b) { H.tone_block(); H.run(); }
    H.ctrl[P_DIM] = bad;
    H.tone_block(); H.run();
    if (!whole_run) H.ctrl[P_DIM] = -12.0f;
    for (int b = 0; b < blocks(0.2); ++b) { H.tone_block(); H.run(); }   // the 10 ms smoother settles
    long nonfinite = 0;
    double gain_max = 0.0;
    for (int b = 0; b < blocks(0.5); ++b) {
        H.tone_block(); H.run();
        for (int i = 0; i < BLOCK; ++i) {
            if (!std::isfinite(H.out[i])) { ++nonfinite; continue; }
            if (std::fabs(H.in[i]) > 0.05f)
                gain_max = std::max(gain_max, std::fabs((double)H.out[i] / H.in[i]));
        }
    }
    H.close();
    const double want = whole_run ? 1.0 : std::pow(10.0, -12.0 / 20.0);
    const bool ok = nonfinite == 0 && gain_max <= want * 1.001 && gain_max >= want * 0.999;
    std::printf("dim %g%s: %ld non-finite output samples, gain %.4f (want %.4f)\n%s\n", (double)bad,
                whole_run ? " held" : " for one block", nonfinite, gain_max, want, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

int activate_twice(const Desc* d)
{
    Host H;
    if (!H.open(d)) { std::printf("instantiate failed\n"); return 1; }
    d->activate(H.h);
    d->activate(H.h);                       // breach: no deactivate() in between
    for (int b = 0; b < blocks(0.5); ++b) { H.tone_block(); H.run(); }
    d->deactivate(H.h);
    d->deactivate(H.h);                     // and twice the other way
    d->activate(H.h);
    for (int b = 0; b < blocks(0.2); ++b) { H.tone_block(); H.run(); }
    H.close();
    std::printf("activate twice, deactivate twice: survived\nPASS\n");
    return 0;
}

int rate_case(const Desc* d)
{
    const double rates[] = { 0.0, -48000.0, std::numeric_limits<double>::quiet_NaN(),
                             std::numeric_limits<double>::infinity(), 1.0e12 };
    int bad = 0;
    for (const double r : rates) {
        void* h = d->instantiate(d, r, "", nullptr);
        std::printf("rate %g: %s\n", r, h ? "instantiated (should have refused)" : "refused");
        if (h) { ++bad; d->cleanup(h); }
    }
    // and an ordinary rate still works
    void* h = d->instantiate(d, 44100.0, "", nullptr);
    std::printf("rate 44100: %s\n", h ? "instantiated" : "REFUSED");
    if (!h) ++bad; else d->cleanup(h);
    std::printf("%s\n", bad == 0 ? "PASS" : "FAIL");
    return bad == 0 ? 0 : 1;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) { std::fprintf(stderr, "usage: %s <plugin.so> <case>\n", argv[0]); return 2; }
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { std::fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
    auto get = (const Desc* (*)(uint32_t))dlsym(lib, "lv2_descriptor");
    const Desc* d = get ? get(0) : nullptr;
    if (!d) return 2;
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const char* c = argv[2];
    if (!std::strcmp(c, "input-nan"))      return input_case(d, nan);
    if (!std::strcmp(c, "input-inf"))      return input_case(d, inf);
    if (!std::strcmp(c, "dim-nan"))        return dim_case(d, nan, false);
    if (!std::strcmp(c, "dim-inf"))        return dim_case(d, inf, false);
    if (!std::strcmp(c, "dim-over"))       return dim_case(d, 20.0f, true);
    if (!std::strcmp(c, "activate-twice")) return activate_twice(d);
    if (!std::strcmp(c, "rate"))           return rate_case(d);
    std::fprintf(stderr, "unknown case %s\n", c);
    return 2;
}
