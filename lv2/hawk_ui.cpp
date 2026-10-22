// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
// The Hawk — LV2 X11 + Cairo GUI.
//
// A self-contained, framework-free tuner display. The host (e.g. Ardour) embeds our X11 window in its plugin slot and
// drives us through the LV2 idle interface; we never run our own event loop. We read the plugin's output ports (note,
// cents, the in-tune lamp, the input level, the strobe phase) and write its controls (mute, the reference tone, the
// strobe view, the A4 reference, the tuning, the Advanced Settings, and the bypass).
//
// Visual: a 1.5U rack unit: a photograph of the unit with its LED screens
// drawn live over it -- see "Drawing" below.

#include <lv2/atom/atom.h>
#include <lv2/core/lv2.h>
#include <lv2/options/options.h>
#include <lv2/ui/ui.h>
#include <lv2/urid/urid.h>

#include <X11/Xlib.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>

#include "hawk_ui_fonts.h"
#include "hawk_ui_layout.h"

#include <chrono>
#include <cmath>
#include <mutex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "tunings.hpp"   // shared tuning table (Tuning, TUNINGS, build_targets) —
                         // single source of truth with the DSP (src/tunings.hpp)

#define HAWK_UI_URI "https://nylarea.com/lv2/themonohawk#ui"

namespace {

// Output control-port indices — must match hawk.ttl.
enum {
    PORT_ACTIVE  = 2,   // Dim/Mute Active (input toggle) — written by the UI
    PORT_DIM_DB  = 3,   // Dim Amount (input) — edited in Advanced Settings
    PORT_NOTE    = 4,
    PORT_CENTS   = 5,
    PORT_VOICED  = 8,
    PORT_TOLERANCE = 9, // In-Tune Tolerance (input) — edited in Advanced Settings
    PORT_RAISE   = 10,
    PORT_IN_TUNE = 11,
    PORT_LOWER   = 12,
    PORT_A4      = 13,
    PORT_TUNING  = 14,  // selected tuning (enum input) — faceplate selector + host
    PORT_REF_ACTIVE = 15,  // reference-tone on/off (input toggle) — TONE button
    PORT_REF_NOTE   = 16,  // reference-tone target, MIDI note (input)
    PORT_PING_ENABLE = 17, // in-tune ping on/off (input toggle) — Advanced Settings
    PORT_LEVEL       = 18, // input level 0..1 (output) — drives the signal meter
    PORT_SENSITIVITY = 19, // detection sensitivity 0..1 (input) — Advanced Settings
    PORT_STROBE      = 20, // the string's phase against the note, radians (output)
    PORT_STROBE_MODE = 21, // strobe view on/off (input toggle) — STROBE button
    PORT_ENABLED     = 22, // lv2:enabled — the host's bypass; the hawk and the name toggle it
};

const char* const NOTE_NAMES[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

// The tuning table (struct Tuning, TUNINGS[], TUNING_ROOT[], pitch_class,
// build_targets) now lives in the shared header src/tunings.hpp, so the GUI row
// and the DSP detector can never drift apart. Bass-family helper below.

// A tuning is in the Bass family iff its display name starts with "Bass" (all
// bass presets are named that way); everything else is Guitar. The string count
// is just the number of notes (TUNINGS[].count).
inline bool tuning_is_bass(const Tuning& t) {
    return std::strncmp(t.name, "Bass", 4) == 0;
}

// Hierarchical display model for the tuning dropdown:
//   category (Guitar, then Bass) -> instrument (string count ascending)
//   -> tunings (table order, which puts Standard first in each group).
struct DispRow {
    int         kind;    // 0 = category header, 1 = instrument header, 2 = tuning, 3 = action
    const char* label;
    int         tuning;  // index into TUNINGS for kind 2, else -1
    int         indent;  // 0/1/2
};
const char* const STRINGS_LABEL[9] = {
    "", "1-String", "2-String", "3-String", "4-String",
    "5-String", "6-String", "7-String", "8-String"
};
// Rows: the action, two family headings, at most eight string-count headings per family, and every tuning.
DispRow g_disp[1 + 2 + 2 * 8 + TUNING_COUNT];
int     g_disp_n = -1;     // lazily built; UI thread only

void ensure_disp() {
    if (g_disp_n >= 0) return;
    int n = 0;
    g_disp[n++] = { 3, "Detect Tuning...", -1, 0 };  // action: auto-detect
    for (int fam = 0; fam < 2; ++fam) {              // 0 = Guitar, 1 = Bass
        g_disp[n++] = { 0, fam ? "BASS" : "GUITAR", -1, 0 };   // bounded by the array's size above
        for (int sc = 1; sc <= 8; ++sc) {            // string count, ascending
            bool any = false;
            for (int i = 0; i < TUNING_COUNT; ++i)
                if ((int)tuning_is_bass(TUNINGS[i]) == fam && TUNINGS[i].count == sc) {
                    any = true; break;
                }
            if (!any) continue;
            g_disp[n++] = { 1, STRINGS_LABEL[sc], -1, 1 };
            for (int i = 0; i < TUNING_COUNT; ++i)
                if ((int)tuning_is_bass(TUNINGS[i]) == fam && TUNINGS[i].count == sc)
                    g_disp[n++] = { 2, TUNINGS[i].name, i, 2 };
        }
    }
    g_disp_n = n;
}

// Display-row index of the row showing a given tuning (for opening centred).
int disp_row_for_tuning(int t) {
    ensure_disp();
    for (int i = 0; i < g_disp_n; ++i)
        if (g_disp[i].kind == 2 && g_disp[i].tuning == t) return i;
    return 0;
}

// Palette.
// Colour-blind-safe palette: a blue<->amber divergent pair (the gold-standard
// CVD contrast — survives red-green colour blindness) with a light/white centre
// distinguished by brightness, not hue. Direction is also encoded by position
// (flat left, sharp right, in-tune centre), so colour is never the only cue.
const double COL_INTUNE[3] = { 1.00, 1.00, 1.00 };  // in tune  (white)
const double COL_FLAT  [3] = { 0.20, 0.55, 0.95 };  // flat  -> raise (blue)
const double COL_SHARP [3] = { 1.00, 0.72, 0.12 };  // sharp -> lower (amber)
const double COL_DIM  [3] = { 0.26, 0.26, 0.30 };  // inactive
// The strobe's in-tune block: a third colour, because the strip's resting
// segments are already white. A first candidate for him to try.
const double COL_STROBE_TUNE[3] = { 0.30, 0.92, 0.45 };

// A4 tuning-reference dropdown: 380..500 Hz in 1 Hz steps.
constexpr int A4_MIN     = 380;
constexpr int A4_MAX     = 500;
constexpr int A4_COUNT   = A4_MAX - A4_MIN + 1;   // 121 values
constexpr int DD_VISIBLE = 7;                     // rows shown at once (both menus)

// 14-segment bit pattern for a character. The display only ever shows note
// letters (A-G, including B and D) and octave digits (0-9), so only those are
// defined here. Segment bits: A=0,B=1,C=2,D=3,E=4,F=5,G1=6,G2=7,H=8,I=9,J=10,
// K=11,L=12,M=13.
uint16_t seg14_mask(char ch)
{
    switch (ch) {
        case '0': return 0x003F; case '1': return 0x0006; case '2': return 0x00DB;
        case '3': return 0x00CF; case '4': return 0x00E6; case '5': return 0x00ED;
        case '6': return 0x00FD; case '7': return 0x0007; case '8': return 0x00FF;
        case '9': return 0x00EF;
        case 'A': return 0x00F7; case 'B': return 0x128F; case 'C': return 0x0039;
        case 'D': return 0x120F; case 'E': return 0x00F9; case 'F': return 0x00F1;
        case 'G': return 0x00BD;
        default:  return 0x0000;
    }
}

// A rectangle in content-box pixels (hit-testing).
struct Rect {
    double x = 0, y = 0, w = 0, h = 0;
    bool contains(double px, double py) const { return w > 0 && px >= x && px <= x + w && py >= y && py <= y + h; }
};

// Where everything is: computed by layout() from the window's size and the state, read by draw() and by the click
// handler -- so a click is always tested against what the current state and size put on screen.
struct Layout {
    double ox = 0, oy = 0;          // the content box's origin in the window (letterbox)
    double W = 0, H = 0, k = 1;     // its size, and pixels per panel mm
    int    ti = 0, nstr = 0;        // the tuning and its string count
    double tsz = 0, gap_half = 0;   // the tuning menu's type size, and the gap it opens in the letters' row
    double letter_x[8] = {};        // each string letter's x, panel mm
    double nsz = 0;                 // the name's type size (shrunk to the buttons' span if needed)
    Rect   note, ref_tag, a4_menu, row, name, button[4];   // button: MUTE TONE STROBE SETTINGS
    double logo_cx = 0, logo_cy = 0, logo_r = 0;           // the hawk (the bypass), a circle
    Rect   dd_panel;                // an open dropdown's rows
    double dd_item_h = 0;
    Rect   det_apply, det_clear, det_cancel;
    Rect   set_panel, slider[3], ping, set_close;           // slider: Dim Amount, In-Tune Tolerance, Sensitivity
};

struct HawkUI {
    Layout lay;
    // The photograph and its cut-outs (gui/assets, from the Blender scene), the printed type, and the photograph
    // scaled to the window (rebuilt on a size change only).
    cairo_surface_t*   panel        = nullptr;
    cairo_surface_t*   static_layer = nullptr;  // the photograph + the unlit segments, at window size
    int                static_w = 0, static_h = 0, static_view = -1;
    cairo_surface_t*   sprite[7]    = {};       // MUTE TONE STROBE SETTINGS pressed; the hawk on/lit/bypass
    cairo_font_face_t* font         = nullptr;  // Oswald, from the bundle
    float  enabled = 1.0f;                      // mirrors PORT_ENABLED (1 = running, 0 = bypassed)
    // Repaint only when something changed or moves, at most 30 times a second.
    bool   dirty = true;
    std::chrono::steady_clock::time_point last_draw;

    Display*         dpy     = nullptr;
    Window           win     = 0;
    Colormap         cmap    = 0;
    Visual*          visual  = nullptr;
    int              screen  = 0;
    cairo_surface_t* surface   = nullptr;
    int              width     = 1200;      // the panel's proportions (482.6 x 65.9 mm)
    int              height  = 164;

    LV2UI_Resize*        host_resize = nullptr;
    LV2UI_Write_Function write       = nullptr;
    LV2UI_Controller     controller  = nullptr;

    float  mute = 0.0f;              // the MUTE button (PORT_ACTIVE)

    // Reference-tone (pitch-pipe) button: state + hit-rectangle. When active, the
    // plugin drones a sine at the selected string's pitch; the note box shows
    // which string, and clicking the note box cycles through the tuning's strings.
    float  ref_active = 0.0f;       // UI source of truth (mirrors PORT_REF_ACTIVE)
    int    ref_string = 0;          // selected string index within the current tuning
    int    ref_octave = 0;          // reference-tone octave shift (0/+1/+2 for audibility)
    float  ref_note_in = -1.0f;     // the ref_note port as the host last sent it (<0: not yet)
    int    ref_cand   = -1;         // candidate string awaiting the follow debounce
    std::chrono::steady_clock::time_point ref_cand_since;

    // Input-level meter + sensitivity (Advanced Settings).
    float  level       = 0.0f;      // mirrors PORT_LEVEL (input signal level)
    float  sensitivity = 0.5f;      // mirrors PORT_SENSITIVITY

    // Dropdown menu (shared by the A4 and tuning selectors — only one open at a
    // time). dd_which picks the list: 0 = A4, 1 = tuning.
    bool   dd_open   = false;
    int    dd_which  = 0;
    int    dd_scroll = 0;

    // Latest values pushed from the plugin's output ports.
    float note    = -1.0f;  // MIDI note number; <0 means "none yet"
    float cents   = 0.0f;
    float a4_ref  = 440.0f; // tuning reference (A4), mirrors the a4 control port
    float voiced  = 0.0f;
    float in_tune = 0.0f;   // the only tuning lamp the faceplate uses (flat/sharp
                            // are derived from the sign of `cents`, so the plugin's
                            // raise/lower lamp ports are not read here)
    float tuning  = 0.0f;   // selected tuning index (into TUNINGS)

    // Strobe view (mirrors PORT_STROBE_MODE; PORT_STROBE is the string's phase
    // against the detected note). strobe_pos is how far the stripes have slid,
    // in stripe periods [0, 1): it follows the phase itself while the string is
    // slow enough to follow, and slides at a capped speed beyond.
    float  strobe_mode  = 0.0f;
    float  strobe_phase = 0.0f;
    double strobe_pos   = 0.0;
    float  strobe_last  = 0.0f;   // phase at the latest fresh reading
    bool   strobe_sounding = false, strobe_s_intune = false;   // update()'s verdict for the disc
    bool   strobe_init  = false;  // strobe_pos/strobe_last/strobe_t hold a frame
    double strobe_extra = 0.0;    // slid by extrapolation since the latest fresh reading
    // What the disc keeps showing through a re-pluck, when the plugin publishes
    // no reading (the attack) or none at all (the pick stops the string).
    float  strobe_note   = -1.0f;
    float  strobe_cents  = 0.0f;
    bool   strobe_intune = false;
    std::chrono::steady_clock::time_point strobe_t;        // previous frame
    std::chrono::steady_clock::time_point strobe_fresh_t;  // latest fresh phase
    std::chrono::steady_clock::time_point strobe_seen_t;   // latest voiced frame

    // Per-string tuning progress: each target latches "lit" when hit in tune,
    // and stays lit. When all are lit the tuning is complete (row + logo glow
    // amber for COMPLETE_HOLD, then everything clears so the next instrument can tune).
    int   tgt_midi[8] = {0};
    bool  tgt_lit[8]  = {false};
    int   tgt_count   = 0;
    int   tgt_for     = -1;     // which tuning index tgt_* were built for
    bool  prev_lock   = false;  // previous in-tune state (for rising-edge latch)
    bool  complete    = false;
    std::chrono::steady_clock::time_point complete_at;

    // Advanced Settings pop-up (opened by the SETTINGS button): live-edit the
    // settings that have no faceplate control. dim_db/tolerance mirror ports 3/9.
    bool   settings_open = false;
    float  dim_db    = -12.0f;
    float  tolerance = 2.0f;
    float  ping_enable = 0.0f;       // in-tune ping on/off (mirrors PORT_PING_ENABLE; off by default)
    int    drag = -1;   // slider being dragged: -1 none, 0 Dim Amount, 1 Tolerance

    // Auto-detect tuning ("Detect Tuning" from the menu): play the open strings
    // one at a time; it collects each settled pitch and matches the set against
    // the tuning table. One string at a time — single-note detection only.
    bool  detect_mode      = false;
    int   detect_notes[8]  = {0};
    int   detect_count     = 0;
    bool  detect_prev_voiced = false;
    bool  detect_pluck_done  = false;   // the pitch being held is already captured
    int   detect_cap_note  = -1;        // the pitch being held (its dwell runs from detect_cap_start)
    std::chrono::steady_clock::time_point detect_cap_start;
    int   detect_best      = -1;        // best-matching tuning index
    bool  detect_exact     = false;     // the full set matched a tuning exactly
};

// Match the collected open-string pitches against the tuning table. Picks the tuning with the smallest total
// mismatch (semitones from each played note to that tuning's nearest string); ties break in table order, which puts
// the common tunings first (Standard, then the 6-string guitar ones, bass and extended range after). Breaking them
// toward the FEWEST strings named a 4-string bass for a guitar's low D: with a few notes played, a
// smaller instrument always "holds every note".
// HAWK_UI_DETECT_OLD=1 restores both old rules (that tie-break, and a capture only after the signal went unvoiced), for
// the detect gate's negative arm only (tools/ui_detect_probe.cpp).
#ifndef HAWK_UI_DETECT_OLD
#define HAWK_UI_DETECT_OLD 0
#endif
void match_detect(HawkUI* ui)
{
    ui->detect_best  = -1;
    ui->detect_exact = false;
    if (ui->detect_count == 0) return;
    int  tmp[8];
    long best_rank = (1L << 60), best_score = 0;
    for (int t = 0; t < TUNING_COUNT; ++t) {
        if (TUNINGS[t].count < ui->detect_count) continue;   // must hold every note
        build_targets(t, tmp);
        long score = 0;
        for (int i = 0; i < ui->detect_count; ++i) {
            int bestd = 1 << 30;
            for (int j = 0; j < TUNINGS[t].count; ++j) {
                const int d = std::abs(ui->detect_notes[i] - tmp[j]);
                if (d < bestd) bestd = d;
            }
            score += bestd;
        }
#if HAWK_UI_DETECT_OLD
        const long rank = score * 1000 + (TUNINGS[t].count - ui->detect_count);
#else
        const long rank = score * 1000 + t;
#endif
        if (rank < best_rank) { best_rank = rank; ui->detect_best = t; best_score = score; }
    }
    ui->detect_exact = (ui->detect_best >= 0 && best_score == 0
                        && TUNINGS[ui->detect_best].count == ui->detect_count);
}

// Collect open-string pitches while in detect mode: each time the detected note changes and then holds steady for
// DETECT_DWELL, commit it (if new) and re-match. A new note is a new pitch, not a new onset: one plays the open strings
// one after another while the previous ones still ring, and the plugin does not report "no note" between them.
// A note ringing on steadily for DETECT_DWELL is taken; a note that ENDS (muted, or replaced by the next string) is
// still taken if it held for DETECT_MIN, so strings muted quickly are not lost. While several
// strings ring at once (a strum) nothing is taken: that is a chord, not an open string.
constexpr auto DETECT_DWELL = std::chrono::milliseconds(250);
#if !HAWK_UI_DETECT_OLD
constexpr auto DETECT_MIN   = std::chrono::milliseconds(100);
#endif

void detect_commit(HawkUI* ui, int m)
{
    bool dup = false;
    for (int i = 0; i < ui->detect_count; ++i)
        if (ui->detect_notes[i] == m) dup = true;
    if (!dup && ui->detect_count < 8) ui->detect_notes[ui->detect_count++] = m;
    ui->detect_pluck_done = true;                       // this note is in; the next pitch starts a new dwell
    match_detect(ui);
}

void update_detect(HawkUI* ui, bool voiced, bool strum)
{
    if (!ui->detect_mode) { ui->detect_prev_voiced = voiced; return; }
    const auto now = std::chrono::steady_clock::now();
    const int m = (int)std::lround(ui->note);
#if !HAWK_UI_DETECT_OLD
    // the note being held ends (silence, a strum, or another pitch): take it if it held long enough
    const bool ends = ui->detect_cap_note >= 0 && (!voiced || ui->note < 0.0f || strum || m != ui->detect_cap_note);
    if (ends && !ui->detect_pluck_done && now - ui->detect_cap_start >= DETECT_MIN)
        detect_commit(ui, ui->detect_cap_note);
#else
    (void)strum;
#endif
    if (!voiced || ui->note < 0.0f || strum) {
        ui->detect_cap_note = -1;
    } else if (m != ui->detect_cap_note
#if HAWK_UI_DETECT_OLD
               && (!ui->detect_prev_voiced || ui->detect_cap_note < 0)
#endif
              ) {                                       // a new pitch: start its dwell
        ui->detect_cap_note   = m;
        ui->detect_cap_start  = now;
        ui->detect_pluck_done = false;
    } else if (m == ui->detect_cap_note && !ui->detect_pluck_done && now - ui->detect_cap_start > DETECT_DWELL) {
        detect_commit(ui, m);
    }
    ui->detect_prev_voiced = voiced;
}

// Advance the per-string tuning progress. Rebuilds the target table when the
// tuning changes, latches a target "lit" on each fresh in-tune lock, marks the
// set complete when all are lit, and clears everything COMPLETE_HOLD after completion.
constexpr auto COMPLETE_HOLD = std::chrono::seconds(5);
void ref_apply_note(HawkUI* ui);   // below, with the reference-tone helpers
void update_tuning_progress(HawkUI* ui, bool voiced, bool intune)
{
    const int t = (int)std::lround(ui->tuning);
    if (t != ui->tgt_for) {                 // tuning changed -> reset progress
#if !HAWK_UI_IGNORE_REF_NOTE
        // the drone follows the new tuning's string (the host may have changed it behind the GUI's back)
        if (ui->tgt_for >= 0 && ui->ref_active > 0.5f) { ui->tgt_for = t; ref_apply_note(ui); }
#endif
        ui->tgt_for   = t;
        ui->tgt_count = build_targets(t, ui->tgt_midi);
        for (int i = 0; i < 8; ++i) ui->tgt_lit[i] = false;
        ui->complete  = false;
        ui->prev_lock = false;
    }

    if (ui->complete) {
        const auto elapsed = std::chrono::steady_clock::now() - ui->complete_at;
        if (elapsed > COMPLETE_HOLD) {               // hold done, then clear
            for (int i = 0; i < 8; ++i) ui->tgt_lit[i] = false;
            ui->complete = false;
        }
        ui->prev_lock = voiced && intune;
        return;                              // no new latches during the hold
    }

    const bool lock = voiced && intune;
    if (lock && !ui->prev_lock) {            // rising edge: one fresh lock
        const int m = (int)std::lround(ui->note);
        for (int i = 0; i < ui->tgt_count; ++i)
            if (!ui->tgt_lit[i] && ui->tgt_midi[i] == m) { ui->tgt_lit[i] = true; break; }
    }
    ui->prev_lock = lock;

    bool all = ui->tgt_count > 0;
    for (int i = 0; i < ui->tgt_count; ++i) if (!ui->tgt_lit[i]) all = false;
    if (all) {
        ui->complete    = true;
        ui->complete_at = std::chrono::steady_clock::now();
    }
}

// Reference-tone helpers. ref_build_targets fills out[] with the MIDI note of
// each string of the currently-selected tuning (low -> high) and returns the
// count; ref_nearest_string picks the string closest to a given MIDI note, so
// engaging the tone starts on whatever string you were last playing.
int ref_build_targets(const HawkUI* ui, int* out) {
    int t = (int)std::lround(ui->tuning);
    if (t < 0)              t = 0;
    if (t >= TUNING_COUNT)  t = TUNING_COUNT - 1;
    return build_targets(t, out);
}
int ref_nearest_string(const int* tgt, int n, int midi) {
    int best = 0, bestd = 1 << 30;
    for (int i = 0; i < n; ++i) {
        const int d = std::abs(tgt[i] - midi);
        if (d < bestd) { bestd = d; best = i; }
    }
    return best;
}

// Write the reference-tone note for the current string, shifted up by ref_octave
// (so low strings can be heard in a comfortable register). Called from every
// place that changes the selected string or the octave.
void ref_apply_note(HawkUI* ui) {
    int tgt[8];
    const int n = ref_build_targets(ui, tgt);
    if (ui->ref_string < 0)  ui->ref_string = 0;
    if (ui->ref_string >= n) ui->ref_string = n - 1;
    int m = tgt[ui->ref_string] + 12 * ui->ref_octave;
    if (m < 0)   m = 0;
    if (m > 127) m = 127;
    if (ui->write) {
        float rn = (float)m;
        ui->write(ui->controller, PORT_REF_NOTE, sizeof(float), 0, &rn);
    }
}

// The host owns ref_note (it restores it with the session, automation can move it): show the string and octave it
// names. A note no string of the tuning reaches is shown as it is (ref_draw_note). HAWK_UI_IGNORE_REF_NOTE=1 restores
// the old GUI that kept its own string and never read the port, for the ports gate's negative arm only
// (tools/ui_ports_probe.cpp).
#ifndef HAWK_UI_IGNORE_REF_NOTE
#define HAWK_UI_IGNORE_REF_NOTE 0
#endif
void ref_from_port(HawkUI* ui, float v)
{
#if !HAWK_UI_IGNORE_REF_NOTE
    ui->ref_note_in = v;
    int tgt[8];
    const int n = ref_build_targets(ui, tgt), m = (int)std::lround(v);
    for (int o = 0; o <= 2; ++o)
        for (int i = 0; i < n; ++i)
            if (tgt[i] + 12 * o == m) { ui->ref_string = i; ui->ref_octave = o; return; }
#else
    (void)ui; (void)v;
#endif
}

// The note the note screen shows while TONE drones: the string's own note (the octave shift is the REF tag's), or the
// port's note when no string of the tuning reaches it.
int ref_draw_note(HawkUI* ui)
{
    int tgt[8];
    const int n = ref_build_targets(ui, tgt);
    if (ui->ref_string < 0)  ui->ref_string = 0;
    if (ui->ref_string >= n) ui->ref_string = n - 1;
    const int m = tgt[ui->ref_string];
    if (ui->ref_note_in >= 0.0f && (int)std::lround(ui->ref_note_in) != m + 12 * ui->ref_octave)
        return (int)std::lround(ui->ref_note_in);
    return m;
}

// Auto-follow: while the reference tone is on, switch the drone to whichever
// string you're playing. Match the detected note to the nearest string target
// (octave-aware, so it tracks even when the string is out of tune) and retune
// the drone to that string's in-tune pitch. Debounced so transient/harmonic
// blips don't make it jump; holds the current string while you're not playing,
// so the drone keeps ringing as you tune. Manual note-box clicks still override.
void update_ref_follow(HawkUI* ui) {
    if (ui->ref_active <= 0.5f) { ui->ref_cand = -1; return; }
    if (!(ui->voiced > 0.5f) || ui->note < 0.0f) { ui->ref_cand = -1; return; }

    int tgt[8];
    const int n     = ref_build_targets(ui, tgt);
    const int match = ref_nearest_string(tgt, n, (int)std::lround(ui->note));
    if (match == ui->ref_string) { ui->ref_cand = -1; return; }  // already there

    const auto now = std::chrono::steady_clock::now();
    if (match != ui->ref_cand) {                 // new candidate -> start its dwell
        ui->ref_cand       = match;
        ui->ref_cand_since = now;
    } else if (now - ui->ref_cand_since > std::chrono::milliseconds(150)) {
        ui->ref_string = match;                  // held long enough -> switch the drone
        ui->ref_cand   = -1;
        ref_apply_note(ui);
    }
}

// ---- Drawing -------------------------------------------------------------
//
// The rack design. The window shows ONE photograph of the unit,
// gui/assets/panel.png, rendered with its screens empty and without the printed text the plugin changes; everything
// that changes is drawn over it here, in panel millimetres (lv2/hawk_ui_layout.h, dumped by the scene itself):
//
//   [note screen]  [ level line .................................. ]  [MUTE ][TONE    ]  (hawk)
//                  [ chevrons, or the strobe disc .................. ]  [STROBE][SETTINGS]
//   cents  A4 v      E     A     D    Standard v    G     B     E          THE HAWK
//
// The pressed buttons and the lit hawk are cut-outs of renders of the same camera (gui/assets/*.png), pasted where
// they belong. The LED content is drawn as light: a soft halo, the element, and a whitish core.

namespace L = hawk_layout;

// The printed inks, measured on the round-12 render (hawk_cycles_black_off.png, the brightest 3 % of each glyph's
// pixels): the white print of the letters and readouts, the amber of what can be clicked, the grey of an unlit
// legend, and the faint print of an unlit LED segment.
const double INK_WHITE[3] = { 0.76, 0.76, 0.76 };
const double INK_AMBER[3] = { 0.86, 0.58, 0.31 };
const double INK_DIM  [3] = { 0.54, 0.54, 0.54 };
const double GHOST    [3] = { 0.15, 0.16, 0.20 };

struct Geo {   // panel mm -> content-box pixels
    double W, H, k;
    double X(double x) const { return W * 0.5 + x * k; }
    double Y(double z) const { return H * 0.5 - (z - L::PANEL_H * 0.5) * k; }
    double S(double mm) const { return mm * k; }
};

// Blender's text size is not cairo's em: measured on the round-12 render (the first string letter, 12 mm, Oswald), its
// capitals stand 31 px tall at 2400 px across where cairo's at the same size stand 48: 0.646.
constexpr double TEXT_EM = 31.0 / 48.0;

// Draw text in the panel's face at a size in mm (the scene's size); the vertical anchor is the capitals' middle
// (Blender's CENTER of an all-caps line). Returns the advance width in px.
double panel_text(cairo_t* cr, HawkUI* ui, const Geo& g, const char* s, double size_mm, double x_mm, double z_mm,
                  int align /* -1 left, 0 centre, 1 right */, const double rgb[3], double alpha, bool measure_only = false)
{
    if (ui->font) cairo_set_font_face(cr, ui->font);
    else cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, g.S(size_mm * TEXT_EM));
    cairo_text_extents_t te, hcap;
    cairo_text_extents(cr, s, &te);
    cairo_text_extents(cr, "H", &hcap);
    if (measure_only) return te.x_advance;
    double x = g.X(x_mm);
    if (align == 0)      x -= te.x_advance * 0.5;
    else if (align == 1) x -= te.x_advance;
    const double y = g.Y(z_mm) - (hcap.height * 0.5 + hcap.y_bearing);
    cairo_set_source_rgba(cr, rgb[0], rgb[1], rgb[2], alpha);
    cairo_move_to(cr, x, y);
    cairo_show_text(cr, s);
    return te.x_advance;
}

// A lit LED stroke: halo, element, core (the scene's lit(): a coloured glow layer and a narrower near-white core, and
// light ADDS). path() lays the path; it is replayed for each layer.
template <class F>
void led_stroke(cairo_t* cr, F path, double width, const double c[3], double level)
{
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    const struct { double w, a, white; } layers[] = {
        { 2.6, 0.10, 0.0 }, { 1.7, 0.22, 0.0 }, { 1.0, 0.95, 0.15 }, { 0.42, 0.85, 0.70 },
    };
    for (const auto& ly : layers) {
        path();
        cairo_set_line_width(cr, width * ly.w);
        cairo_set_source_rgba(cr, c[0] + (1 - c[0]) * ly.white, c[1] + (1 - c[1]) * ly.white,
                              c[2] + (1 - c[2]) * ly.white, ly.a * level);
        cairo_stroke(cr);
    }
}

template <class F>
void ghost_stroke(cairo_t* cr, F path, double width)
{
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    path();
    cairo_set_line_width(cr, width);
    cairo_set_source_rgb(cr, GHOST[0], GHOST[1], GHOST[2]);
    cairo_stroke(cr);
}

void rrect_path(cairo_t* cr, double x, double y, double w, double h, double r)
{
    r = std::fmin(r, std::fmin(w, h) * 0.5);
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r,     r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0,          M_PI / 2);
    cairo_arc(cr, x + r,     y + h - r, r,  M_PI / 2,  M_PI);
    cairo_arc(cr, x + r,     y + r,     r,  M_PI,      3 * M_PI / 2);
    cairo_close_path(cr);
}

// A 14-segment glyph as the scene draws it (slanted 6 degrees, pointed segments): bits A=0 B C D E F G1 G2 H I J K L
// M=13. Lit segments as light, unlit as ghosts. (ox, oz) = its bottom-left in panel mm, hgt its height.
void seg14_glyph(cairo_t* cr, const Geo& g, double ox, double oz, double hgt, uint16_t mask, const double col[3],
                 double level, bool ghosts = true)
{
    const double wdt = hgt * 0.60, t = hgt * 0.085, gp = t * 0.75, sl = std::tan(6.0 * M_PI / 180.0);
    auto P = [&](double x, double z, double& px, double& py) {
        px = g.X(ox + x + (z - hgt / 2) * sl);
        py = g.Y(oz + z);
    };
    const double x0 = 0, x1 = wdt, xm = wdt / 2, z0 = 0, z1 = hgt, zm = hgt / 2;
    const double S[14][4] = {
        { x0 + gp, z1, x1 - gp, z1 }, { x1, z1 - gp, x1, zm + gp }, { x1, zm - gp, x1, z0 + gp },
        { x0 + gp, z0, x1 - gp, z0 }, { x0, zm - gp, x0, z0 + gp }, { x0, z1 - gp, x0, zm + gp },
        { x0 + gp, zm, xm - gp * 0.6, zm }, { xm + gp * 0.6, zm, x1 - gp, zm },
        { x0 + gp * 1.6, z1 - gp * 1.6, xm - gp * 0.9, zm + gp * 1.4 }, { xm, z1 - gp, xm, zm + gp },
        { x1 - gp * 1.6, z1 - gp * 1.6, xm + gp * 0.9, zm + gp * 1.4 },
        { x0 + gp * 1.6, z0 + gp * 1.6, xm - gp * 0.9, zm - gp * 1.4 }, { xm, zm - gp, xm, z0 + gp },
        { x1 - gp * 1.6, z0 + gp * 1.6, xm + gp * 0.9, zm - gp * 1.4 },
    };
    const double w = g.S(t);
    for (int pass = 0; pass < 2; ++pass)          // ghosts first, then the light over them
        for (int bit = 0; bit < 14; ++bit) {
            const bool on = ((mask >> bit) & 1u) && level > 0;
            if (on != (pass == 1)) continue;
            if (!on && !ghosts) continue;     // the static layer has them
            double ax, ay, bx, by;
            P(S[bit][0], S[bit][1], ax, ay);
            P(S[bit][2], S[bit][3], bx, by);
            // pointed ends: a segment is a line with butt ends shortened by half its width, and a diamond cap
            auto path = [&]() {
                const double dx = bx - ax, dy = by - ay, len = std::hypot(dx, dy);
                if (len < 1e-6) return;
                const double ux = dx / len, uy = dy / len, nx = -uy * w * 0.5, ny = ux * w * 0.5;
                cairo_move_to(cr, ax, ay);
                cairo_line_to(cr, ax + ux * w * 0.5 + nx, ay + uy * w * 0.5 + ny);
                cairo_line_to(cr, bx - ux * w * 0.5 + nx, by - uy * w * 0.5 + ny);
                cairo_line_to(cr, bx, by);
                cairo_line_to(cr, bx - ux * w * 0.5 - nx, by - uy * w * 0.5 - ny);
                cairo_line_to(cr, ax + ux * w * 0.5 - nx, ay + uy * w * 0.5 - ny);
                cairo_close_path(cr);
            };
            if (!on) {
                path();
                cairo_set_source_rgb(cr, GHOST[0], GHOST[1], GHOST[2]);
                cairo_fill(cr);
            } else {
                // light: the filled segment, a halo stroked round it, and a whitish core along its axis
                path();
                cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
                cairo_set_line_width(cr, w * 1.4);
                cairo_set_source_rgba(cr, col[0], col[1], col[2], 0.16 * level);
                cairo_stroke_preserve(cr);
                cairo_set_source_rgba(cr, col[0], col[1], col[2], 0.95 * level);
                cairo_fill(cr);
                cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
                cairo_set_line_width(cr, w * 0.32);
                cairo_move_to(cr, ax + (bx - ax) * 0.18, ay + (by - ay) * 0.18);
                cairo_line_to(cr, bx - (bx - ax) * 0.18, by - (by - ay) * 0.18);
                cairo_set_source_rgba(cr, col[0] + (1 - col[0]) * 0.7, col[1] + (1 - col[1]) * 0.7,
                                      col[2] + (1 - col[2]) * 0.7, 0.8 * level);
                cairo_stroke(cr);
            }
        }
}

// A cut-out from the renders, pasted at its place (pixel origin in the photograph).
void paste(cairo_t* cr, const Geo& g, cairo_surface_t* img, int px, int py)
{
    if (!img) return;
    const double s = g.W / L::PHOTO_W;
    cairo_save(cr);
    cairo_translate(cr, px * s, py * s);
    cairo_scale(cr, s, s);
    cairo_set_source_surface(cr, img, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_paint(cr);
    cairo_restore(cr);
}

// The static layer, built once per size and view and stamped every frame: the photograph scaled to the content box
// (device pixels) with the LED segments that never change drawn unlit -- their "ghosts": the level strip, the note
// screen's two glyphs, and in the chevron view the chevrons and the centre bar. view: 0 chevrons, 1 strobe, 2 strum.
void ensure_static(HawkUI* ui, const Geo& g, int view)
{
    const int w = (int)std::lround(g.W), h = (int)std::lround(g.H);
    if (ui->static_layer && ui->static_w == w && ui->static_h == h && ui->static_view == view) return;
    if (ui->static_layer) cairo_surface_destroy(ui->static_layer);
    ui->static_layer = nullptr;
    if (w <= 0 || h <= 0) return;
    ui->static_layer = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
    ui->static_w = w; ui->static_h = h; ui->static_view = view;
    cairo_t* c = cairo_create(ui->static_layer);
    if (ui->panel) {
        cairo_save(c);
        cairo_scale(c, (double)w / L::PHOTO_W, (double)h / L::PHOTO_H);
        cairo_set_source_surface(c, ui->panel, 0, 0);
        // GOOD, not BEST: BEST cost 39-76 ms per rebuild (a resize drag pays one per step), GOOD 0.2-4 ms
        cairo_pattern_set_filter(cairo_get_source(c), CAIRO_FILTER_GOOD);
        cairo_paint(c);
        cairo_restore(c);
    } else {                                      // no photograph in the bundle: a plain dark face
        cairo_set_source_rgb(c, 0.02, 0.02, 0.024);
        cairo_paint(c);
    }
    {   // the level line's unlit track
        const double* r = L::LEVEL_SCREEN;
        const double sh = (r[3] - r[1]) * L::LEVEL_HEIGHT, zc = (r[1] + r[3]) / 2;
        const double x0 = r[0] + L::LEVEL_PAD, x1 = r[2] - L::LEVEL_PAD;
        rrect_path(c, g.X(x0), g.Y(zc + sh / 2), g.S(x1 - x0), g.S(sh), g.S(sh / 2));
        cairo_set_source_rgb(c, GHOST[0], GHOST[1], GHOST[2]);
        cairo_fill(c);
    }
    if (view != 2) {   // the note screen's two glyphs
        const double gh = L::GLYPH_H;
        seg14_glyph(c, g, L::NOTE_CX - gh * 0.58, L::GLYPH_Z, gh, 0, COL_INTUNE, 0.0);
        seg14_glyph(c, g, L::NOTE_CX + gh * 0.36, L::GLYPH_Z, gh * 0.42, 0, COL_INTUNE, 0.0);
    }
    if (view == 0) {   // the chevrons and the centre bar
        for (int k = 1; k <= L::NCH; ++k) {
            const double d = L::CGAP + (k - 0.5) * L::STEP;
            for (int side = -1; side <= 1; side += 2) {
                const double x = L::MCX + side * d, cw = L::CHEV_W / 2;
                ghost_stroke(c, [&]() {
                    const double tipx = x + (side < 0 ? cw : -cw), backx = x + (side < 0 ? -cw : cw);
                    cairo_move_to(c, g.X(backx), g.Y(L::CZ + L::CHEV_H));
                    cairo_line_to(c, g.X(tipx),  g.Y(L::CZ));
                    cairo_line_to(c, g.X(backx), g.Y(L::CZ - L::CHEV_H));
                }, g.S(L::STROKE));
            }
        }
        ghost_stroke(c, [&]() {
            cairo_move_to(c, g.X(L::MCX), g.Y(L::CZ - L::BAR_H / 2 + L::BAR_W / 2));
            cairo_line_to(c, g.X(L::MCX), g.Y(L::CZ + L::BAR_H / 2 - L::BAR_W / 2));
        }, g.S(L::BAR_W));
    }
    cairo_destroy(c);
}

// The strobe disc's motion: see the comments inside. Returns whether the disc shows
// a note (live, or riding through a re-pluck) and, through s_intune, its in-tune state.
bool update_strobe(HawkUI* ui, bool voiced, bool intune, bool& s_intune)
{
    // A re-pluck must not stop the disc: the plugin skips the windows holding an attack (~0.3 s with no new phase),
    // and a pick that stops the string reads as the note ending (~0.3 s unvoiced). Through either the disc keeps
    // sliding at the last offset for up to STROBE_HOLD; and a fresh phase after a gap longer than STROBE_GAP is taken
    // as a new start, not a jump (a re-plucked string starts anew).
    constexpr double STROBE_GAIN = 3.0;   // stripe periods per cycle of offset (one, as a real disc, crawled)
    constexpr double STROBE_CAP  = 6.0;   // stripe periods per second, at most
    constexpr double STROBE_HOLD = 0.6;   // s the disc rides through no note
    constexpr double STROBE_GAP  = 0.15;  // s without a fresh phase that is a gap (hop ~42 ms)
    const auto now = std::chrono::steady_clock::now();
    auto since = [&](std::chrono::steady_clock::time_point t) {
        return std::chrono::duration<double>(now - t).count();
    };
    const bool live = voiced && ui->note >= 0.0f;
    if (live) {
        ui->strobe_seen_t = now;
        ui->strobe_note   = ui->note;
        ui->strobe_cents  = ui->cents;
        ui->strobe_intune = intune;
    }
    const bool sounding = live || (ui->strobe_init && since(ui->strobe_seen_t) < STROBE_HOLD);
    s_intune = live ? intune : ui->strobe_intune;
    if (sounding) {
        const double f0  = ui->a4_ref * std::pow(2.0, (ui->strobe_note - 69.0) / 12.0);
        const double off = f0 * (std::pow(2.0, ui->strobe_cents / 1200.0) - 1.0);   // Hz
        const double vel = std::fmin(std::fmax(STROBE_GAIN * off, -STROBE_CAP), STROBE_CAP);
        const bool fresh = live && ui->strobe_phase != ui->strobe_last;
        if (!ui->strobe_init) {
            ui->strobe_pos   = ui->strobe_phase / (2.0 * M_PI);
            ui->strobe_extra = 0.0;
        } else {
            double dt = since(ui->strobe_t);
            dt = std::fmin(std::fmax(dt, 0.0), 0.1);
            if (!fresh) {                         // between readings, or none: ride on
                ui->strobe_pos   += vel * dt;
                ui->strobe_extra += vel * dt;
            } else if (since(ui->strobe_fresh_t) > STROBE_GAP
                       || std::fabs(off) * STROBE_GAIN > STROBE_CAP) {
                ui->strobe_pos  += vel * dt;      // a new start, or too fast to follow
                ui->strobe_extra = 0.0;
            } else {                              // the true strobe: follow the phase
                ui->strobe_pos  += STROBE_GAIN * std::remainder((double)ui->strobe_phase - ui->strobe_last,
                                                                2.0 * M_PI) / (2.0 * M_PI)
                                 - ui->strobe_extra;
                ui->strobe_extra = 0.0;
            }
        }
        if (fresh || !ui->strobe_init) {
            ui->strobe_last    = ui->strobe_phase;
            ui->strobe_fresh_t = now;
        }
        ui->strobe_pos -= std::floor(ui->strobe_pos);
        ui->strobe_t    = now;
        ui->strobe_init = true;
    } else {
        ui->strobe_init = false;                  // pick the phase up afresh on the next note
    }
    return sounding;
}

// The strobe disc in the chevron screen: the rim of a coloured disc turning behind the screen, a matt
// band of amber and blue stripes blending into each other, narrowing towards the ends; two shades of green in tune;
// grey and still with no note.
void draw_strobe(cairo_t* cr, const Geo& g, HawkUI* ui, bool sounding, bool s_intune)
{
    constexpr int    NSTRIPE      = 8;      // stripe periods across the visible arc
    constexpr double TH           = 1.05;   // half the visible arc, rad (60 deg)
    constexpr double STROBE_BLEND = 0.30;   // of a stripe, blended into the next at either side
    const double x0 = g.X(L::MCX - L::MHALF), x1 = g.X(L::MCX + L::MHALF);
    const double sh = g.S((L::METER_SCREEN[3] - L::METER_SCREEN[1]) * L::DISC_HEIGHT);
    const double sy = g.Y(L::CZ) - sh * 0.5;
    const double xm = 0.5 * (x0 + x1), xr = 0.5 * (x1 - x0);
    auto proj = [&](double u) {
        double a = u * 2.0 * TH / NSTRIPE;
        a = std::fmin(std::fmax(a, -M_PI_2), M_PI_2);
        return xm + xr * std::sin(a) / std::sin(TH);
    };
    const double* ca = !sounding ? COL_DIM : (s_intune ? COL_STROBE_TUNE : COL_SHARP);
    const double* cb = !sounding ? COL_DIM : (s_intune ? COL_STROBE_TUNE : COL_FLAT);
    const double  aa = !sounding ? 0.34 : 0.95;
    const double  ab = !sounding ? 0.20 : (s_intune ? 0.45 : 0.95);   // in tune: a darker shade
    const int     kmax = NSTRIPE / 2 + 2;
    cairo_pattern_t* band = cairo_pattern_create_linear(x0, 0, x1, 0);
    for (int k = -kmax; k <= kmax; ++k)
        for (int h = 0; h < 2; ++h) {
            const double  u0  = k + h * 0.5 + ui->strobe_pos;    // sharp -> slides right
            const double* col = h ? cb : ca;
            const double  al  = h ? ab : aa;
            for (double u : { u0 + 0.5 * STROBE_BLEND * 0.5, u0 + 0.5 * (1.0 - STROBE_BLEND * 0.5) }) {
                const double t = (proj(u) - x0) / (x1 - x0);
                if (t > 0.0 && t < 1.0)
                    cairo_pattern_add_color_stop_rgba(band, t, col[0], col[1], col[2], al);
            }
        }
    cairo_rectangle(cr, x0, sy, x1 - x0, sh);
    cairo_set_source(cr, band);
    cairo_fill(cr);
    cairo_pattern_destroy(band);
    // the rim's curvature: matt, darker at top and bottom, no highlight
    cairo_pattern_t* rim = cairo_pattern_create_linear(0, sy, 0, sy + sh);
    cairo_pattern_add_color_stop_rgba(rim, 0.00, 0, 0, 0, 0.45);
    cairo_pattern_add_color_stop_rgba(rim, 0.30, 0, 0, 0, 0.0);
    cairo_pattern_add_color_stop_rgba(rim, 0.70, 0, 0, 0, 0.0);
    cairo_pattern_add_color_stop_rgba(rim, 1.00, 0, 0, 0, 0.55);
    cairo_rectangle(cr, x0, sy, x1 - x0, sh);
    cairo_set_source(cr, rim);
    cairo_fill(cr);
    cairo_pattern_destroy(rim);
    // the disc's edge curving away at both ends
    cairo_pattern_t* edge = cairo_pattern_create_linear(x0, 0, x1, 0);
    cairo_pattern_add_color_stop_rgba(edge, 0.00, 0, 0, 0, 0.85);
    cairo_pattern_add_color_stop_rgba(edge, 0.18, 0, 0, 0, 0.0);
    cairo_pattern_add_color_stop_rgba(edge, 0.82, 0, 0, 0, 0.0);
    cairo_pattern_add_color_stop_rgba(edge, 1.00, 0, 0, 0, 0.85);
    cairo_rectangle(cr, x0, sy, x1 - x0, sh);
    cairo_set_source(cr, edge);
    cairo_fill(cr);
    cairo_pattern_destroy(edge);
}

// A floating window of dark smoked glass with a fine amber edge, with its soft shadow on the face.
void smoked_window(cairo_t* cr, const Geo& g, const double win[4])
{
    const double x0 = g.X(win[0]), x1 = g.X(win[2]), y0 = g.Y(win[3]), y1 = g.Y(win[1]);
    const double r = g.S(L::SET_R);
    for (int k = 2; k >= 0; --k) {                 // the shadow: offset down and right, three soft layers
        const double grow = g.S(1.2 * k), off = g.S(2.0);
        rrect_path(cr, x0 + off - grow, y0 + off - grow, x1 - x0 + 2 * grow, y1 - y0 + 2 * grow, r + grow);
        cairo_set_source_rgba(cr, 0, 0, 0, L::SET_SHADOW);
        cairo_fill(cr);
    }
    rrect_path(cr, x0, y0, x1 - x0, y1 - y0, r);
    cairo_set_source_rgba(cr, 0.016, 0.016, 0.020, 0.965);   // the render: smoke 0.82 over a tinted glass
    cairo_fill(cr);
    const double in = g.S(0.6);
    rrect_path(cr, x0 + in, y0 + in, x1 - x0 - 2 * in, y1 - y0 - 2 * in, std::fmax(r - in, 0.5));
    cairo_set_line_width(cr, std::fmax(1.0, g.S(L::SET_BORDER)));
    cairo_set_source_rgba(cr, COL_SHARP[0], COL_SHARP[1], COL_SHARP[2], 0.85);
    cairo_stroke(cr);
}

// The rectangle of an outline button (CLOSE's style), in content pixels: layout() and the drawing share it.
Rect outline_rect(const Geo& g, double x0, double x1, double z, double h)
{
    return Rect{ g.X(x0), g.Y(z + h / 2), g.S(x1 - x0), g.S(h) };
}

// CLOSE's style: a thin outline and its legend.
void outline_button(cairo_t* cr, HawkUI* ui, const Geo& g, const char* text, double x0, double x1, double z, double h,
                    double size, bool on)
{
    const Rect r = outline_rect(g, x0, x1, z, h);
    rrect_path(cr, r.x, r.y, r.w, r.h, g.S(1.2));
    cairo_set_line_width(cr, std::fmax(1.0, g.S(0.35)));
    cairo_set_source_rgb(cr, INK_DIM[0], INK_DIM[1], INK_DIM[2]);
    cairo_stroke(cr);
    panel_text(cr, ui, g, text, size, (x0 + x1) / 2, z, 0, on ? INK_WHITE : INK_DIM, on ? 0.95 : 0.6);
}

// A dropdown printed on the panel ("440 Hz v", the tuning): text and a caret as one group, anchored by its centre,
// left or right edge. Returns the group's width in mm; records nothing.
double menu_group(cairo_t* cr, HawkUI* ui, const Geo& g, const char* text, double x, double z, double size, int anchor,
                  const double rgb[3], bool draw)
{
    const double tw = panel_text(cr, ui, g, text, size, 0, 0, -1, rgb, 1.0, true) / g.k;
    const double cw = size * 0.55, gap = size * 0.35, gw = tw + gap + cw;
    const double gl = anchor == 0 ? x - gw / 2 : anchor < 0 ? x : x - gw;
    if (draw) {
        panel_text(cr, ui, g, text, size, gl, z, -1, rgb, 1.0);
        const double cx = gl + tw + gap + cw / 2, h = cw * 0.55;
        cairo_move_to(cr, g.X(cx - cw / 2), g.Y(z + h / 2));
        cairo_line_to(cr, g.X(cx + cw / 2), g.Y(z + h / 2));
        cairo_line_to(cr, g.X(cx), g.Y(z - h / 2));
        cairo_close_path(cr);
        cairo_set_source_rgb(cr, rgb[0], rgb[1], rgb[2]);
        cairo_fill(cr);
    }
    return gw;
}

// The name printed under the buttons.
const char* product_name()
{
    return "THE MONO HAWK";
}

// The state every part reads the same way.
struct Flags {
    bool enabled, voiced, intune, strum, strobe, ref;
};
Flags flags(const HawkUI* ui)
{
    Flags f;
    f.enabled = ui->enabled > 0.5f;
    f.voiced  = ui->voiced  > 0.5f && f.enabled;
    f.intune  = ui->in_tune > 0.5f && f.voiced;
    f.strum = false;
    f.strobe = ui->strobe_mode > 0.5f && f.enabled;
    f.ref     = ui->ref_active > 0.5f;
    return f;
}

// Advance what moves with time and the plugin's readings: the tuning progress, DETECT TUNING, the reference tone's
// follow, the strobe disc. Run on every idle tick, drawn or not.
void update(HawkUI* ui)
{
    const Flags f = flags(ui);
    update_tuning_progress(ui, f.voiced, f.intune);
    update_detect(ui, f.voiced, f.strum);
    update_ref_follow(ui);
    bool s_intune = false;
    ui->strobe_sounding = update_strobe(ui, f.voiced, f.intune, s_intune);
    ui->strobe_s_intune = s_intune;
}

// Where everything is, from the window's size and the state (into ui->lay). `cr` only MEASURES text -- a context on
// the surface that is drawn, so the measures are the drawing's. Draws nothing. Keeps an open dropdown's scroll in range.
void layout(HawkUI* ui, cairo_t* cr)
{
    Layout& l = ui->lay;
    const double aw = ui->width, ah = ui->height;
    constexpr double ASPECT = L::PANEL_W / L::PANEL_H;
    double W = aw, H = ah;
    if (aw / ah > ASPECT) W = ah * ASPECT;   // window too wide -> pillarbox
    else                  H = aw / ASPECT;   // window too tall -> letterbox
    l.ox = std::floor((aw - W) * 0.5);
    l.oy = std::floor((ah - H) * 0.5);
    l.W = W; l.H = H; l.k = W / L::PANEL_W;
    const Geo g{ W, H, l.k };
    const Flags f = flags(ui);

    // the letters' row: where each string sits, the tuning's name in a gap in the middle that follows the name; a
    // name too long for the gap's cap shrinks its type
    int ti = (int)std::lround(ui->tuning);
    if (ti < 0) ti = 0;
    if (ti >= TUNING_COUNT) ti = TUNING_COUNT - 1;
    l.ti = ti;
    l.nstr = TUNINGS[ti].count;
    l.tsz = L::TUNING_SIZE;
    for (;;) {
        const double gw = menu_group(cr, ui, g, TUNINGS[ti].name, L::MCX, L::MENU_Z, l.tsz, 0, INK_AMBER, false);
        l.gap_half = std::fmax(L::ROW_GAP_HALF, gw / 2 + L::ROW_PAD);
        if (l.gap_half <= L::ROW_GAP_MAX_HALF || l.tsz < L::TUNING_SIZE * 0.6) break;
        l.tsz *= 0.94;
    }
    {
        const int nl = (l.nstr + 1) / 2, nr = l.nstr - nl;
        for (int i = 0; i < l.nstr && i < 8; ++i) {
            if (i < nl) l.letter_x[i] = L::MCX - L::ROW_D_MAX + (L::ROW_D_MAX - l.gap_half) * i / std::max(nl - 1, 1);
            else        l.letter_x[i] = L::MCX + l.gap_half + (L::ROW_D_MAX - l.gap_half) * (i - nl) / std::max(nr - 1, 1);
        }
    }
    l.row = Rect{ g.X(L::MCX - L::ROW_D_MAX - L::ROW_SIZE * 0.6), g.Y(L::ROW_Z + L::ROW_SIZE * 0.6),
                  g.S(2 * L::ROW_D_MAX + L::ROW_SIZE * 1.2), g.S(L::ROW_SIZE * 1.2) };

    l.note = Rect{ g.X(L::NOTE_SCREEN[0]), g.Y(L::NOTE_SCREEN[3]),
                   g.S(L::NOTE_SCREEN[2] - L::NOTE_SCREEN[0]), g.S(L::NOTE_SCREEN[3] - L::NOTE_SCREEN[1]) };

    l.ref_tag = Rect{};                                  // REF and its octave, in the cents slot, while TONE drones
    if (f.ref) {
        char cbuf[32];
        if (ui->ref_octave > 0) std::snprintf(cbuf, sizeof cbuf, "REF  8va+%d", ui->ref_octave);
        else                    std::snprintf(cbuf, sizeof cbuf, "REF");
        const double tw = panel_text(cr, ui, g, cbuf, L::MENU_SIZE, L::CENTS_X, L::MENU_Z, -1, COL_SHARP, 1.0, true);
        l.ref_tag = Rect{ g.X(L::CENTS_X), g.Y(L::MENU_Z + L::MENU_SIZE * 0.5), tw, g.S(L::MENU_SIZE) };
    }
    {
        char abuf[24];
        std::snprintf(abuf, sizeof abuf, "%d Hz", (int)std::lround(ui->a4_ref));
        const double aw_mm = menu_group(cr, ui, g, abuf, L::A4_X, L::MENU_Z, L::MENU_SIZE, 1, INK_AMBER, false);
        l.a4_menu = Rect{ g.X(L::A4_X - aw_mm) - 2, g.Y(L::MENU_Z + L::MENU_SIZE * 0.6), g.S(aw_mm) + 4,
                          g.S(L::MENU_SIZE * 1.2) };
    }
    {   // the name, centred under the four buttons, shrunk to their span if it does not fit
        l.nsz = L::NAME_SIZE;
        const double span = L::NAME_X1 - L::NAME_X0;
        const double nw = panel_text(cr, ui, g, product_name(), l.nsz, 0, 0, -1, INK_AMBER, 1.0, true) / g.k;
        if (nw > span) l.nsz *= span / nw;
        const double nw2 = panel_text(cr, ui, g, product_name(), l.nsz, 0, 0, -1, INK_AMBER, 1.0, true) / g.k;
        l.name = Rect{ g.X((L::NAME_X0 + L::NAME_X1) / 2 - nw2 / 2) - 3, g.Y(L::NAME_Z + l.nsz * 0.6), g.S(nw2) + 6,
                       g.S(l.nsz * 1.2) };
    }
    for (int b = 0; b < 4; ++b) {
        const auto& B = L::BUTTONS[b];
        l.button[b] = Rect{ g.X(B.x0), g.Y(B.z1), g.S(B.x1 - B.x0), g.S(B.z1 - B.z0) };
    }
    l.logo_cx = g.X(L::LOGO_CX); l.logo_cy = g.Y(L::LOGO_CZ); l.logo_r = g.S(L::LOGO_R);

    l.dd_panel = Rect{}; l.dd_item_h = 0;
    if (ui->dd_open) {
        const bool is_tuning = (ui->dd_which == 1);
        if (is_tuning) ensure_disp();
        const int count = is_tuning ? g_disp_n : A4_COUNT;
        const double* win = is_tuning ? L::DD_TUNING_WIN : L::DD_A4_WIN;
        const double pad = is_tuning ? L::DDT_PAD : L::DDA_PAD;
        if (ui->dd_scroll < 0) ui->dd_scroll = 0;
        if (ui->dd_scroll > count - DD_VISIBLE) ui->dd_scroll = count - DD_VISIBLE;
        const double rh = (win[3] - win[1] - 2 * pad) / DD_VISIBLE;
        l.dd_panel = Rect{ g.X(win[0]), g.Y(win[3] - pad), g.S(win[2] - win[0]), g.S(rh * DD_VISIBLE) };
        l.dd_item_h = g.S(rh);
    }
    {
        const double cx = (L::DET_X0 + L::DET_X1) / 2, bw = L::DET_BUTTON_W, gp = L::DET_BUTTON_GAP;
        const double xs[3] = { cx - 1.5 * bw - gp, cx - 0.5 * bw, cx + 0.5 * bw + gp };
        l.det_apply  = outline_rect(g, xs[0], xs[0] + bw, L::DET_BUTTON_Z, L::SET_CLOSE_H);
        l.det_clear  = outline_rect(g, xs[1], xs[1] + bw, L::DET_BUTTON_Z, L::SET_CLOSE_H);
        l.det_cancel = outline_rect(g, xs[2], xs[2] + bw, L::DET_BUTTON_Z, L::SET_CLOSE_H);
    }
    l.set_panel = Rect{ g.X(L::SET_X0), g.Y(L::SET_Z1), g.S(L::SET_X1 - L::SET_X0), g.S(L::SET_Z1 - L::SET_Z0) };
    for (int r = 0; r < 3; ++r)
        l.slider[r] = Rect{ g.X(L::SET_TRACK_X0), g.Y(L::SET_ROW_Z[r] + L::SET_KNOB_R * 1.6),
                            g.S(L::SET_TRACK_X1 - L::SET_TRACK_X0), g.S(L::SET_KNOB_R * 3.2) };
    l.ping = Rect{ g.X(L::SET_PING_X), g.Y(L::SET_PING_Z + L::SET_PILL_H / 2), g.S(L::SET_PILL_W), g.S(L::SET_PILL_H) };
    l.set_close = outline_rect(g, L::SET_CLOSE_X0, L::SET_CLOSE_X1, L::SET_PING_Z, L::SET_CLOSE_H);
}

void draw(HawkUI* ui)
{
    if (!ui->surface) return;

    cairo_t* cr = cairo_create(ui->surface);
    cairo_push_group(cr);            // double-buffer this frame (kills flicker)

    // Letterbox to the panel's own proportions: black bars fill any remainder.
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_rectangle(cr, 0, 0, ui->width, ui->height);
    cairo_fill(cr);
    layout(ui, cr);
    const Layout& l = ui->lay;
    cairo_translate(cr, l.ox, l.oy);
    const Geo g{ l.W, l.H, l.k };

    const Flags fl = flags(ui);
    const bool enabled = fl.enabled, voiced = fl.voiced, intune = fl.intune, strum = fl.strum, strobe = fl.strobe;
    const bool ref = fl.ref, complete = ui->complete;
    const Tuning& tu = TUNINGS[l.ti];
    const int nstr = l.nstr;
    const double* letter_x = l.letter_x;
    // ---------------- the photograph and the unlit segments
    ensure_static(ui, g, strum ? 2 : strobe ? 1 : 0);
    if (ui->static_layer) {
        cairo_set_source_surface(cr, ui->static_layer, 0, 0);
        cairo_paint(cr);
    }

    // ---------------- the level line ("is it hearing me?"): one continuous amber line, the old
    // meter's sqrt display law
    {
        const double* r = L::LEVEL_SCREEN;
        const double sh = (r[3] - r[1]) * L::LEVEL_HEIGHT, zc = (r[1] + r[3]) / 2;
        const double x0 = r[0] + L::LEVEL_PAD, x1 = r[2] - L::LEVEL_PAD;
        const double lv = enabled ? std::sqrt(std::fmax(0.0, std::fmin(1.0, (double)ui->level))) : 0.0;
        if (lv > 0.002) {
            const double xe = x0 + (x1 - x0) * lv;
            led_stroke(cr, [&]() {
                cairo_move_to(cr, g.X(x0 + sh / 2), g.Y(zc));
                cairo_line_to(cr, g.X(std::fmax(xe - sh / 2, x0 + sh / 2)), g.Y(zc));
            }, g.S(sh), COL_SHARP, 0.9);
        }
    }

    // ---------------- the meter screen: chevrons + pointer, or the strobe disc
    {
        if (strobe) {
            draw_strobe(cr, g, ui, ui->strobe_sounding, ui->strobe_s_intune);
        } else {
            // nine equal chevrons a side pointing at the centre, brightest near it; the centre bar
            // is the POINTER, blue/amber off tune, white and centred in tune
            double cc = ui->cents;
            if (cc >  L::RANGE_C) cc =  L::RANGE_C;
            if (cc < -L::RANGE_C) cc = -L::RANGE_C;
            for (int k = 1; k <= L::NCH; ++k) {
                const double d = L::CGAP + (k - 0.5) * L::STEP;
                // the chevron lights half a step after the pointer passes its mouth
                const double thresh = (d + L::CHEV_W / 2 + 0.5 * L::STEP) / L::MHALF * L::RANGE_C;
                const double lvl = 1.0 - L::SIDE_FADE * (k - 1) / (L::NCH - 1);
                for (int side = -1; side <= 1; side += 2) {
                    const bool on = voiced && !intune && (side < 0 ? -cc >= thresh : cc >= thresh);
                    const double x = L::MCX + side * d, cw = L::CHEV_W / 2;
                    auto path = [&]() {
                        const double tipx = x + (side < 0 ? cw : -cw), backx = x + (side < 0 ? -cw : cw);
                        cairo_move_to(cr, g.X(backx), g.Y(L::CZ + L::CHEV_H));
                        cairo_line_to(cr, g.X(tipx),  g.Y(L::CZ));
                        cairo_line_to(cr, g.X(backx), g.Y(L::CZ - L::CHEV_H));
                    };
                    if (on) led_stroke(cr, path, g.S(L::STROKE), side < 0 ? COL_FLAT : COL_SHARP, lvl);
                }
            }
            auto bar = [&](double xc) {
                return [&cr, &g, xc]() {
                    cairo_move_to(cr, g.X(xc), g.Y(L::CZ - L::BAR_H / 2 + L::BAR_W / 2));
                    cairo_line_to(cr, g.X(xc), g.Y(L::CZ + L::BAR_H / 2 - L::BAR_W / 2));
                };
            };
            if (intune) {
                led_stroke(cr, bar(L::MCX), g.S(L::BAR_W), COL_INTUNE, 1.0);
            } else {
                if (voiced && std::fabs(ui->cents) <= L::RANGE_C * 4) {
                    const double xp = L::MCX + cc / L::RANGE_C * L::MHALF;
                    led_stroke(cr, bar(xp), g.S(L::BAR_W), ui->cents < 0 ? COL_FLAT : COL_SHARP, 1.0);
                }
            }
        }
    }

    // ---------------- the note screen: the 14-segment note (blue flat, amber sharp, white in tune), or REF
    {
        const double sx0 = g.X(L::NOTE_SCREEN[0]), sw = g.S(L::NOTE_SCREEN[2] - L::NOTE_SCREEN[0]);
        const double sy0 = g.Y(L::NOTE_SCREEN[3]), shh = g.S(L::NOTE_SCREEN[3] - L::NOTE_SCREEN[1]);
        {
            int midi = -1;
            const double* col = COL_INTUNE;
            if (ref) {                                       // TONE: the droned string, amber
                midi = ref_draw_note(ui);
                col = COL_SHARP;
            } else if (voiced && ui->note >= 0.0f) {
                midi = (int)std::lround(ui->note);
                col = intune ? COL_INTUNE : (ui->cents < 0 ? COL_FLAT : COL_SHARP);
            }
            const double gh = L::GLYPH_H;
            uint16_t m_note = 0, m_oct = 0;
            bool sharp = false;
            if (midi >= 0) {
                const int pc = ((midi % 12) + 12) % 12, oct = midi / 12 - 1;
                m_note = seg14_mask(NOTE_NAMES[pc][0]);
                sharp  = NOTE_NAMES[pc][1] == '#';
                if (oct >= 0 && oct <= 9) m_oct = seg14_mask((char)('0' + oct));
            }
            seg14_glyph(cr, g, L::NOTE_CX - gh * 0.58, L::GLYPH_Z, gh, m_note, col, midi >= 0 ? 1.0 : 0.0, false);
            seg14_glyph(cr, g, L::NOTE_CX + gh * 0.36, L::GLYPH_Z, gh * 0.42, m_oct, col, midi >= 0 ? 0.85 : 0.0, false);
            if (sharp) {   // the sharp sign over the octave digit, as the old display
                const double hs = gh * 0.30, hx = L::NOTE_CX + gh * 0.36 + gh * 0.03, hz = L::GLYPH_Z + gh * 0.62;
                auto path = [&]() {
                    cairo_move_to(cr, g.X(hx + hs * 0.34), g.Y(hz + hs)); cairo_line_to(cr, g.X(hx + hs * 0.30), g.Y(hz));
                    cairo_move_to(cr, g.X(hx + hs * 0.70), g.Y(hz + hs)); cairo_line_to(cr, g.X(hx + hs * 0.66), g.Y(hz));
                    cairo_move_to(cr, g.X(hx), g.Y(hz + hs * 0.66));      cairo_line_to(cr, g.X(hx + hs), g.Y(hz + hs * 0.66));
                    cairo_move_to(cr, g.X(hx), g.Y(hz + hs * 0.34));      cairo_line_to(cr, g.X(hx + hs), g.Y(hz + hs * 0.34));
                };
                led_stroke(cr, path, g.S(gh * 0.05), col, 0.85);
            }
        }
    }

    // ---------------- the printed row: cents (or REF), the A4 menu, the letters, the tuning menu, the name
    {
        char cbuf[32];
        if (ref) {
            if (ui->ref_octave > 0) std::snprintf(cbuf, sizeof cbuf, "REF  8va+%d", ui->ref_octave);
            else                    std::snprintf(cbuf, sizeof cbuf, "REF");
            panel_text(cr, ui, g, cbuf, L::MENU_SIZE, L::CENTS_X, L::MENU_Z, -1, COL_SHARP, 1.0);
        } else {
            // always shown; with nothing playing it reads 0.0
            if (voiced && ui->note >= 0.0f) std::snprintf(cbuf, sizeof cbuf, "%+.1f \xC2\xA2", ui->cents);
            else                            std::snprintf(cbuf, sizeof cbuf, "0.0 \xC2\xA2");
            panel_text(cr, ui, g, cbuf, L::MENU_SIZE, L::CENTS_X, L::MENU_Z, -1, INK_WHITE, 1.0);
        }
        char abuf[24];
        std::snprintf(abuf, sizeof abuf, "%d Hz", (int)std::lround(ui->a4_ref));
        menu_group(cr, ui, g, abuf, L::A4_X, L::MENU_Z, L::MENU_SIZE, 1, INK_AMBER, true);
        menu_group(cr, ui, g, tu.name, L::MCX, L::MENU_Z, l.tsz, 0, INK_AMBER, true);

        // the letters: a backlit print -- amber once that string is tuned (latched) or while TONE drones
        // it, white while it is the string sounding, the plain print otherwise
        const int played = voiced && !strum ? (int)std::lround(ui->note) : -1;
        for (int i = 0; i < nstr && i < 8; ++i) {
            const bool drone  = ref && i == ui->ref_string;
            const bool tuned  = i < ui->tgt_count && ui->tgt_lit[i];
            const bool active = played >= 0 && i < ui->tgt_count && ui->tgt_midi[i] == played;
            const char* s = tu.notes[i];
            if (tuned || drone || active) {
                const double* c = (tuned || drone) ? COL_SHARP : COL_INTUNE;
                // a soft glow under the lit print
                if (ui->font) cairo_set_font_face(cr, ui->font);
                cairo_set_font_size(cr, g.S(L::ROW_SIZE * TEXT_EM));
                cairo_text_extents_t te, hc;
                cairo_text_extents(cr, s, &te);
                cairo_text_extents(cr, "H", &hc);
                cairo_move_to(cr, g.X(letter_x[i]) - te.x_advance * 0.5, g.Y(L::ROW_Z) - (hc.height * 0.5 + hc.y_bearing));
                cairo_text_path(cr, s);
                cairo_set_line_width(cr, g.S(0.9));
                cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
                cairo_set_source_rgba(cr, c[0], c[1], c[2], 0.22);
                cairo_stroke(cr);
                panel_text(cr, ui, g, s, L::ROW_SIZE, letter_x[i], L::ROW_Z, 0, c, 1.0);
            } else {
                panel_text(cr, ui, g, s, L::ROW_SIZE, letter_x[i], L::ROW_Z, 0, INK_WHITE, 1.0);
            }
        }

        // the name: amber while the plugin runs, white when bypassed, centred under the four buttons
        panel_text(cr, ui, g, product_name(), l.nsz, (L::NAME_X0 + L::NAME_X1) / 2, L::NAME_Z, 0,
                   enabled ? INK_AMBER : INK_WHITE, 1.0);
    }

    // ---------------- the buttons (pressed and lit: cut-outs) and the hawk
    for (int b = 0; b < 4; ++b) {
        const bool on = (b == 0 && ui->mute > 0.5f) || (b == 1 && ref) || (b == 2 && ui->strobe_mode > 0.5f)
                     || (b == 3 && ui->settings_open);
        if (on) paste(cr, g, ui->sprite[b], L::SPRITES[b].x, L::SPRITES[b].y);
    }
    // the hawk: amber while the plugin runs, brighter amber while a note is in tune or the tuning
    // is complete, dark when bypassed
    const int logo_state = !enabled ? 2 : (complete || (intune && !strum)) ? 1 : 0;
    paste(cr, g, ui->sprite[4 + logo_state], L::SPRITES[4 + logo_state].x, L::SPRITES[4 + logo_state].y);

    // ---------------- overlays: a dropdown, DETECT TUNING, or Advanced Settings
    if (ui->dd_open) {
        const bool is_tuning = (ui->dd_which == 1);
        if (is_tuning) ensure_disp();
        const int count = is_tuning ? g_disp_n : A4_COUNT;
        const double* win = is_tuning ? L::DD_TUNING_WIN : L::DD_A4_WIN;
        const double pad = is_tuning ? L::DDT_PAD : L::DDA_PAD, tsize = is_tuning ? L::DDT_TEXT_SIZE : L::DDA_TEXT_SIZE;
        const double ind = is_tuning ? L::DDT_INDENT : L::DDA_INDENT, ind_h = is_tuning ? L::DDT_INDENT_HEAD : L::DDA_INDENT_HEAD;
        smoked_window(cr, g, win);
        const double rh = (win[3] - win[1] - 2 * pad) / DD_VISIBLE;
        const int cur_tun = (int)std::lround(ui->tuning), cur_a4 = (int)std::lround(ui->a4_ref) - A4_MIN;
        for (int row = 0; row < DD_VISIBLE; ++row) {
            const int idx = ui->dd_scroll + row;
            if (idx < 0 || idx >= count) continue;
            const double z = win[3] - pad - (row + 0.5) * rh;
            const char* label;
            char ib[24];
            int kind;   // 0/1 heading, 2 item, 3 action
            bool sel;
            if (is_tuning) { label = g_disp[idx].label; kind = g_disp[idx].kind; sel = kind == 2 && g_disp[idx].tuning == cur_tun; }
            else { std::snprintf(ib, sizeof ib, "%d Hz", A4_MIN + idx); label = ib; kind = 2; sel = idx == cur_a4; }
            if (sel) {   // the current choice: a faint amber bar, its text lit amber
                rrect_path(cr, g.X(win[0] + 3), g.Y(z + rh * 0.42), g.S(win[2] - win[0] - 9), g.S(rh * 0.84), g.S(1.0));
                cairo_set_source_rgba(cr, COL_SHARP[0], COL_SHARP[1], COL_SHARP[2], 0.22);
                cairo_fill(cr);
            }
            const bool head = kind == 0 || kind == 1;
            const double* c = head ? INK_DIM : (kind == 3 || sel) ? COL_SHARP : INK_WHITE;
            panel_text(cr, ui, g, label, tsize * (head ? 0.78 : 1.0), win[0] + (head ? ind_h : ind), z, -1, c, 1.0);
        }
        // the scroll bar: where these rows sit in the whole list
        const double tx = win[2] - 3.2, tz0 = win[1] + pad, tz1 = win[3] - pad;
        rrect_path(cr, g.X(tx - 0.5), g.Y(tz1), g.S(1.0), g.S(tz1 - tz0), g.S(0.5));
        cairo_set_source_rgb(cr, 0.25, 0.25, 0.26);
        cairo_fill(cr);
        if (count > 0) {
            const double f0 = (double)ui->dd_scroll / count, f1 = std::fmin(1.0, (double)(ui->dd_scroll + DD_VISIBLE) / count);
            rrect_path(cr, g.X(tx - 0.7), g.Y(tz1 - f0 * (tz1 - tz0)), g.S(1.4), g.S((f1 - f0) * (tz1 - tz0)), g.S(0.7));
            cairo_set_source_rgb(cr, COL_SHARP[0], COL_SHARP[1], COL_SHARP[2]);
            cairo_fill(cr);
        }
    }

    if (ui->detect_mode) {
        smoked_window(cr, g, L::DETECT_WIN);
        const double cx = (L::DET_X0 + L::DET_X1) / 2;
        panel_text(cr, ui, g, "DETECT TUNING", L::SET_TITLE_SIZE, cx, L::DET_TITLE_Z, 0, COL_SHARP, 1.0);
        if (ui->detect_count == 0) {
            panel_text(cr, ui, g, "Play each open string on its own: let it ring a moment, then mute it", L::SET_TEXT_SIZE,
                       cx, L::DET_NOTES_Z, 0, INK_WHITE, 1.0);
        } else {
            char nb[80];
            int p = 0;
            for (int i = 0; i < ui->detect_count && p < (int)sizeof(nb) - 6; ++i) {
                const int pc = ((ui->detect_notes[i] % 12) + 12) % 12;
                p += std::snprintf(nb + p, sizeof(nb) - p, "%s%s", i ? "   " : "", NOTE_NAMES[pc]);
            }
            panel_text(cr, ui, g, nb, L::SET_TITLE_SIZE * 1.2, cx, L::DET_NOTES_Z, 0, INK_WHITE, 1.0);
            if (ui->detect_best >= 0) {
                char rb[48];
                std::snprintf(rb, sizeof rb, "%s%s", ui->detect_exact ? "" : "~ ", TUNINGS[ui->detect_best].name);
                panel_text(cr, ui, g, rb, L::SET_TEXT_SIZE, cx, L::DET_MATCH_Z, 0,
                           ui->detect_exact ? COL_SHARP : INK_WHITE, 1.0);
            }
        }
        const double bw = L::DET_BUTTON_W, gp = L::DET_BUTTON_GAP;
        const double xs[3] = { cx - 1.5 * bw - gp, cx - 0.5 * bw, cx + 0.5 * bw + gp };
        outline_button(cr, ui, g, "APPLY",  xs[0], xs[0] + bw, L::DET_BUTTON_Z, L::SET_CLOSE_H, L::SET_TEXT_SIZE * 0.9,
                       ui->detect_best >= 0);
        outline_button(cr, ui, g, "CLEAR",  xs[1], xs[1] + bw, L::DET_BUTTON_Z, L::SET_CLOSE_H, L::SET_TEXT_SIZE * 0.9,
                       true);
        outline_button(cr, ui, g, "CANCEL", xs[2], xs[2] + bw, L::DET_BUTTON_Z, L::SET_CLOSE_H, L::SET_TEXT_SIZE * 0.9,
                       true);
    }

    if (ui->settings_open) {
        smoked_window(cr, g, L::SETTINGS_WIN);
        panel_text(cr, ui, g, "ADVANCED SETTINGS", L::SET_TITLE_SIZE, L::SET_LABEL_X, L::SET_TITLE_Z, -1, COL_SHARP, 1.0);
        const char* labels[3] = { "Dim Amount", "In-Tune Tolerance", "Sensitivity" };
        const double fr[3] = { (ui->dim_db + 60.0) / 60.0, (ui->tolerance - 0.5) / 9.5, (double)ui->sensitivity };
        char vals[3][24];
        std::snprintf(vals[0], sizeof vals[0], "%.0f dB", ui->dim_db);
        std::snprintf(vals[1], sizeof vals[1], "%.1f \xC2\xA2", ui->tolerance);
        std::snprintf(vals[2], sizeof vals[2], "%.0f %%", ui->sensitivity * 100.0);
        for (int r = 0; r < 3; ++r) {
            const double z = L::SET_ROW_Z[r];
            const double f = std::fmin(1.0, std::fmax(0.0, fr[r]));
            panel_text(cr, ui, g, labels[r], L::SET_TEXT_SIZE, L::SET_LABEL_X, z, -1, INK_WHITE, 1.0);
            const double t0 = L::SET_TRACK_X0, t1 = L::SET_TRACK_X1, tw = L::SET_TRACK_W;
            rrect_path(cr, g.X(t0), g.Y(z + tw / 2), g.S(t1 - t0), g.S(tw), g.S(tw / 2));
            cairo_set_source_rgb(cr, 0.30, 0.30, 0.31);
            cairo_fill(cr);
            const double xf = t0 + f * (t1 - t0);
            rrect_path(cr, g.X(t0), g.Y(z + tw / 2), g.S(xf - t0), g.S(tw), g.S(tw / 2));
            cairo_set_source_rgb(cr, COL_SHARP[0], COL_SHARP[1], COL_SHARP[2]);
            cairo_fill(cr);
            cairo_arc(cr, g.X(xf), g.Y(z), g.S(L::SET_KNOB_R), 0, 2 * M_PI);
            cairo_set_source_rgb(cr, 1.0, 0.80, 0.35);
            cairo_fill(cr);
            panel_text(cr, ui, g, vals[r], L::SET_TEXT_SIZE, L::SET_VALUE_X, z, 1, INK_WHITE, 1.0);
        }
        const double pz = L::SET_PING_Z, px0 = L::SET_PING_X, pw = L::SET_PILL_W, ph = L::SET_PILL_H;
        const bool ping_on = ui->ping_enable > 0.5f;
        panel_text(cr, ui, g, "Ping", L::SET_TEXT_SIZE, L::SET_LABEL_X, pz, -1, INK_WHITE, 1.0);
        rrect_path(cr, g.X(px0), g.Y(pz + ph / 2), g.S(pw), g.S(ph), g.S(ph / 2));
        if (ping_on) cairo_set_source_rgb(cr, COL_SHARP[0], COL_SHARP[1], COL_SHARP[2]);
        else         cairo_set_source_rgb(cr, 0.30, 0.30, 0.31);
        cairo_fill(cr);
        const double kx = ping_on ? px0 + pw - ph / 2 : px0 + ph / 2;
        cairo_arc(cr, g.X(kx), g.Y(pz), g.S(ph * 0.38), 0, 2 * M_PI);
        cairo_set_source_rgb(cr, 0.92, 0.92, 0.92);
        cairo_fill(cr);
        outline_button(cr, ui, g, "CLOSE", L::SET_CLOSE_X0, L::SET_CLOSE_X1, pz, L::SET_CLOSE_H, L::SET_TEXT_SIZE * 0.9,
                       true);
    }

    cairo_pop_group_to_source(cr);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_flush(ui->surface);
}

// Load the photograph, the cut-outs and the type from a directory (the bundle at run time, the repo for the lab
// tools). Anything missing is drawn without: the plain face, no pressed caps, the toy font.
void load_assets(HawkUI* ui, const std::string& dir_in)
{
    std::string dir = dir_in;
    if (!dir.empty() && dir.back() != '/') dir += '/';
    auto png = [&](const char* name) -> cairo_surface_t* {
        cairo_surface_t* s = cairo_image_surface_create_from_png((dir + name).c_str());
        if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) { cairo_surface_destroy(s); return nullptr; }
        return s;
    };
    ui->panel = png("panel.png");
    static_assert(sizeof(L::SPRITES) / sizeof(L::SPRITES[0]) == 7, "four buttons and three hawk states");
    for (int i = 0; i < 7; ++i) ui->sprite[i] = png(L::SPRITES[i].file);
    ui->font = hawk_fonts::load_face(dir + "Oswald.ttf");
}

void free_assets(HawkUI* ui)
{
    if (ui->panel)        cairo_surface_destroy(ui->panel);
    if (ui->static_layer) cairo_surface_destroy(ui->static_layer);
    for (auto& s : ui->sprite) if (s) { cairo_surface_destroy(s); s = nullptr; }
    if (ui->font) cairo_font_face_destroy(ui->font);   // our reference; its library goes when cairo drops the face
    ui->panel = ui->static_layer = nullptr;
    ui->font = nullptr;
}

// ---- LV2 UI entry points -------------------------------------------------

// Xlib's default error handler exit()s the process -- the HOST. Any X error on our own connections (a parent window
// the host destroyed first, a parent not yet known to the server) is swallowed instead; errors on other connections go
// to the handler that was there before. Installed while at least one editor is open, restored with the last one.
// HAWK_UI_NO_X_HANDLER=1 leaves Xlib's default, for the X gate's negative arm only (tools/ui_x_probe.cpp).
#ifndef HAWK_UI_NO_X_HANDLER
#define HAWK_UI_NO_X_HANDLER 0
#endif
#if !HAWK_UI_NO_X_HANDLER
std::mutex       g_x_mutex;
XErrorHandler    g_x_prev = nullptr;
Display*         g_x_ours[16] = {};
int              g_x_users = 0;

int hawk_x_error(Display* d, XErrorEvent* e)
{
    {
        std::lock_guard<std::mutex> lk(g_x_mutex);
        for (Display* o : g_x_ours) if (o == d) return 0;
    }
    return g_x_prev ? g_x_prev(d, e) : 0;
}
#endif

void x_guard_add(Display* d)
{
#if !HAWK_UI_NO_X_HANDLER
    std::lock_guard<std::mutex> lk(g_x_mutex);
    for (Display*& o : g_x_ours) if (!o) { o = d; break; }
    if (g_x_users++ == 0) g_x_prev = XSetErrorHandler(hawk_x_error);
#else
    (void)d;
#endif
}

void x_guard_remove(Display* d)
{
#if !HAWK_UI_NO_X_HANDLER
    std::lock_guard<std::mutex> lk(g_x_mutex);
    for (Display*& o : g_x_ours) if (o == d) o = nullptr;
    if (--g_x_users == 0) {
        const XErrorHandler cur = XSetErrorHandler(g_x_prev);
        if (cur != hawk_x_error) XSetErrorHandler(cur);   // someone installed theirs after ours: leave it
        g_x_prev = nullptr;
    }
#else
    (void)d;
#endif
}

LV2UI_Handle instantiate(const LV2UI_Descriptor*, const char*,
                         const char* bundle_path,
                         LV2UI_Write_Function write_function, LV2UI_Controller controller,
                         LV2UI_Widget* widget, const LV2_Feature* const* features)
{
    Window        parent = 0;
    LV2UI_Resize* host_resize = nullptr;
    const LV2_Options_Option* options = nullptr;
    LV2_URID_Map* map = nullptr;

    for (int i = 0; features && features[i]; ++i) {
        if (!std::strcmp(features[i]->URI, LV2_UI__parent))
            parent = (Window)(uintptr_t)features[i]->data;
        else if (!std::strcmp(features[i]->URI, LV2_UI__resize))
            host_resize = (LV2UI_Resize*)features[i]->data;
        else if (!std::strcmp(features[i]->URI, LV2_OPTIONS__options))
            options = (const LV2_Options_Option*)features[i]->data;
        else if (!std::strcmp(features[i]->URI, LV2_URID__map))
            map = (LV2_URID_Map*)features[i]->data;
    }
    // The host's GUI scale (ui:scaleFactor, e.g. Ardour's interface scale): the editor opens that much larger. All
    // drawing follows the window's size, so nothing else depends on it.
    double scale = 1.0;
    if (options && map) {
        const LV2_URID key = map->map(map->handle, LV2_UI__scaleFactor);
        const LV2_URID tfloat = map->map(map->handle, LV2_ATOM__Float);
        for (const LV2_Options_Option* o = options; o->key; ++o)
            if (o->key == key && o->type == tfloat && o->size == sizeof(float) && o->value) {
                const float f = *(const float*)o->value;
                if (std::isfinite(f) && f >= 0.5f && f <= 4.0f) scale = f;
            }
    }
    if (!parent) return nullptr;  // X11 embedding needs a parent window

    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) return nullptr;
    x_guard_add(dpy);

    HawkUI* ui     = new HawkUI();
    ui->dpy        = dpy;
    ui->screen     = DefaultScreen(dpy);
    ui->host_resize= host_resize;
    ui->write      = write_function;
    ui->controller = controller;
    ui->width      = (int)std::lround(ui->width * scale);
    ui->height     = (int)std::lround(ui->height * scale);

    // Match the parent's visual/depth so Cairo renders correctly under any host.
    XWindowAttributes pattr;
    if (!XGetWindowAttributes(dpy, parent, &pattr)) {   // a parent the server does not know: no editor
        x_guard_remove(dpy);
        XCloseDisplay(dpy);
        delete ui;
        return nullptr;
    }
    ui->visual = pattr.visual;
    const int depth = pattr.depth;

    ui->cmap = XCreateColormap(dpy, parent, ui->visual, AllocNone);

    XSetWindowAttributes swa;
    swa.background_pixel = BlackPixel(dpy, ui->screen);
    swa.border_pixel     = 0;
    swa.colormap         = ui->cmap;
    swa.event_mask       = ExposureMask | StructureNotifyMask | ButtonPressMask
                         | ButtonReleaseMask | Button1MotionMask;

    ui->win = XCreateWindow(dpy, parent, 0, 0, ui->width, ui->height, 0,
                            depth, InputOutput, ui->visual,
                            CWBackPixel | CWBorderPixel | CWColormap | CWEventMask,
                            &swa);
    XMapWindow(dpy, ui->win);
    XFlush(dpy);

    ui->surface = cairo_xlib_surface_create(dpy, ui->win, ui->visual,
                                            ui->width, ui->height);

    // The photograph, its cut-outs and the type travel in the bundle.
    if (bundle_path) load_assets(ui, bundle_path);

    if (host_resize && host_resize->ui_resize)
        host_resize->ui_resize(host_resize->handle, ui->width, ui->height);

    *widget = (LV2UI_Widget)(uintptr_t)ui->win;
    return ui;
}

void cleanup(LV2UI_Handle handle)
{
    HawkUI* ui = (HawkUI*)handle;
    free_assets(ui);
    if (ui->surface)   cairo_surface_destroy(ui->surface);
    if (ui->win)     XDestroyWindow(ui->dpy, ui->win);
    if (ui->cmap)    XFreeColormap(ui->dpy, ui->cmap);
    if (ui->dpy) {
        XSync(ui->dpy, False);                  // errors of our last requests still reach our handler
        x_guard_remove(ui->dpy);
        XCloseDisplay(ui->dpy);
    }
    delete ui;
}

void port_event(LV2UI_Handle handle, uint32_t port, uint32_t bufsize,
                uint32_t format, const void* buffer)
{
    if (format != 0 || bufsize != sizeof(float)) return;  // plain control port
    HawkUI* ui = (HawkUI*)handle;
    float v = *(const float*)buffer;
#ifndef HAWK_UI_TRUST_PORTS
    // A NaN or an infinity from a host or an automation lane keeps the last sane value (one NaN phase would leave the
    // strobe disc NaN for the whole note, a NaN level drew the meter full); displayed values stay in the TTL's ranges.
    // HAWK_UI_TRUST_PORTS=1 drops this, for the ports gate's negative arm only (tools/ui_ports_probe.cpp).
    if (!std::isfinite(v)) return;
    auto clampf = [](float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; };
    switch (port) {
        case PORT_DIM_DB:      v = clampf(v, -60.0f, 0.0f);  break;
        case PORT_TOLERANCE:   v = clampf(v, 0.5f, 10.0f);   break;
        case PORT_A4:          v = clampf(v, (float)A4_MIN, (float)A4_MAX); break;
        case PORT_LEVEL:       v = clampf(v, 0.0f, 1.0f);    break;
        case PORT_SENSITIVITY: v = clampf(v, 0.0f, 1.0f);    break;
        default: break;
    }
#endif
    switch (port) {
        case PORT_ACTIVE:  ui->mute    = v; break;
        case PORT_DIM_DB:    ui->dim_db    = v; break;
        case PORT_TOLERANCE: ui->tolerance = v; break;
        case PORT_REF_NOTE:  ref_from_port(ui, v); break;
        case PORT_PING_ENABLE: ui->ping_enable = v; break;
        case PORT_NOTE:    ui->note    = v; break;
        case PORT_CENTS:   ui->cents   = v; break;
        case PORT_VOICED:  ui->voiced  = v; break;
        case PORT_IN_TUNE: ui->in_tune = v; break;
        case PORT_A4:      ui->a4_ref  = v; break;
        case PORT_TUNING:  // clamp a stray host value to the valid enum range
            ui->tuning = (v < 0.0f) ? 0.0f
                       : (v > (float)(TUNING_COUNT - 1)) ? (float)(TUNING_COUNT - 1) : v;
            break;
        case PORT_REF_ACTIVE: ui->ref_active = v; break;
        case PORT_LEVEL:       ui->level       = v; break;
        case PORT_SENSITIVITY: ui->sensitivity = v; break;
        case PORT_STROBE:      ui->strobe_phase = v; break;
        case PORT_STROBE_MODE: ui->strobe_mode  = v; break;
        case PORT_ENABLED:     ui->enabled      = v; break;
        default: break;
    }
    ui->dirty = true;    // the redraw happens on the next idle tick
}

// Set the currently-dragged Advanced-Settings slider from a content-space x, and
// write the matching port. Shared by the initial click and the drag motion.
void set_slider(HawkUI* ui, double mx)
{
    if (ui->drag < 0 || ui->drag > 2) return;
    const Rect& r = ui->lay.slider[ui->drag];
    double f = (mx - r.x) / r.w;
    f = f < 0 ? 0 : (f > 1 ? 1 : f);
    if (ui->drag == 0) {                                    // Dim Amount (-60..0 dB)
        ui->dim_db = (float)(-60.0 + f * 60.0);
        if (ui->write) ui->write(ui->controller, PORT_DIM_DB, sizeof(float), 0, &ui->dim_db);
    } else if (ui->drag == 1) {                             // Tolerance (0.5..10 c)
        ui->tolerance = (float)(0.5 + f * 9.5);
        if (ui->write) ui->write(ui->controller, PORT_TOLERANCE, sizeof(float), 0, &ui->tolerance);
    } else {                                                // Sensitivity (0..1)
        ui->sensitivity = (float)f;
        if (ui->write) ui->write(ui->controller, PORT_SENSITIVITY, sizeof(float), 0, &ui->sensitivity);
    }
}

// Bring the layout up to date with the window's size and the state, measuring on the surface that is drawn.
// HAWK_UI_NO_LAYOUT_ON_PRESS=1 skips it -- a click is then tested against the last frame's layout, as when the hit
// rectangles were set by draw() -- for the click gate's negative arm only (tools/ui_click_probe.cpp).
#ifndef HAWK_UI_NO_LAYOUT_ON_PRESS
#define HAWK_UI_NO_LAYOUT_ON_PRESS 0
#endif
void refresh_layout(HawkUI* ui)
{
#if !HAWK_UI_NO_LAYOUT_ON_PRESS
    cairo_surface_t* s = ui->surface ? ui->surface : cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t* cr = cairo_create(s);
    layout(ui, cr);
    cairo_destroy(cr);
    if (!ui->surface) cairo_surface_destroy(s);
#else
    (void)ui;
#endif
}

// A mouse button pressed at window pixel (x, y). The overlays take the click first (Detect, Settings, a dropdown);
// otherwise the panel's controls.
void handle_press(HawkUI* ui, double x, double y, unsigned button)
{
    refresh_layout(ui);
    const Layout& l = ui->lay;
    const double mx = x - l.ox, my = y - l.oy;              // into the letterboxed content box
    auto toggle = [&](float& v, uint32_t port) {
        v = (v > 0.5f) ? 0.0f : 1.0f;
        if (ui->write) ui->write(ui->controller, port, sizeof(float), 0, &v);
    };
    if (ui->detect_mode) {
        if (button != Button1) return;
        if (l.det_apply.contains(mx, my) && ui->detect_best >= 0) {
            ui->tuning = (float)ui->detect_best;
            if (ui->write) ui->write(ui->controller, PORT_TUNING, sizeof(float), 0, &ui->tuning);
            ui->detect_mode = false;
        } else if (l.det_clear.contains(mx, my)) {
            ui->detect_count = 0;
            ui->detect_best  = -1;
            ui->detect_exact = false;
            ui->detect_pluck_done = false;
        } else if (l.det_cancel.contains(mx, my)) {
            ui->detect_mode = false;
        }
        return;
    }
    if (ui->settings_open) {
        if (button != Button1) return;
        for (int r = 0; r < 3; ++r)
            if (l.slider[r].contains(mx, my)) { ui->drag = r; set_slider(ui, mx); return; }
        if (l.ping.contains(mx, my))          toggle(ui->ping_enable, PORT_PING_ENABLE);
        else if (l.set_close.contains(mx, my)) ui->settings_open = false;
        else if (!l.set_panel.contains(mx, my)) ui->settings_open = false;   // click outside (SETTINGS too) closes
        return;
    }
    if (ui->dd_open) {
        if (button == Button4) { ui->dd_scroll -= 1; return; }               // wheel up
        if (button == Button5) { ui->dd_scroll += 1; return; }               // wheel down
        if (button != Button1) return;
        bool close = true;                                                   // click-away closes by default
        if (l.dd_panel.contains(mx, my)) {
            const int idx = ui->dd_scroll + (int)((my - l.dd_panel.y) / l.dd_item_h);
            if (ui->dd_which == 1) {                                         // tuning menu: headings or tunings
                ensure_disp();
                if (idx >= 0 && idx < g_disp_n && g_disp[idx].kind == 2) {
                    ui->tuning = (float)g_disp[idx].tuning;
                    if (ui->write) ui->write(ui->controller, PORT_TUNING, sizeof(float), 0, &ui->tuning);
                } else if (idx >= 0 && idx < g_disp_n && g_disp[idx].kind == 3) {
                    ui->detect_mode  = true;                                 // enter auto-detect
                    ui->detect_count = 0;
                    ui->detect_best  = -1;
                    ui->detect_exact = false;
                    ui->detect_pluck_done = false;
                } else {
                    close = false;                                           // a heading (or a gap): stay open
                }
            } else if (idx >= 0 && idx < A4_COUNT) {
                ui->a4_ref = (float)(A4_MIN + idx);
                if (ui->write) ui->write(ui->controller, PORT_A4, sizeof(float), 0, &ui->a4_ref);
            }
        }
        if (close) ui->dd_open = false;
        return;
    }
    if (button != Button1) return;
    const double ldx = mx - l.logo_cx, ldy = my - l.logo_cy;
    if (l.a4_menu.contains(mx, my)) {
        ui->dd_open   = true;                                                // the A4 dropdown
        ui->dd_which  = 0;
        ui->dd_scroll = (int)std::lround(ui->a4_ref) - A4_MIN - DD_VISIBLE / 2;
    } else if (l.row.contains(mx, my)) {
        ui->dd_open   = true;                                                // the tuning dropdown
        ui->dd_which  = 1;
        ui->dd_scroll = disp_row_for_tuning((int)std::lround(ui->tuning)) - DD_VISIBLE / 2;
    } else if (ldx * ldx + ldy * ldy <= l.logo_r * l.logo_r || l.name.contains(mx, my)) {
        // the hawk and the name are the bypass: lv2:enabled, so the host's bypass and these are one switch
        toggle(ui->enabled, PORT_ENABLED);
    } else if (l.button[3].contains(mx, my)) {
        ui->settings_open = true;                                            // SETTINGS
    } else if (l.button[0].contains(mx, my)) {
        toggle(ui->mute, PORT_ACTIVE);                                       // MUTE
    } else if (l.button[2].contains(mx, my)) {
        toggle(ui->strobe_mode, PORT_STROBE_MODE);                           // STROBE
    } else if (l.button[1].contains(mx, my)) {
        // TONE. On engaging, start on the string nearest whatever you were last playing.
        ui->ref_active = (ui->ref_active > 0.5f) ? 0.0f : 1.0f;
        if (ui->ref_active > 0.5f) {
            int tgt[8];
            const int n = ref_build_targets(ui, tgt);
            ui->ref_string = (ui->note >= 0.0f) ? ref_nearest_string(tgt, n, (int)std::lround(ui->note)) : 0;
            ref_apply_note(ui);
        }
        if (ui->write) ui->write(ui->controller, PORT_REF_ACTIVE, sizeof(float), 0, &ui->ref_active);
    } else if (ui->ref_active > 0.5f && l.ref_tag.contains(mx, my)) {
        ui->ref_octave = (ui->ref_octave + 1) % 3;                           // REF: cycle the octave 0 -> +1 -> +2
        ref_apply_note(ui);
    } else if (ui->ref_active > 0.5f && l.note.contains(mx, my)) {
        int tgt[8];                                                          // the note screen: cycle the strings
        const int n = ref_build_targets(ui, tgt);
        ui->ref_string = (n > 0) ? (ui->ref_string + 1) % n : 0;
        ref_apply_note(ui);
    }
}

int ui_idle(LV2UI_Handle handle)
{
    HawkUI* ui = (HawkUI*)handle;
    if (ui->dpy) {
        while (XPending(ui->dpy)) {
            XEvent ev;
            XNextEvent(ui->dpy, &ev);
            if (ev.type == ConfigureNotify) {
                ui->width  = ev.xconfigure.width;
                ui->height = ev.xconfigure.height;
                if (ui->surface)
                    cairo_xlib_surface_set_size(ui->surface, ui->width, ui->height);
            } else if (ev.type == ButtonPress) {
                handle_press(ui, ev.xbutton.x, ev.xbutton.y, ev.xbutton.button);
            } else if (ev.type == MotionNotify) {
                if (ui->settings_open && ui->drag >= 0)
                    set_slider(ui, ev.xmotion.x - ui->lay.ox);
            } else if (ev.type == ButtonRelease) {
                ui->drag = -1;                         // end any slider drag
            }
            ui->dirty = true;                          // Expose, a resize, a click, a drag
        }
    }
    update(ui);
    // What moves on its own: the strobe disc, the chord-nailed blink, the complete hold running out, the reference
    // tone's follow dwell.
    bool moving = (ui->strobe_mode > 0.5f && ui->strobe_init) || ui->complete || ui->ref_cand >= 0;
    const auto now = std::chrono::steady_clock::now();
    if ((ui->dirty || moving) && now - ui->last_draw >= std::chrono::milliseconds(33)) {
        ui->dirty = false;
        ui->last_draw = now;
        draw(ui);
    }
    return 0;
}

const void* extension_data(const char* uri)
{
    static const LV2UI_Idle_Interface idle = { ui_idle };
    if (!std::strcmp(uri, LV2_UI__idleInterface)) return &idle;
    return nullptr;
}

const LV2UI_Descriptor descriptor = {
    HAWK_UI_URI,
    instantiate,
    cleanup,
    port_event,
    extension_data
};

}  // namespace

extern "C" LV2_SYMBOL_EXPORT
const LV2UI_Descriptor* lv2ui_descriptor(uint32_t index)
{
    return (index == 0) ? &descriptor : nullptr;
}
