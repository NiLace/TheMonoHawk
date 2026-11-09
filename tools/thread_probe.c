// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
// thread_probe.c -- activate() survives a system that refuses it a thread.
//
// With the process limit at 1 (RLIMIT_NPROC: the user already has more processes), creating the analysis thread fails;
// a plugin that does not handle that failure (an uncaught exception, an abort) takes the HOST down with it. The plugin
// must instead run without analysis: activate, process audio (the output is the input), deactivate and clean up.
// Negative arm: the .so built with -DHAWK_THREAD_UNGUARDED=1 must abort here (test/meson.build).
// run: thread_probe <plugin.so>      exit 0 = PASS
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/resource.h>

typedef struct Desc {
    const char* URI;
    void* (*instantiate)(const void*, double, const char*, const void* const*);
    void  (*connect_port)(void*, uint32_t, void*);
    void  (*activate)(void*);
    void  (*run)(void*, uint32_t);
    void  (*deactivate)(void*);
    void  (*cleanup)(void*);
    const void* (*extension_data)(const char*);
} Desc;
enum { BLOCK = 256 };
enum { P_IN = 0, P_OUT = 1 };
enum { P_ENABLED = 22, P_N = 23 };

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <plugin.so>\n", argv[0]); return 2; }
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
    const Desc* (*get)(uint32_t);
    *(void**)&get = dlsym(lib, "lv2_descriptor");   // POSIX's way to take a function from dlsym
    const Desc* d = get ? get(0) : NULL;
    if (!d) return 2;
    void* h = d->instantiate(d, 48000.0, "", NULL);
    if (!h) { printf("FAIL: instantiate\n"); return 1; }
    float ctrl[P_N] = { 0 }, in[BLOCK], out[BLOCK] = { 0 };
    for (int i = 0; i < BLOCK; ++i) in[i] = 0.25f;
    ctrl[3] = -12.0f; ctrl[9] = 2.0f; ctrl[13] = 440.0f; ctrl[P_ENABLED] = 1.0f; ctrl[19] = 0.5f;
    for (int p = 2; p < P_N; ++p) d->connect_port(h, (uint32_t)p, &ctrl[p]);
    d->connect_port(h, P_IN, in);
    d->connect_port(h, P_OUT, out);
    const struct rlimit one = { 1, 1 };
    if (setrlimit(RLIMIT_NPROC, &one) != 0) { printf("not run: setrlimit refused\n"); return 77; }
    d->activate(h);                                        // the worker cannot be created here
    for (int b = 0; b < 50; ++b) d->run(h, BLOCK);
    d->deactivate(h);
    d->cleanup(h);
    printf("activate() with no thread to be had: the host is still here, the audio %s\nPASS\n",
           out[BLOCK - 1] == in[BLOCK - 1] ? "passes" : "is processed");
    return 0;
}
