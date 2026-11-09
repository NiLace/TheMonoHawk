// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
// ui_x_probe.c -- gate: an X error on the GUI's own connection does not end the host.
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

#include <stdlib.h>
#include <string.h>

#include <X11/Xlib.h>

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>

static void no_write(LV2UI_Controller controller, uint32_t port, uint32_t size, uint32_t protocol, const void* buf)
{
    (void)controller; (void)port; (void)size; (void)protocol; (void)buf;
}
static char* g_uris[64];   // the URIs mapped so far (a handful), owned copies
static size_t g_nuris = 0;
static LV2_URID map_uri(LV2_URID_Map_Handle handle, const char* uri)
{
    (void)handle;
    for (size_t i = 0; i < g_nuris; ++i) if (!strcmp(g_uris[i], uri)) return (LV2_URID)(i + 1);
    if (g_nuris >= sizeof g_uris / sizeof g_uris[0]) return 0;   // the map is full: unmapped
    char* copy = strdup(uri);
    if (!copy) return 0;
    g_uris[g_nuris++] = copy;
    return (LV2_URID)g_nuris;
}
static int g_asked_w = 0, g_asked_h = 0;
static int resize_cb(LV2UI_Feature_Handle handle, int w, int h) { (void)handle; g_asked_w = w; g_asked_h = h; return 0; }

int main(int argc, char** argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <ui module> <bundle dir>\n", argv[0]); return 2; }
    Display* d = XOpenDisplay(NULL);
    if (!d) { printf("not run: no X display\n"); return 77; }
    // Xlib's DEFAULT handler stays installed, as in a host without one of its own: the one that exits
    const XErrorHandler xlib_default = XSetErrorHandler(NULL);
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { printf("FAIL: %s\n", dlerror()); return 1; }
    const LV2UI_Descriptor* (*get)(uint32_t);
    *(void**)&get = dlsym(lib, "lv2ui_descriptor");   // POSIX's way to take a function from dlsym
    const LV2UI_Descriptor* desc = get ? get(0) : NULL;
    const LV2UI_Idle_Interface* idle = desc ? (const LV2UI_Idle_Interface*)desc->extension_data(LV2_UI__idleInterface)
                                            : NULL;
    if (!desc || !idle) { printf("FAIL: no UI descriptor or idle interface\n"); return 1; }

    // 1. a parent the server does not know: the editor may refuse to open, the host must live
    {
        const Window bogus = (Window)0x7ffffff0;
        LV2_Feature fp = { LV2_UI__parent, (void*)(uintptr_t)bogus };
        const LV2_Feature* feats[] = { &fp, NULL };
        LV2UI_Widget w = NULL;
        LV2UI_Handle h = desc->instantiate(desc, "probe", argv[2], no_write, NULL, &w, feats);
        if (h) { idle->idle(h); desc->cleanup(h); }
        printf("ok   bogus parent: the host is still here (editor %s)\n", h ? "opened" : "refused");
    }
    // 2. the parent destroyed while the editor still draws
    {
        const Window parent = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 1200, 164, 0, 0, 0);   // unmapped
        XSync(d, False);
        LV2_Feature fp = { LV2_UI__parent, (void*)(uintptr_t)parent };
        const LV2_Feature* feats[] = { &fp, NULL };
        LV2UI_Widget w = NULL;
        LV2UI_Handle h = desc->instantiate(desc, "probe", argv[2], no_write, NULL, &w, feats);
        if (!h) { printf("FAIL: instantiate with a real parent\n"); return 1; }
        for (int i = 0; i < 3; ++i) idle->idle(h);
        XDestroyWindow(d, parent);
        XSync(d, False);
        const float one = 1.0f;
        for (int i = 0; i < 5; ++i) { desc->port_event(h, 8, sizeof(float), 0, &one); idle->idle(h); }
        desc->cleanup(h);
        printf("ok   parent destroyed before cleanup: the host is still here\n");
    }
    // 3. the host's GUI scale: ui:scaleFactor 2 opens the editor at twice its size (2400 x 328)
    {
        const Window parent = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 2400, 328, 0, 0, 0);   // unmapped
        XSync(d, False);
        LV2_URID_Map map = { NULL, map_uri };
        const float two = 2.0f;
        const LV2_Options_Option opts[] = {
            { LV2_OPTIONS_INSTANCE, 0, map_uri(NULL, LV2_UI__scaleFactor), sizeof(float),
              map_uri(NULL, LV2_ATOM__Float), &two },
            { LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, NULL },
        };
        LV2UI_Resize rz = { NULL, resize_cb };
        LV2_Feature fp = { LV2_UI__parent, (void*)(uintptr_t)parent };
        LV2_Feature fm = { LV2_URID__map, &map };
        LV2_Feature fo = { LV2_OPTIONS__options, (LV2_Options_Option*)opts };
        LV2_Feature fr = { LV2_UI__resize, &rz };
        const LV2_Feature* feats[] = { &fp, &fm, &fo, &fr, NULL };
        LV2UI_Widget w = NULL;
        LV2UI_Handle h = desc->instantiate(desc, "probe", argv[2], no_write, NULL, &w, feats);
        if (!h) { printf("FAIL: instantiate with options\n"); return 1; }
        desc->cleanup(h);
        XDestroyWindow(d, parent);
        if (g_asked_w != 2400 || g_asked_h != 328) {
            printf("FAIL: ui:scaleFactor 2 asked the host for %d x %d, not 2400 x 328\n", g_asked_w, g_asked_h);
            return 1;
        }
        printf("ok   ui:scaleFactor 2: the editor asks for 2400 x 328\n");
    }
    // 4. the GUI restored the handler it found
    if (XSetErrorHandler(NULL) != xlib_default) { printf("FAIL: the GUI left its X error handler installed\n"); return 1; }
    dlclose(lib);
    XCloseDisplay(d);
    printf("PASS\n");
    return 0;
}
