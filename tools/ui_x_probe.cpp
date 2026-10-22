// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
// ui_x_probe.cpp -- gate: an X error on the GUI's own connection does not end the host.
//
// Xlib's default error handler exit()s the process. Two things a host can do provoke an error on the GUI's connection:
// hand it a parent window the server does not know (BadWindow), or destroy the parent while the editor still draws
// (BadDrawable/BadWindow on the next frame and at cleanup). The probe loads the GUI module like a host, does both, and
// must still be running at the end; the host's own handler must be back in place afterwards.
// Negative arm: the module built with -DHAWK_UI_NO_X_HANDLER=1 must die here (test/meson.build).
// Needs an X display; without one it says so and exits 77. Its windows are never mapped.
// run: hawk_ui_x_probe <ui module> <bundle dir>      exit 0 pass, 77 not run, anything else fail
#include <lv2/atom/atom.h>
#include <lv2/core/lv2.h>
#include <lv2/options/options.h>
#include <lv2/ui/ui.h>
#include <lv2/urid/urid.h>

#include <cstring>
#include <string>
#include <vector>

#include <X11/Xlib.h>

#include <cstdint>
#include <cstdio>
#include <dlfcn.h>

namespace {
void no_write(LV2UI_Controller, uint32_t, uint32_t, uint32_t, const void*) {}
std::vector<std::string> g_uris;
LV2_URID map_uri(LV2_URID_Map_Handle, const char* uri)
{
    for (std::size_t i = 0; i < g_uris.size(); ++i) if (g_uris[i] == uri) return (LV2_URID)(i + 1);
    g_uris.emplace_back(uri);
    return (LV2_URID)g_uris.size();
}
int g_asked_w = 0, g_asked_h = 0;
int resize_cb(LV2UI_Feature_Handle, int w, int h) { g_asked_w = w; g_asked_h = h; return 0; }
}  // namespace

int main(int argc, char** argv)
{
    if (argc < 3) { std::fprintf(stderr, "usage: %s <ui module> <bundle dir>\n", argv[0]); return 2; }
    Display* d = XOpenDisplay(nullptr);
    if (!d) { std::printf("not run: no X display\n"); return 77; }
    // Xlib's DEFAULT handler stays installed, as in a host without one of its own: the one that exits
    const XErrorHandler xlib_default = XSetErrorHandler(nullptr);
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { std::printf("FAIL: %s\n", dlerror()); return 1; }
    auto get = reinterpret_cast<const LV2UI_Descriptor* (*)(uint32_t)>(dlsym(lib, "lv2ui_descriptor"));
    const LV2UI_Descriptor* desc = get ? get(0) : nullptr;
    const auto* idle = desc ? static_cast<const LV2UI_Idle_Interface*>(desc->extension_data(LV2_UI__idleInterface))
                            : nullptr;
    if (!desc || !idle) { std::printf("FAIL: no UI descriptor or idle interface\n"); return 1; }

    // 1. a parent the server does not know: the editor may refuse to open, the host must live
    {
        const Window bogus = (Window)0x7ffffff0;
        LV2_Feature fp = { LV2_UI__parent, reinterpret_cast<void*>(static_cast<uintptr_t>(bogus)) };
        const LV2_Feature* feats[] = { &fp, nullptr };
        LV2UI_Widget w = nullptr;
        LV2UI_Handle h = desc->instantiate(desc, "probe", argv[2], no_write, nullptr, &w, feats);
        if (h) { idle->idle(h); desc->cleanup(h); }
        std::printf("ok   bogus parent: the host is still here (editor %s)\n", h ? "opened" : "refused");
    }
    // 2. the parent destroyed while the editor still draws
    {
        const Window parent = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 1200, 164, 0, 0, 0);   // unmapped
        XSync(d, False);
        LV2_Feature fp = { LV2_UI__parent, reinterpret_cast<void*>(static_cast<uintptr_t>(parent)) };
        const LV2_Feature* feats[] = { &fp, nullptr };
        LV2UI_Widget w = nullptr;
        LV2UI_Handle h = desc->instantiate(desc, "probe", argv[2], no_write, nullptr, &w, feats);
        if (!h) { std::printf("FAIL: instantiate with a real parent\n"); return 1; }
        for (int i = 0; i < 3; ++i) idle->idle(h);
        XDestroyWindow(d, parent);
        XSync(d, False);
        const float one = 1.0f;
        for (int i = 0; i < 5; ++i) { desc->port_event(h, 8, sizeof(float), 0, &one); idle->idle(h); }
        desc->cleanup(h);
        std::printf("ok   parent destroyed before cleanup: the host is still here\n");
    }
    // 3. the host's GUI scale: ui:scaleFactor 2 opens the editor at twice its size (2400 x 328)
    {
        const Window parent = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 2400, 328, 0, 0, 0);   // unmapped
        XSync(d, False);
        LV2_URID_Map map = { nullptr, map_uri };
        const float two = 2.0f;
        const LV2_Options_Option opts[] = {
            { LV2_OPTIONS_INSTANCE, 0, map_uri(nullptr, LV2_UI__scaleFactor), sizeof(float),
              map_uri(nullptr, LV2_ATOM__Float), &two },
            { LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, nullptr },
        };
        LV2UI_Resize rz = { nullptr, resize_cb };
        LV2_Feature fp = { LV2_UI__parent, reinterpret_cast<void*>(static_cast<uintptr_t>(parent)) };
        LV2_Feature fm = { LV2_URID__map, &map };
        LV2_Feature fo = { LV2_OPTIONS__options, const_cast<LV2_Options_Option*>(opts) };
        LV2_Feature fr = { LV2_UI__resize, &rz };
        const LV2_Feature* feats[] = { &fp, &fm, &fo, &fr, nullptr };
        LV2UI_Widget w = nullptr;
        LV2UI_Handle h = desc->instantiate(desc, "probe", argv[2], no_write, nullptr, &w, feats);
        if (!h) { std::printf("FAIL: instantiate with options\n"); return 1; }
        desc->cleanup(h);
        XDestroyWindow(d, parent);
        if (g_asked_w != 2400 || g_asked_h != 328) {
            std::printf("FAIL: ui:scaleFactor 2 asked the host for %d x %d, not 2400 x 328\n", g_asked_w, g_asked_h);
            return 1;
        }
        std::printf("ok   ui:scaleFactor 2: the editor asks for 2400 x 328\n");
    }
    // 4. the GUI restored the handler it found
    if (XSetErrorHandler(nullptr) != xlib_default) { std::printf("FAIL: the GUI left its X error handler installed\n"); return 1; }
    dlclose(lib);
    XCloseDisplay(d);
    std::printf("PASS\n");
    return 0;
}
