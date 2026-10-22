// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
// thread_probe.cpp -- activate() survives a system that refuses it a thread.
//
// With the process limit at 1 (RLIMIT_NPROC: the user already has more processes), creating the analysis thread fails;
// std::thread then throws, and an exception out of activate() would terminate the HOST. The plugin must instead run
// without analysis: activate, process audio (the output is the input), deactivate and clean up.
// Negative arm: the .so built with -DHAWK_THREAD_UNGUARDED=1 must abort here (test/meson.build).
// run: thread_probe <plugin.so>      exit 0 = PASS
#include <cstdint>
#include <cstdio>
#include <dlfcn.h>
#include <sys/resource.h>
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
constexpr int P_IN = 0, P_OUT = 1;
constexpr int P_ENABLED = 22, P_N = 23;
}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: %s <plugin.so>\n", argv[0]); return 2; }
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { std::fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
    auto get = (const Desc* (*)(uint32_t))dlsym(lib, "lv2_descriptor");
    const Desc* d = get ? get(0) : nullptr;
    if (!d) return 2;
    void* h = d->instantiate(d, 48000.0, "", nullptr);
    if (!h) { std::printf("FAIL: instantiate\n"); return 1; }
    std::vector<float> ctrl(P_N, 0.0f), in(BLOCK, 0.25f), out(BLOCK, 0.0f);
    ctrl[3] = -12.0f; ctrl[9] = 2.0f; ctrl[13] = 440.0f; ctrl[P_ENABLED] = 1.0f; ctrl[19] = 0.5f;
    for (int p = 2; p < P_N; ++p) d->connect_port(h, (uint32_t)p, &ctrl[p]);
    d->connect_port(h, P_IN, in.data());
    d->connect_port(h, P_OUT, out.data());
    const rlimit one = { 1, 1 };
    if (setrlimit(RLIMIT_NPROC, &one) != 0) { std::printf("not run: setrlimit refused\n"); return 77; }
    d->activate(h);                                        // the worker cannot be created here
    for (int b = 0; b < 50; ++b) d->run(h, BLOCK);
    d->deactivate(h);
    d->cleanup(h);
    std::printf("activate() with no thread to be had: the host is still here, the audio %s\nPASS\n",
                out[BLOCK - 1] == in[BLOCK - 1] ? "passes" : "is processed");
    return 0;
}
