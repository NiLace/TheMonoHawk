// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
// rate_probe.cpp -- the chromatic reading at the host's sample rate.
//
// Every other probe runs at 48 kHz, and the analysis does not run at the host's rate: above 50 kHz
// the input is decimated (88.2/96 kHz by 2, 176.4/192 kHz by 4). This plays, at one rate, a held note
// (four partials) for each of B0, E2, A2, D3, G3, B3, E4, each detuned by +10 c and then by -7 c, through
// the loaded .so at real-time pace, and reads the published note and cents over the last 0.5 s of
// each. Pass: the right note in at least 90 % of those blocks, and their median cents within 0.3 c of
// the detune.
// With "sweep" as third argument it is the high-notes sweep instead: E2 to F#6 (82-1480 Hz) every three semitones, the 2nd partial at -6 dB.
// run: rate_probe <plugin.so> <rate> [sweep]      exit 0 = PASS
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
constexpr int BLOCK = 256;
constexpr int P_IN = 0, P_OUT = 1, P_NOTE = 4, P_CENTS = 5, P_VOICED = 8;
constexpr int P_ENABLED = 22, P_N = 23;
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) { std::fprintf(stderr, "usage: %s <plugin.so> <rate> [sweep]\n", argv[0]); return 2; }
    const double SR = std::atof(argv[2]);
    const bool sweep = argc > 3 && std::strcmp(argv[3], "sweep") == 0;
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { std::fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
    auto get = (const Desc* (*)(uint32_t))dlsym(lib, "lv2_descriptor");
    const Desc* d = get ? get(0) : nullptr;
    if (!d) return 2;
    void* h = d->instantiate(d, SR, "", nullptr);
    if (!h) { std::printf("instantiate refused %.0f Hz\nFAIL\n", SR); return 1; }
    std::vector<float> ctrl(P_N, 0.0f);
    ctrl[3] = -12.0f; ctrl[9] = 2.0f; ctrl[13] = 440.0f; ctrl[P_ENABLED] = 1.0f; ctrl[19] = 0.5f;
    for (int p = 2; p < P_N; ++p) d->connect_port(h, (uint32_t)p, &ctrl[p]);
    std::vector<float> in(BLOCK), out(BLOCK);
    d->connect_port(h, P_IN, in.data());
    d->connect_port(h, P_OUT, out.data());
    d->activate(h);

    std::vector<int> midi = { 23, 40, 45, 50, 55, 59, 64 };
    if (sweep) { midi.clear(); for (int m = 40; m <= 88; m += 3) midi.push_back(m); midi.push_back(90); }
    // partial amplitudes: 1/k, or for the sweep the 2nd at -6 dB and a 3rd and 4th below it
    const double amp_default[4] = { 0.25, 0.125, 0.0833, 0.0625 };
    const double amp_sweep[4]   = { 0.25, 0.125, 0.06, 0.03 };
    const double* amp = sweep ? amp_sweep : amp_default;
    const double detune[] = { +10.0, -7.0 };
    constexpr double NOTE_S = 1.4, JUDGE_S = 0.5;
    int bad = 0;
    auto next = std::chrono::steady_clock::now();
    double ph[4] = {};
    for (const int m : midi)
        for (const double c : detune) {
            const double f0 = 440.0 * std::pow(2.0, (m - 69 + c / 100.0) / 12.0);
            const long len = (long)(NOTE_S * SR), judge_from = (long)((NOTE_S - JUDGE_S) * SR);
            std::vector<double> got;
            long blocks = 0, right = 0;
            for (long n0 = 0; n0 < len; n0 += BLOCK) {
                for (int i = 0; i < BLOCK; ++i) {
                    double s = 0.0;
                    for (int k = 0; k < 4; ++k) {
                        s += amp[k] * std::sin(ph[k]);
                        ph[k] += 2 * M_PI * (k + 1) * f0 / SR;
                        if (ph[k] > 2 * M_PI) ph[k] -= 2 * M_PI;
                    }
                    in[i] = (float)s;
                }
                d->run(h, (uint32_t)BLOCK);
                if (n0 >= judge_from) {
                    ++blocks;
                    if (ctrl[P_VOICED] > 0.5f && (int)std::lround(ctrl[P_NOTE]) == m) {
                        ++right;
                        got.push_back(ctrl[P_CENTS]);
                    }
                }
                next += std::chrono::nanoseconds((long)(BLOCK * 1e9 / SR));
                std::this_thread::sleep_until(next);
            }
            double med = NAN;
            if (!got.empty()) {
                std::nth_element(got.begin(), got.begin() + got.size() / 2, got.end());
                med = got[got.size() / 2];
            }
            const bool ok = right * 10 >= blocks * 9 && std::fabs(med - c) <= 0.3;
            if (!ok) ++bad;
            std::printf("%.0f Hz  midi %d %+5.1f c: right note %3ld/%3ld blocks, median %+6.2f c %s\n", SR, m, c,
                        right, blocks, med, ok ? "" : "<-- FAIL");
        }
    d->deactivate(h);
    d->cleanup(h);
    std::printf("%s\n", bad == 0 ? "PASS" : "FAIL");
    return bad == 0 ? 0 : 1;
}
