// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
// ui_snapshot.cpp -- LAB instrument: the plugin's GUI drawn without a display, to look at it (or show it) without a
// host, state by state. It compiles the real
// lv2/hawk_ui.cpp into itself, loads the photograph, cut-outs and type from a directory as the bundle does, and draws
// into a cairo image surface, fed through port_event() the way a host feeds it.
// run: build/tools/hawk_ui_snapshot <out_dir> <assets_dir> [width height]   (default 2400 x 328, the photograph's size)
//      assets_dir: a directory holding gui/assets/*.png and Oswald.ttf (the installed bundle will do)
#include "../lv2/hawk_ui.cpp"

#include <cstdlib>
#include <string>

namespace {
void pe(HawkUI* ui, int port, float v) { port_event(ui, port, sizeof(float), 0, &v); }

void playing(HawkUI* ui, int note, float cents, bool intune)
{
    pe(ui, PORT_TUNING, 0); pe(ui, PORT_NOTE, (float)note); pe(ui, PORT_CENTS, cents); pe(ui, PORT_VOICED, 1);
    pe(ui, PORT_IN_TUNE, intune ? 1.0f : 0.0f); pe(ui, PORT_LEVEL, 0.35f);
}

void shot(int w, int h, const std::string& assets, const std::string& path, void (*feed)(HawkUI*))
{
    HawkUI ui;
    ui.width = w; ui.height = h;
    ui.surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    load_assets(&ui, assets);
    feed(&ui);
    update(&ui);
    draw(&ui);
    cairo_surface_write_to_png(ui.surface, path.c_str());
    free_assets(&ui);
    cairo_surface_destroy(ui.surface);
    ui.surface = nullptr;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) { std::fprintf(stderr, "usage: %s <out_dir> <assets_dir> [width height]\n", argv[0]); return 2; }
    const std::string out = argv[1], assets = argv[2];
    const int w = argc > 4 ? std::atoi(argv[3]) : 2400, h = argc > 4 ? std::atoi(argv[4]) : 328;
    struct S { const char* name; void (*feed)(HawkUI*); };
    const S states[] = {
        { "off",   [](HawkUI* ui) { pe(ui, PORT_TUNING, 0); } },
        { "flat",  [](HawkUI* ui) { playing(ui, 40, -30.0f, false); } },
        { "sharp", [](HawkUI* ui) { playing(ui, 40, 18.0f, false); } },
        { "tune",  [](HawkUI* ui) { playing(ui, 45, 0.3f, true); } },
        { "sharp_note", [](HawkUI* ui) { playing(ui, 49, 6.0f, false); } },          // C#3: the sharp sign
        { "complete", [](HawkUI* ui) { playing(ui, 45, 0.0f, true); ui->tgt_for = 0;
                                       ui->tgt_count = build_targets(0, ui->tgt_midi);
                                       for (int i = 0; i < 6; ++i) ui->tgt_lit[i] = true;
                                       ui->complete = true; ui->complete_at = std::chrono::steady_clock::now(); } },
        { "bypass", [](HawkUI* ui) { pe(ui, PORT_TUNING, 0); pe(ui, PORT_ENABLED, 0); } },
        { "mute",  [](HawkUI* ui) { playing(ui, 40, -30.0f, false); pe(ui, PORT_ACTIVE, 1); } },
        { "strobe_sharp", [](HawkUI* ui) { playing(ui, 40, 18.0f, false); pe(ui, PORT_STROBE_MODE, 1);
                                           pe(ui, PORT_STROBE, 1.0f); } },
        { "strobe_flat",  [](HawkUI* ui) { playing(ui, 40, -12.0f, false); pe(ui, PORT_STROBE_MODE, 1);
                                           pe(ui, PORT_STROBE, -2.0f); } },
        { "strobe_tune",  [](HawkUI* ui) { playing(ui, 45, 0.3f, true); pe(ui, PORT_STROBE_MODE, 1);
                                           pe(ui, PORT_STROBE, 0.0f); } },
        { "strobe_nonote", [](HawkUI* ui) { pe(ui, PORT_TUNING, 0); pe(ui, PORT_STROBE_MODE, 1); } },
        { "progress", [](HawkUI* ui) { playing(ui, 40, 18.0f, false); pe(ui, PORT_LEVEL, 0.5f);
                                       ui->tgt_for = 0; ui->tgt_count = build_targets(0, ui->tgt_midi);
                                       ui->tgt_lit[1] = ui->tgt_lit[2] = true; } },
        { "level_clip", [](HawkUI* ui) { playing(ui, 40, -30.0f, false); pe(ui, PORT_LEVEL, 0.95f); } },
        { "ref", [](HawkUI* ui) { playing(ui, 45, 0.3f, true); pe(ui, PORT_REF_ACTIVE, 1); ui->ref_string = 0; } },
        { "settings", [](HawkUI* ui) { playing(ui, 45, 0.3f, true); ui->settings_open = true; } },
        { "menu_tuning", [](HawkUI* ui) { pe(ui, PORT_TUNING, 0); ui->dd_open = true; ui->dd_which = 1;
                                          ui->dd_scroll = 0; } },
        { "menu_a4", [](HawkUI* ui) { pe(ui, PORT_TUNING, 0); ui->dd_open = true; ui->dd_which = 0;
                                      ui->dd_scroll = 440 - A4_MIN - DD_VISIBLE / 2; } },
        { "detect", [](HawkUI* ui) { pe(ui, PORT_TUNING, 0); ui->detect_mode = true; ui->detect_count = 6;
                                     const int n[6] = { 40, 45, 50, 55, 59, 64 };
                                     for (int i = 0; i < 6; ++i) ui->detect_notes[i] = n[i];
                                     match_detect(ui); } },
        { "detect_empty", [](HawkUI* ui) { pe(ui, PORT_TUNING, 0); ui->detect_mode = true; } },
        { "fripp", [](HawkUI* ui) { pe(ui, PORT_TUNING, 37); } },                  // the longest tuning name
        { "eight", [](HawkUI* ui) { pe(ui, PORT_TUNING, TUNING_COUNT - 1); } },   // eight strings
    };
    for (const S& s : states) shot(w, h, assets, out + "/" + s.name + ".png", s.feed);
    std::printf("wrote %s/*.png\n", out.c_str());
    return 0;
}
