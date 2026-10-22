// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
// cpu_lv2_probe.cpp -- what the loaded plugin costs, in % of one core, worker thread included.
//
// The analysis runs on the plugin's own worker thread, so timing run() alone measures almost
// nothing. This loads the .so the way a host does, feeds a signal IN REAL TIME (the worker must
// see audio at the rate a host delivers it, or the ring overflows and the run measures gap
// handling), and reports two numbers from the kernel's per-thread accounting:
//   audio  -- CPU of the calling thread inside run() (the host's audio thread would pay this)
//   worker -- CPU of every other thread of the process (schedstat, ns) while the plugin ran
// both divided by the audio time. The signal synthesis is excluded from `audio` by timing run()
// with CLOCK_THREAD_CPUTIME_ID around the call only.
//
// Scenes: strum (six strings, Drop D, re-struck every 2 s) and
// single (one A2 string, plucked every 2 s: the chromatic path) and silence (one A2 pluck, then
// digital zeros; only the time from REST_FROM_S on is counted, which is what the plugin costs at rest on
// a track). Compare two builds PAIRED, in the same batch, alternating -- two batches at different
// moments are not comparable.
//
// build: g++ -std=c++17 -O2 tools/cpu_lv2_probe.cpp -o cpu_lv2_probe -ldl -lpthread
// run:   cpu_lv2_probe <plugin.so> <strum|single|silence> [seconds] [rate] [max_worker_pct]
//        with max_worker_pct it is a gate: exit 1 if the worker costs more (meson runs it at rest).

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <dlfcn.h>
#include <string>
#include <thread>
#include <unistd.h>
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
constexpr int P_IN = 0, P_OUT = 1, P_TUNING = 14, P_ENABLED = 22, P_N = 23;   // from lv2/hawk.ttl

double ts(const timespec& t) { return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec; }

// Time on CPU (seconds) of every thread of this process except `skip_tid`, from schedstat, whose
// first field is nanoseconds on a CPU. (utime/stime in stat count 10 ms ticks: too coarse for a
// worker that costs a few percent over a few seconds.)
double other_threads_cpu(pid_t skip_tid)
{
    double total = 0.0;
    DIR* d = opendir("/proc/self/task");
    if (!d) return 0.0;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        if (std::atoi(e->d_name) == skip_tid) continue;
        const std::string path = std::string("/proc/self/task/") + e->d_name + "/schedstat";
        FILE* f = std::fopen(path.c_str(), "r");
        if (!f) continue;
        unsigned long long ns = 0;
        if (std::fscanf(f, "%llu", &ns) == 1) total += 1e-9 * (double)ns;
        std::fclose(f);
    }
    closedir(d);
    return total;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) { std::fprintf(stderr, "usage: %s <plugin.so> <strum|single|silence> [seconds] [rate]\n", argv[0]); return 2; }
    const bool   strum   = std::strcmp(argv[2], "strum") == 0;
    const bool   silence = std::strcmp(argv[2], "silence") == 0;
    // at rest: the meter and the high-pass take a while to decay, denormals arrived at ~7 s
    constexpr double REST_FROM_S = 16.0;
    const double seconds = argc > 3 ? std::atof(argv[3]) : 8.0;
    const double SR      = argc > 4 ? std::atof(argv[4]) : 48000.0;
    const double max_pct = argc > 5 ? std::atof(argv[5]) : -1.0;

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

    const pid_t me = (pid_t)gettid();
    double worker0 = other_threads_cpu(me);
    d->activate(h);

    const int  midi_strum[6] = { 38, 45, 50, 55, 59, 64 };   // Drop D, low to high
    const int  midi_single[1] = { 45 };                        // A2
    const int* midi = strum ? midi_strum : midi_single;
    const int  nstr = strum ? 6 : 1;
    const double B = 1.0e-4;
    double phase[6][8] = {};
    double audio_cpu = 0.0;
    const long total = (long)(seconds * SR);
    auto next = std::chrono::steady_clock::now();
    const auto blk = std::chrono::nanoseconds((long)(BLOCK * 1e9 / SR));
    const long rest_from = silence ? (long)(REST_FROM_S * SR) : 0;
    if (silence && total <= rest_from + (long)SR) { std::fprintf(stderr, "silence needs > %.0f s\n", REST_FROM_S + 1); return 2; }
    for (long n0 = 0; n0 < total; n0 += BLOCK) {
        if (silence && n0 <= rest_from && n0 + BLOCK > rest_from) { worker0 = other_threads_cpu(me); audio_cpu = 0.0; }
        for (int i = 0; i < BLOCK; ++i) {
            if (silence && n0 + i >= (long)SR) { in[i] = 0.0f; continue; }
            const double t   = std::fmod((double)(n0 + i) / SR, 2.0);
            const double env = std::exp(-1.5 * t);
            double s = 0.0;
            for (int k = 0; k < nstr; ++k) {
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
        timespec a, b;
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &a);
        d->run(h, (uint32_t)BLOCK);
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &b);
        audio_cpu += ts(b) - ts(a);
        next += blk;
        std::this_thread::sleep_until(next);   // absolute schedule: no drift from the synthesis
    }
    // read the worker while it is still alive: a joined thread leaves /proc/self/task
    const double worker1 = other_threads_cpu(me);
    if (d->deactivate) d->deactivate(h);
    d->cleanup(h);

    const double counted = seconds - (silence ? REST_FROM_S : 0.0);
    std::printf("%s %s %.0f s @ %.0f Hz  audio %.4f %%  worker %.3f %%  of one core\n",
                argv[1], argv[2], counted, SR,
                100.0 * audio_cpu / counted, 100.0 * (worker1 - worker0) / counted);
    if (max_pct >= 0.0) {
        const bool ok = 100.0 * (worker1 - worker0) / counted <= max_pct;
        std::printf("%s (worker at most %.2f %%)\n", ok ? "PASS" : "FAIL", max_pct);
        return ok ? 0 : 1;
    }
    return 0;
}
