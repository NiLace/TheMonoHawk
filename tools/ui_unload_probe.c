// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
// ui_unload_probe.c -- gate: the GUI module can be unloaded, and the host keeps drawing text after it is gone.
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

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>

static void no_write(LV2UI_Controller controller, uint32_t port, uint32_t size, uint32_t protocol, const void* buf)
{
    (void)controller; (void)port; (void)size; (void)protocol; (void)buf;
}
enum { kEvictSizes = 600 };   // cairo's scaled-font cache holds 256
enum { P_REFACT = 15, P_NOTE = 4, P_CENTS = 5, P_VOICED = 8 };   // the same in both trees

int main(int argc, char** argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <ui module> <bundle dir>\n", argv[0]); return 2; }
    Display* d = XOpenDisplay(NULL);
    if (!d) { printf("not run: no X display, the UI cannot run here\n"); return 77; }
    const Window parent = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 1200, 164, 0, 0, 0);   // never mapped
    XSync(d, False);
    if (!dlopen("libcairo.so.2", RTLD_NOW | RTLD_GLOBAL)) { printf("FAIL: %s\n", dlerror()); return 1; }
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { printf("FAIL: %s\n", dlerror()); return 1; }
    const LV2UI_Descriptor* (*get)(uint32_t);
    *(void**)&get = dlsym(lib, "lv2ui_descriptor");   // POSIX's way to take a function from dlsym
    const LV2UI_Descriptor* desc = get ? get(0) : NULL;
    const LV2UI_Idle_Interface* idle = desc ? (const LV2UI_Idle_Interface*)desc->extension_data(LV2_UI__idleInterface)
                                            : NULL;
    if (!desc || !idle) { printf("FAIL: no UI descriptor or idle interface\n"); return 1; }
    LV2_Feature fp = { LV2_UI__parent, (void*)(uintptr_t)parent };
    const LV2_Feature* feats[] = { &fp, NULL };
    for (int round = 0; round < 2; ++round) {
        LV2UI_Widget w = NULL;
        LV2UI_Handle h = desc->instantiate(desc, "test", argv[2], no_write, NULL, &w, feats);
        if (!h) { printf("FAIL: instantiate %d\n", round + 1); return 1; }
        const float note = 40.0f, cents = 7.5f, one = 1.0f;
        desc->port_event(h, P_NOTE, sizeof(float), 0, &note);
        desc->port_event(h, P_CENTS, sizeof(float), 0, &cents);
        desc->port_event(h, P_VOICED, sizeof(float), 0, &one);
        for (int i = 0; i < 3; ++i) idle->idle(h);
        desc->port_event(h, P_REFACT, sizeof(float), 0, &one);
        for (int i = 0; i < 3; ++i) idle->idle(h);
        desc->cleanup(h);
    }
    if (dlclose(lib)) { printf("FAIL: dlclose: %s\n", dlerror()); return 1; }
    if (dlopen(argv[1], RTLD_NOW | RTLD_NOLOAD)) {
        printf("FAIL: the UI module is still loaded after dlclose (the check would be blind)\n");
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
    printf("ok   ui_unload_probe: opened twice, unloaded, the host drew text after\n");
    return 0;
}
