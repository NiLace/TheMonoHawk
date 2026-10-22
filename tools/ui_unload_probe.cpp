// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
// ui_unload_probe.cpp -- gate: the GUI module can be unloaded, and the host keeps drawing text after it is gone.
//
// Cairo keeps the GUI's font faces in a process-wide cache and may drop them long after the editor closed and the
// host dlclosed the module; a face whose release function lives in the module then calls into unmapped code and the
// host dies (measured on the Omega Cake; lv2/hawk_ui_fonts.h is the fix). This probe loads the
// module, opens the editor twice (drawing the states that use the type: a note, REF, the settings window), closes it,
// dlcloses the module, checks it is really gone (RTLD_NOLOAD: a module the loader keeps mapped would pass blind),
// then draws text in 600 sizes as a host would, so cairo evicts the GUI's faces, and resets cairo's static data.
// Negative arm: the module built with -DHAWK_UI_FONT_CALLBACK_IN_MODULE=1 must CRASH here (test/meson.build).
// Needs an X display; without one it says so and exits 77 (meson: skipped). Its windows are never mapped.
// run: hawk_ui_unload_probe <ui module> <bundle dir>      exit 0 pass, 77 not run, anything else fail
#include <lv2/core/lv2.h>
#include <lv2/ui/ui.h>

#include <X11/Xlib.h>
#include <cairo/cairo.h>

#include <cstdint>
#include <cstdio>
#include <dlfcn.h>

namespace {
void no_write(LV2UI_Controller, uint32_t, uint32_t, uint32_t, const void*) {}
constexpr int kEvictSizes = 600;   // cairo's scaled-font cache holds 256
constexpr uint32_t P_REFACT = 15, P_NOTE = 4, P_CENTS = 5, P_VOICED = 8;   // the same in both trees
}  // namespace

int main(int argc, char** argv)
{
    if (argc < 3) { std::fprintf(stderr, "usage: %s <ui module> <bundle dir>\n", argv[0]); return 2; }
    Display* d = XOpenDisplay(nullptr);
    if (!d) { std::printf("not run: no X display, the UI cannot run here\n"); return 77; }
    const Window parent = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 1200, 164, 0, 0, 0);   // never mapped
    XSync(d, False);
    if (!dlopen("libcairo.so.2", RTLD_NOW | RTLD_GLOBAL)) { std::printf("FAIL: %s\n", dlerror()); return 1; }
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { std::printf("FAIL: %s\n", dlerror()); return 1; }
    auto get = reinterpret_cast<const LV2UI_Descriptor* (*)(uint32_t)>(dlsym(lib, "lv2ui_descriptor"));
    const LV2UI_Descriptor* desc = get ? get(0) : nullptr;
    const auto* idle = desc ? static_cast<const LV2UI_Idle_Interface*>(desc->extension_data(LV2_UI__idleInterface))
                            : nullptr;
    if (!desc || !idle) { std::printf("FAIL: no UI descriptor or idle interface\n"); return 1; }
    LV2_Feature fp = { LV2_UI__parent, reinterpret_cast<void*>(static_cast<uintptr_t>(parent)) };
    const LV2_Feature* feats[] = { &fp, nullptr };
    for (int round = 0; round < 2; ++round) {
        LV2UI_Widget w = nullptr;
        LV2UI_Handle h = desc->instantiate(desc, "test", argv[2], no_write, nullptr, &w, feats);
        if (!h) { std::printf("FAIL: instantiate %d\n", round + 1); return 1; }
        const float note = 40.0f, cents = 7.5f, one = 1.0f;
        desc->port_event(h, P_NOTE, sizeof(float), 0, &note);
        desc->port_event(h, P_CENTS, sizeof(float), 0, &cents);
        desc->port_event(h, P_VOICED, sizeof(float), 0, &one);
        for (int i = 0; i < 3; ++i) idle->idle(h);
        desc->port_event(h, P_REFACT, sizeof(float), 0, &one);
        for (int i = 0; i < 3; ++i) idle->idle(h);
        desc->cleanup(h);
    }
    if (dlclose(lib)) { std::printf("FAIL: dlclose: %s\n", dlerror()); return 1; }
    if (dlopen(argv[1], RTLD_NOW | RTLD_NOLOAD)) {
        std::printf("FAIL: the UI module is still loaded after dlclose (the check would be blind)\n");
        return 1;
    }
    {   // the host draws text after the UI module is gone
        cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 256, 64);
        cairo_t* cr = cairo_create(s);
        cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        for (int i = 0; i < kEvictSizes; ++i) {
            cairo_set_font_size(cr, 6.0 + 0.25 * i);
            cairo_move_to(cr, 2, 30);
            cairo_show_text(cr, "host");
        }
        cairo_destroy(cr);
        cairo_surface_destroy(s);
        cairo_debug_reset_static_data();
    }
    XDestroyWindow(d, parent);
    XCloseDisplay(d);
    std::printf("ok   ui_unload_probe: opened twice, unloaded, the host drew text after\n");
    return 0;
}
