// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
// ui_click_probe.cpp -- gate: every control of the GUI, clicked where it is drawn, does what it says.
//
// The real lv2/hawk_ui.cpp is compiled in, with an image surface and the bundle's type. For each control the probe
// clicks the centre of its rectangle (from layout(), the same geometry the drawing uses) through handle_press() and
// checks the port written and its value, or the overlay opened: MUTE, TONE, STROBE, SETTINGS (Ping, a slider, CLOSE),
// the hawk and the name (the bypass), the A4 menu (a row), the tuning menu (a tuning, Detect Tuning), Detect's CANCEL.
// Then the window is resized and a control is clicked WITHOUT a frame drawn in between: the click must still land.
// Negative arm: built with -DHAWK_UI_NO_LAYOUT_ON_PRESS=1 (clicks tested against the last frame's layout) it must FAIL.
// run: hawk_ui_click_probe <assets dir>      exit 0 = PASS
#include "../lv2/hawk_ui.cpp"

#include <vector>

namespace {
struct Write { uint32_t port; float v; };
std::vector<Write> g_writes;
void rec(LV2UI_Controller, uint32_t port, uint32_t, uint32_t, const void* buf)
{
    g_writes.push_back({ port, *(const float*)buf });
}
bool wrote(uint32_t port, float v)
{
    for (const Write& w : g_writes) if (w.port == port && std::fabs(w.v - v) < 1e-3f) return true;
    return false;
}
// the centre of a rectangle, in WINDOW pixels
void click(HawkUI& ui, const Rect& r, unsigned b = Button1)
{
    handle_press(&ui, ui.lay.ox + r.x + r.w / 2, ui.lay.oy + r.y + r.h / 2, b);
}
void frame(HawkUI& ui) { update(&ui); draw(&ui); }   // a frame drawn: the layout as the screen shows it
}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: %s <assets dir>\n", argv[0]); return 2; }
    int bad = 0;
    auto check = [&](const char* what, bool ok) { std::printf("%-58s %s\n", what, ok ? "ok" : "FAIL"); if (!ok) ++bad; };
    HawkUI ui;
    ui.width = 1200; ui.height = 300;                          // letterboxed: the clicks must take the offset
    ui.surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, ui.width, ui.height);
    load_assets(&ui, argv[1]);
    ui.write = rec;
    frame(ui);

    g_writes.clear(); click(ui, ui.lay.button[0]); check("MUTE writes active = 1", wrote(PORT_ACTIVE, 1));
    g_writes.clear(); click(ui, ui.lay.button[0]); check("MUTE again writes active = 0", wrote(PORT_ACTIVE, 0));
    g_writes.clear(); click(ui, ui.lay.button[2]); check("STROBE writes strobe_mode = 1", wrote(PORT_STROBE_MODE, 1));
    g_writes.clear(); click(ui, ui.lay.button[1]); frame(ui);
    check("TONE writes ref_active = 1 and a ref_note", wrote(PORT_REF_ACTIVE, 1) && wrote(PORT_REF_NOTE, 40));
    g_writes.clear(); click(ui, ui.lay.ref_tag); check("REF cycles the octave (ref_note + 12)", wrote(PORT_REF_NOTE, 52));
    g_writes.clear(); click(ui, ui.lay.note); check("the note screen steps the string (A2)", wrote(PORT_REF_NOTE, 57));
    click(ui, ui.lay.button[1]); frame(ui);
    g_writes.clear(); click(ui, ui.lay.name); check("the name writes enabled = 0 (bypass)", wrote(PORT_ENABLED, 0));
    g_writes.clear();
    handle_press(&ui, ui.lay.ox + ui.lay.logo_cx, ui.lay.oy + ui.lay.logo_cy, Button1);
    check("the hawk writes enabled = 1", wrote(PORT_ENABLED, 1));

    click(ui, ui.lay.button[3]); frame(ui);
    check("SETTINGS opens Advanced Settings", ui.settings_open);
    g_writes.clear(); click(ui, ui.lay.ping); check("Ping writes ping_enable = 1", wrote(PORT_PING_ENABLE, 1));
    g_writes.clear(); click(ui, ui.lay.slider[0]); check("Dim Amount at mid-track writes -30 dB", wrote(PORT_DIM_DB, -30));
    ui.drag = -1;
    click(ui, ui.lay.set_close); check("CLOSE closes it", !ui.settings_open);

    frame(ui);
    click(ui, ui.lay.a4_menu); frame(ui);
    check("the A4 menu opens its list", ui.dd_open && ui.dd_which == 0);
    g_writes.clear();
    handle_press(&ui, ui.lay.ox + ui.lay.dd_panel.x + 10, ui.lay.oy + ui.lay.dd_panel.y + ui.lay.dd_item_h * 0.5, Button1);
    check("its first visible row writes A4 = 437 Hz", wrote(PORT_A4, 437) && !ui.dd_open);
    frame(ui);
    click(ui, ui.lay.row); frame(ui);
    check("the letters' row opens the tuning list", ui.dd_open && ui.dd_which == 1);
    ui.dd_scroll = 0; frame(ui);
    g_writes.clear();
    handle_press(&ui, ui.lay.ox + ui.lay.dd_panel.x + 10, ui.lay.oy + ui.lay.dd_panel.y + ui.lay.dd_item_h * 4.5, Button1);
    check("its 5th row (Drop D) writes tuning = 1", wrote(PORT_TUNING, 1));
    frame(ui);
    click(ui, ui.lay.row); ui.dd_scroll = 0; frame(ui);
    handle_press(&ui, ui.lay.ox + ui.lay.dd_panel.x + 10, ui.lay.oy + ui.lay.dd_panel.y + ui.lay.dd_item_h * 0.5, Button1);
    check("its first row opens Detect Tuning", ui.detect_mode);
    frame(ui);
    click(ui, ui.lay.det_cancel); check("Detect's CANCEL closes it", !ui.detect_mode);

    // a resize, and a click before any frame is drawn at the new size
    frame(ui);
    cairo_surface_destroy(ui.surface);
    ui.width = 2400; ui.height = 328;
    ui.surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, ui.width, ui.height);
    // where MUTE is at the new size (a layout of our own: the plugin has not drawn yet)
    HawkUI probe_ui;
    probe_ui.width = ui.width; probe_ui.height = ui.height;
    probe_ui.surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, ui.width, ui.height);
    load_assets(&probe_ui, argv[1]);
    refresh_layout(&probe_ui);
    {
        cairo_t* c = cairo_create(probe_ui.surface);
        layout(&probe_ui, c);
        cairo_destroy(c);
    }
    const Rect m = probe_ui.lay.button[0];
    g_writes.clear();
    handle_press(&ui, probe_ui.lay.ox + m.x + m.w / 2, probe_ui.lay.oy + m.y + m.h / 2, Button1);
    check("after a resize, MUTE clicked before a frame still lands", wrote(PORT_ACTIVE, 1));

    free_assets(&probe_ui); cairo_surface_destroy(probe_ui.surface); probe_ui.surface = nullptr;
    free_assets(&ui); cairo_surface_destroy(ui.surface); ui.surface = nullptr;
    std::printf("%s\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
