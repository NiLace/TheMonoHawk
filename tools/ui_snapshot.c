// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
// ui_snapshot.c -- LAB instrument: the plugin's GUI drawn without a display, to look at it (or show it) without a
// host, state by state. It compiles the real
// lv2/hawk_ui.c into itself, loads the photograph, cut-outs and type from a directory as the bundle does, and draws
// into a cairo image surface, fed through port_event() the way a host feeds it.
// run: build/tools/hawk_ui_snapshot <out_dir> <assets_dir> [width height]   (default 2400 x 328, the photograph's size)
//      assets_dir: a directory holding gui/assets/*.png and Oswald.ttf (the installed bundle will do)
#include "../lv2/hawk_ui.c"

#include <stdlib.h>

static void pe(HawkUI* ui, int port, float v) { port_event(ui, port, sizeof(float), 0, &v); }

static void playing(HawkUI* ui, int note, float cents, bool intune)
{
    pe(ui, PORT_TUNING, 0); pe(ui, PORT_NOTE, (float)note); pe(ui, PORT_CENTS, cents); pe(ui, PORT_VOICED, 1);
    pe(ui, PORT_IN_TUNE, intune ? 1.0f : 0.0f); pe(ui, PORT_LEVEL, 0.35f);
}

static void shot(int w, int h, const char* assets, const char* path, void (*feed)(HawkUI*))
{
    HawkUI ui;
    hawk_ui_init(&ui);
    ui.width = w; ui.height = h;
    ui.surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    load_assets(&ui, assets);
    feed(&ui);
    update(&ui);
    draw(&ui);
    cairo_surface_write_to_png(ui.surface, path);
    free_assets(&ui);
    cairo_surface_destroy(ui.surface);
    ui.surface = NULL;
}

static void st_off(HawkUI* ui)   { pe(ui, PORT_TUNING, 0); }
static void st_flat(HawkUI* ui)  { playing(ui, 40, -30.0f, false); }
static void st_sharp(HawkUI* ui) { playing(ui, 40, 18.0f, false); }
static void st_tune(HawkUI* ui)  { playing(ui, 45, 0.3f, true); }
static void st_sharp_note(HawkUI* ui) { playing(ui, 49, 6.0f, false); }          // C#3: the sharp sign
static void st_complete(HawkUI* ui) { playing(ui, 45, 0.0f, true); ui->tgt_for = 0;
                                      ui->tgt_count = build_targets(0, ui->tgt_midi);
                                      for (int i = 0; i < 6; ++i) ui->tgt_lit[i] = true;
                                      ui->complete = true; ui->complete_at = hawk_now(); }
static void st_bypass(HawkUI* ui) { pe(ui, PORT_TUNING, 0); pe(ui, PORT_ENABLED, 0); }
static void st_mute(HawkUI* ui)  { playing(ui, 40, -30.0f, false); pe(ui, PORT_ACTIVE, 1); }
static void st_strobe_sharp(HawkUI* ui) { playing(ui, 40, 18.0f, false); pe(ui, PORT_STROBE_MODE, 1);
                                          pe(ui, PORT_STROBE, 1.0f); }
static void st_strobe_flat(HawkUI* ui)  { playing(ui, 40, -12.0f, false); pe(ui, PORT_STROBE_MODE, 1);
                                          pe(ui, PORT_STROBE, -2.0f); }
static void st_strobe_tune(HawkUI* ui)  { playing(ui, 45, 0.3f, true); pe(ui, PORT_STROBE_MODE, 1);
                                          pe(ui, PORT_STROBE, 0.0f); }
static void st_strobe_nonote(HawkUI* ui) { pe(ui, PORT_TUNING, 0); pe(ui, PORT_STROBE_MODE, 1); }
static void st_progress(HawkUI* ui) { playing(ui, 40, 18.0f, false); pe(ui, PORT_LEVEL, 0.5f);
                                      ui->tgt_for = 0; ui->tgt_count = build_targets(0, ui->tgt_midi);
                                      ui->tgt_lit[1] = ui->tgt_lit[2] = true; }
static void st_level_clip(HawkUI* ui) { playing(ui, 40, -30.0f, false); pe(ui, PORT_LEVEL, 0.95f); }
static void st_ref(HawkUI* ui) { playing(ui, 45, 0.3f, true); pe(ui, PORT_REF_ACTIVE, 1); ui->ref_string = 0; }
static void st_settings(HawkUI* ui) { playing(ui, 45, 0.3f, true); ui->settings_open = true; }
static void st_menu_tuning(HawkUI* ui) { pe(ui, PORT_TUNING, 0); ui->dd_open = true; ui->dd_which = 1;
                                         ui->dd_scroll = 0; }
static void st_menu_a4(HawkUI* ui) { pe(ui, PORT_TUNING, 0); ui->dd_open = true; ui->dd_which = 0;
                                     ui->dd_scroll = 440 - A4_MIN - DD_VISIBLE / 2; }
static void st_detect(HawkUI* ui) { pe(ui, PORT_TUNING, 0); ui->detect_mode = true; ui->detect_count = 6;
                                    const int n[6] = { 40, 45, 50, 55, 59, 64 };
                                    for (int i = 0; i < 6; ++i) ui->detect_notes[i] = n[i];
                                    match_detect(ui); }
static void st_detect_empty(HawkUI* ui) { pe(ui, PORT_TUNING, 0); ui->detect_mode = true; }
static void st_fripp(HawkUI* ui) { pe(ui, PORT_TUNING, 37); }                  // the longest tuning name
static void st_eight(HawkUI* ui) { pe(ui, PORT_TUNING, TUNING_COUNT - 1); }   // eight strings

int main(int argc, char** argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <out_dir> <assets_dir> [width height]\n", argv[0]); return 2; }
    const char* out = argv[1];
    const char* assets = argv[2];
    const int w = argc > 4 ? atoi(argv[3]) : 2400, h = argc > 4 ? atoi(argv[4]) : 328;
    typedef struct S { const char* name; void (*feed)(HawkUI*); } S;
    const S states[] = {
        { "off", st_off }, { "flat", st_flat }, { "sharp", st_sharp }, { "tune", st_tune },
        { "sharp_note", st_sharp_note }, { "complete", st_complete }, { "bypass", st_bypass }, { "mute", st_mute },
        { "strobe_sharp", st_strobe_sharp }, { "strobe_flat", st_strobe_flat }, { "strobe_tune", st_strobe_tune },
        { "strobe_nonote", st_strobe_nonote }, { "progress", st_progress }, { "level_clip", st_level_clip },
        { "ref", st_ref }, { "settings", st_settings }, { "menu_tuning", st_menu_tuning }, { "menu_a4", st_menu_a4 },
        { "detect", st_detect }, { "detect_empty", st_detect_empty }, { "fripp", st_fripp }, { "eight", st_eight },
    };
    for (size_t i = 0; i < sizeof states / sizeof states[0]; ++i) {
        const size_t n = strlen(out) + 1 + strlen(states[i].name) + 5;   // out_dir/name.png
        char* png = (char*)malloc(n);
        if (!png) { fprintf(stderr, "out of memory\n"); return 1; }
        snprintf(png, n, "%s/%s.png", out, states[i].name);
        shot(w, h, assets, png, states[i].feed);
        free(png);
    }
    printf("wrote %s/*.png\n", out);
    return 0;
}
