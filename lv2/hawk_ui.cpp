// The Hawk — LV2 X11 + Cairo GUI.
//
// A self-contained, framework-free tuner display. The host (e.g. Ardour) embeds
// our X11 window in its plugin slot and drives us through the LV2 idle
// interface; we never run our own event loop. We only *read* the plugin's
// output control ports (note, cents, the three tuning lamps) and draw them.
//
// Visual: a "hardware unit" look — a dark brushed-metal faceplate with a
// recessed display screen inset into it. Inside the screen: the note name
// boxed on the left, a horizontal converging-chevron centre-zero meter in the
// middle, and the three lamp dots on the right, all sharing one centre axis.

#include <lv2/core/lv2.h>
#include <lv2/ui/ui.h>

#include <X11/Xlib.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "tunings.hpp"   // shared tuning table (Tuning, TUNINGS, build_targets) —
                         // single source of truth with the DSP (src/tunings.hpp)

#define HAWK_UI_URI "https://themonohawk.audio/lv2/tuner#ui"

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
DispRow g_disp[64];
int     g_disp_n = -1;     // lazily built; UI thread only

void ensure_disp() {
    if (g_disp_n >= 0) return;
    int n = 0;
    g_disp[n++] = { 3, "Detect Tuning...", -1, 0 };  // action: auto-detect
    for (int fam = 0; fam < 2; ++fam) {              // 0 = Guitar, 1 = Bass
        g_disp[n++] = { 0, fam ? "BASS" : "GUITAR", -1, 0 };
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
const double COL_LED  [3] = { 0.80, 0.90, 1.00 };  // note LED (cool white)

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

struct HawkUI {
    Display*         dpy     = nullptr;
    Window           win     = 0;
    Colormap         cmap    = 0;
    Visual*          visual  = nullptr;
    int              screen  = 0;
    cairo_surface_t* surface   = nullptr;
    cairo_surface_t* logo      = nullptr;  // logo artwork (PNG from the bundle)
    int              width     = 600;
    int              height  = 160;
    double           ox = 0, oy = 0;   // letterbox offset (content box origin)

    LV2UI_Resize*        host_resize = nullptr;
    LV2UI_Write_Function write       = nullptr;
    LV2UI_Controller     controller  = nullptr;

    // Mute button: state + hit-rectangle (refreshed each frame in draw()).
    float  mute = 0.0f;
    double mute_bx = 0, mute_by = 0, mute_bw = 0, mute_bh = 0;

    // Reference-tone (pitch-pipe) button: state + hit-rectangle. When active, the
    // plugin drones a sine at the selected string's pitch; the note box shows
    // which string, and clicking the note box cycles through the tuning's strings.
    float  ref_active = 0.0f;       // UI source of truth (mirrors PORT_REF_ACTIVE)
    int    ref_string = 0;          // selected string index within the current tuning
    int    ref_octave = 0;          // reference-tone octave shift (0/+1/+2 for audibility)
    int    ref_cand   = -1;         // candidate string awaiting the follow debounce
    std::chrono::steady_clock::time_point ref_cand_since;
    double tone_bx = 0, tone_by = 0, tone_bw = 0, tone_bh = 0;
    double nbox_bx = 0, nbox_by = 0, nbox_bw = 0, nbox_bh = 0;  // note-box hit-rect
    double oct_bx = 0, oct_by = 0, oct_bw = 0, oct_bh = 0;      // "8va" cycle hit-rect

    // Input-level meter + sensitivity (Advanced Settings).
    float  level       = 0.0f;      // mirrors PORT_LEVEL (input signal level)
    float  sensitivity = 0.5f;      // mirrors PORT_SENSITIVITY
    double set_sens_bx = 0, set_sens_by = 0, set_sens_bw = 0, set_sens_bh = 0;

    // Dropdown menu (shared by the A4 and tuning selectors — only one open at a
    // time). dd_which picks the list: 0 = A4, 1 = tuning.
    bool   dd_open   = false;
    int    dd_which  = 0;
    int    dd_scroll = 0;
    double freq_bx = 0, freq_by = 0, freq_bw = 0, freq_bh = 0;          // A4 trigger
    double row_bx = 0, row_by = 0, row_bw = 0, row_bh = 0;              // tuning-row trigger
    double dd_px = 0, dd_py = 0, dd_pw = 0, dd_ph = 0, dd_item_h = 0;   // panel

    // Latest values pushed from the plugin's output ports.
    float note    = -1.0f;  // MIDI note number; <0 means "none yet"
    float cents   = 0.0f;
    float a4_ref  = 440.0f; // tuning reference (A4), mirrors the a4 control port
    float voiced  = 0.0f;
    float in_tune = 0.0f;   // the only tuning lamp the faceplate uses (flat/sharp
                            // are derived from the sign of `cents`, so the plugin's
                            // raise/lower lamp ports are not read here)
    float tuning  = 0.0f;   // selected tuning index (into TUNINGS)

    // Per-string tuning progress: each target latches "lit" when hit in tune,
    // and stays lit. When all are lit the tuning is complete (row + logo glow
    // amber for ~3 s, then everything clears so the next instrument can tune).
    int   tgt_midi[8] = {0};
    bool  tgt_lit[8]  = {false};
    int   tgt_count   = 0;
    int   tgt_for     = -1;     // which tuning index tgt_* were built for
    bool  prev_lock   = false;  // previous in-tune state (for rising-edge latch)
    bool  complete    = false;
    std::chrono::steady_clock::time_point complete_at;

    // Advanced Settings pop-up (opened by clicking the logo): live-edit the two
    // settings that have no faceplate control. dim_db/tolerance mirror ports 3/9.
    bool   settings_open = false;
    float  dim_db    = -12.0f;
    float  tolerance = 2.0f;
    float  ping_enable = 0.0f;       // in-tune ping on/off (mirrors PORT_PING_ENABLE; off by default)
    double logo_cx = 0, logo_cy = 0, logo_r = 0;        // logo hit-test (content space)
    double set_dim_bx = 0, set_dim_by = 0, set_dim_bw = 0, set_dim_bh = 0;
    double set_tol_bx = 0, set_tol_by = 0, set_tol_bw = 0, set_tol_bh = 0;
    double set_close_bx = 0, set_close_by = 0, set_close_bw = 0, set_close_bh = 0;
    double set_ping_bx = 0, set_ping_by = 0, set_ping_bw = 0, set_ping_bh = 0;
    double set_panel_bx = 0, set_panel_by = 0, set_panel_bw = 0, set_panel_bh = 0;
    int    drag = -1;   // slider being dragged: -1 none, 0 Dim Amount, 1 Tolerance

    // Auto-detect tuning ("Detect Tuning" from the menu): play the open strings
    // one at a time; it collects each settled pitch and matches the set against
    // the tuning table. One string at a time — single-note detection only.
    bool  detect_mode      = false;
    int   detect_notes[8]  = {0};
    int   detect_count     = 0;
    bool  detect_prev_voiced = false;
    bool  detect_pluck_done  = false;   // already captured the current pluck?
    int   detect_cap_note  = -1;        // pitch being stabilised this pluck
    std::chrono::steady_clock::time_point detect_cap_start;
    int   detect_best      = -1;        // best-matching tuning index
    bool  detect_exact     = false;     // the full set matched a tuning exactly
    double det_apply_bx = 0, det_apply_by = 0, det_apply_bw = 0, det_apply_bh = 0;
    double det_clear_bx = 0, det_clear_by = 0, det_clear_bw = 0, det_clear_bh = 0;
    double det_cancel_bx = 0, det_cancel_by = 0, det_cancel_bw = 0, det_cancel_bh = 0;
};

// Match the collected open-string pitches against the tuning table. Picks the
// tuning with the smallest total mismatch; ties break toward the one whose
// string count equals what's been played (a complete, exact set).
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
        const long rank = score * 1000 + (TUNINGS[t].count - ui->detect_count);
        if (rank < best_rank) { best_rank = rank; ui->detect_best = t; best_score = score; }
    }
    ui->detect_exact = (ui->detect_best >= 0 && best_score == 0
                        && TUNINGS[ui->detect_best].count == ui->detect_count);
}

// Collect open-string pitches while in detect mode: on each pluck, once the
// detected note holds steady for ~250 ms, commit it (if new) and re-match.
void update_detect(HawkUI* ui, bool voiced)
{
    if (!ui->detect_mode) { ui->detect_prev_voiced = voiced; return; }
    const int m = (int)std::lround(ui->note);
    if (voiced && !ui->detect_prev_voiced) {            // new pluck onset
        ui->detect_pluck_done = false;
        ui->detect_cap_note   = m;
        ui->detect_cap_start  = std::chrono::steady_clock::now();
    } else if (voiced && !ui->detect_pluck_done) {
        if (m != ui->detect_cap_note) {                 // pitch shifted -> restart dwell
            ui->detect_cap_note  = m;
            ui->detect_cap_start = std::chrono::steady_clock::now();
        } else if (std::chrono::steady_clock::now() - ui->detect_cap_start
                   > std::chrono::milliseconds(250)) {
            bool dup = false;
            for (int i = 0; i < ui->detect_count; ++i)
                if (ui->detect_notes[i] == m) dup = true;
            if (!dup && ui->detect_count < 8) ui->detect_notes[ui->detect_count++] = m;
            ui->detect_pluck_done = true;
            match_detect(ui);
        }
    }
    ui->detect_prev_voiced = voiced;
}

// Advance the per-string tuning progress. Rebuilds the target table when the
// tuning changes, latches a target "lit" on each fresh in-tune lock, marks the
// set complete when all are lit, and clears everything ~3 s after completion.
void update_tuning_progress(HawkUI* ui, bool voiced, bool intune)
{
    const int t = (int)std::lround(ui->tuning);
    if (t != ui->tgt_for) {                 // tuning changed -> reset progress
        ui->tgt_for   = t;
        ui->tgt_count = build_targets(t, ui->tgt_midi);
        for (int i = 0; i < 8; ++i) ui->tgt_lit[i] = false;
        ui->complete  = false;
        ui->prev_lock = false;
    }

    if (ui->complete) {
        const auto elapsed = std::chrono::steady_clock::now() - ui->complete_at;
        if (elapsed > std::chrono::seconds(5)) {     // hold done, then clear
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
// Layout map (everything below is drawn into a fixed-aspect "content box" of
// width W x height H, after letterboxing — see the top of draw()). Positions
// and sizes are expressed as fractions of the screen rectangle (sx,sy,sw,sh),
// which is the content box minus the faceplate bezel. Most fractions are
// hand-tuned by eye; the structurally important ones are called out below.
//
//   +----------------------------------------------------------------+  faceplate
//   |  +----------------------------------------------------------+  |  (brushed
//   |  | [NOTE]        > > >  <zero>  < < <         (  LOGO  )    |  |   metal)
//   |  |  +0.0c         chevron meter, centred on cy              |  |
//   |  |               E  A  D  G  B  E   <- string-target row    |  |  recessed
//   |  |  440 Hz v        Standard v              MUTE            |  |   screen
//   |  +----------------------------------------------------------+  |
//   +----------------------------------------------------------------+
//
// Shared geometry (computed once, near the top of the content section):
//   cy   = sy + sh*0.42   -- the common horizontal centre axis. The note box,
//                            meter and logo all centre on it. Load-bearing:
//                            move it and all three regions move together.
//   NCH  = 7              -- chevrons per side of the meter.
//   mcx  = meter centre x; mhalf = half the meter's usable width. The meter is
//                            centred in the gap between the note box's right edge
//                            and the logo's left edge, so changing the note/logo
//                            sizes re-centres the meter automatically.
// Overlays (drawn last, only when active): the A4/tuning dropdown, the
// auto-detect panel, and the Advanced Settings panel.

void draw(HawkUI* ui)
{
    if (!ui->surface) return;

    cairo_t* cr = cairo_create(ui->surface);
    cairo_push_group(cr);            // double-buffer this frame (kills flicker)

    // Letterbox to the design aspect so the faceplate scales cleanly at any
    // window size or shape instead of stretching. Everything below draws into a
    // fixed-aspect content box, centred, with black bars filling any remainder.
    // (The whole UI is vector/proportional, so this is all that scaling needs.)
    const double aw = ui->width, ah = ui->height;
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_rectangle(cr, 0, 0, aw, ah);
    cairo_fill(cr);
    constexpr double ASPECT = 600.0 / 160.0;
    double W = aw, H = ah;
    if (aw / ah > ASPECT) W = ah * ASPECT;   // window too wide -> pillarbox
    else                  H = aw / ASPECT;   // window too tall -> letterbox
    ui->ox = (aw - W) * 0.5;
    ui->oy = (ah - H) * 0.5;
    cairo_translate(cr, ui->ox, ui->oy);

    const bool voiced = ui->voiced  > 0.5f;
    const bool intune = ui->in_tune > 0.5f;

    update_tuning_progress(ui, voiced, intune);
    update_detect(ui, voiced);
    update_ref_follow(ui);
    const bool complete = ui->complete;

    // --- Small helpers ---
    auto rrect = [&](double x, double y, double w, double h, double r) {
        cairo_new_sub_path(cr);
        cairo_arc(cr, x + w - r, y + r,     r, -M_PI / 2, 0);
        cairo_arc(cr, x + w - r, y + h - r, r, 0,          M_PI / 2);
        cairo_arc(cr, x + r,     y + h - r, r,  M_PI / 2,  M_PI);
        cairo_arc(cr, x + r,     y + r,     r,  M_PI,      3 * M_PI / 2);
        cairo_close_path(cr);
    };

    // ============================================================
    //  Faceplate (the hardware body)
    // ============================================================
    {
        // Brushed-metal faceplate: a vertical sheen with a highlight band.
        cairo_pattern_t* body = cairo_pattern_create_linear(0, 0, 0, H);
        cairo_pattern_add_color_stop_rgb(body, 0.00, 0.16, 0.16, 0.18);
        cairo_pattern_add_color_stop_rgb(body, 0.28, 0.28, 0.29, 0.31);  // sheen
        cairo_pattern_add_color_stop_rgb(body, 0.55, 0.15, 0.15, 0.17);
        cairo_pattern_add_color_stop_rgb(body, 1.00, 0.09, 0.09, 0.10);
        cairo_set_source(cr, body);
        cairo_paint(cr);
        cairo_pattern_destroy(body);
        // Fine horizontal brush striations.
        cairo_set_line_width(cr, 1.0);
        for (double yy = 1.5; yy < H; yy += 3.0) {
            cairo_set_source_rgba(cr, 1, 1, 1, 0.020);
            cairo_move_to(cr, 0, yy);
            cairo_line_to(cr, W, yy);
            cairo_stroke(cr);
        }
        // Top highlight edge.
        cairo_set_source_rgba(cr, 1, 1, 1, 0.12);
        cairo_set_line_width(cr, 1.0);
        cairo_move_to(cr, 0, 1.0);
        cairo_line_to(cr, W, 1.0);
        cairo_stroke(cr);
    }

    const double bezel = std::fmin(H * 0.06, W * 0.03);

    // ============================================================
    //  Recessed screen
    // ============================================================
    const double sx = bezel, sy = bezel;
    const double sw = W - 2 * bezel, sh = H - 2 * bezel;
    const double srad = std::fmin(sh, sw) * 0.06;
    {
        // Dark glass.
        rrect(sx, sy, sw, sh, srad);
        cairo_pattern_t* glass = cairo_pattern_create_linear(0, sy, 0, sy + sh);
        cairo_pattern_add_color_stop_rgb(glass, 0.0, 0.020, 0.020, 0.026);
        cairo_pattern_add_color_stop_rgb(glass, 1.0, 0.004, 0.004, 0.007);
        cairo_set_source(cr, glass);
        cairo_fill_preserve(cr);
        cairo_pattern_destroy(glass);
        // Recessed border: dark outer, faint inner top highlight.
        cairo_set_source_rgba(cr, 0, 0, 0, 0.8);
        cairo_set_line_width(cr, 2.0);
        cairo_stroke(cr);
        rrect(sx + 1.0, sy + 1.0, sw - 2.0, sh - 2.0, srad);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.05);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);
    }

    // ---------------- Input-level meter (thin strip, top of screen) ----------
    // Shows the tuner is receiving signal ("is it hearing me?"). Dim track +
    // a fill proportional to input level; amber near clipping.
    {
        const double mlx = sx + sw * 0.06, mrx = sx + sw * 0.94;
        const double mty = sy + sh * 0.05, mth = sh * 0.028;
        rrect(mlx, mty, mrx - mlx, mth, mth * 0.5);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.08);                 // recessed track
        cairo_fill(cr);
        const double lv = std::sqrt(std::fmax(0.0, std::fmin(1.0, (double)ui->level)));
        if (lv > 0.001) {
            rrect(mlx, mty, (mrx - mlx) * lv, mth, mth * 0.5);
            const double* lc = (ui->level > 0.9f) ? COL_SHARP : COL_LED;  // amber near clip
            cairo_set_source_rgba(cr, lc[0], lc[1], lc[2], 0.85);
            cairo_fill(cr);
        }
    }

    // Default text font for the cents readout.
    cairo_select_font_face(cr, "sans-serif",
                           CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);

    // ============================================================
    //  Content layout — one shared centre axis (cy)
    // ============================================================
    const double pad     = sw * 0.035;
    const double note_w  = sw * 0.15;
    const double lamp_w  = sw * 0.19;
    const double cy      = sy + sh * 0.42;     // the common centre line
    const double RANGE   = 50.0;

    // Logo badge geometry (right slot) — computed up front so the meter can be
    // spaced symmetrically against it.
    const double lcx = sx + sw - pad - lamp_w * 0.5;
    const double R   = std::fmin(sh * 0.276, lamp_w * 0.437);  // meter-spacing radius
    const double Rl  = R * 0.855;  // logo drawn ~14.5% smaller, same centre (lcx, cy)
    ui->logo_cx = lcx; ui->logo_cy = cy; ui->logo_r = Rl;   // clickable trigger

    // The meter sits centred between the note box's right edge and the logo's
    // left edge, with equal gaps on both sides.
    const double note_r = sx + pad + note_w;   // note box right edge
    const double logo_l = lcx - R;             // logo left edge
    const double gap    = sw * 0.04;           // the shared left/right gap
    const double mcx    = (note_r + logo_l) * 0.5;
    const double mhalf  = (logo_l - note_r) * 0.5 - gap;

    cairo_text_extents_t te;

    // ---------------- Note box: an inset LED display (centred on cy) ----------
    const double nbh = sh * 0.62;
    const double nbx = sx + pad;
    const double nby = cy - nbh * 0.5;
    // Recessed near-black LED window.
    rrect(nbx, nby, note_w, nbh, nbh * 0.16);
    cairo_set_source_rgb(cr, 0.004, 0.004, 0.008);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.85);
    cairo_set_line_width(cr, 2.0);
    cairo_stroke(cr);
    rrect(nbx + 1.0, nby + 1.0, note_w - 2.0, nbh - 2.0, nbh * 0.15);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.05);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    const double ncx = nbx + note_w * 0.5;
    const double* led = intune ? COL_INTUNE : COL_LED;
    ui->nbox_bx = nbx;    ui->nbox_by = nby;     // note-box hit-rect (clicking it
    ui->nbox_bw = note_w; ui->nbox_bh = nbh;     // cycles strings in reference mode)

    // Draw a 14-segment glyph in cell (x,y,w,h): lit segments bright, unlit
    // faintly visible (the classic LED look). 14 segments render every
    // uppercase letter cleanly. Bits A,B,C,D,E,F,G1,G2,H,I,J,K,L,M = 0..13.
    auto seg14 = [&](double x, double y, double w, double h, uint16_t mask,
                     const double col[3]) {
        const double t  = std::fmax(2.0, w * 0.13);
        const double gp = t * 0.6;
        const double Lx = x + t * 0.5, Cx = x + w * 0.5, Rx = x + w - t * 0.5;
        const double Ty = y + t * 0.5, My = y + h * 0.5, By = y + h - t * 0.5;
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_width(cr, t);
        auto seg = [&](int bit, double x1, double y1, double x2, double y2) {
            cairo_set_source_rgba(cr, col[0], col[1], col[2],
                                  ((mask >> bit) & 1u) ? 1.0 : 0.055);
            cairo_move_to(cr, x1, y1);
            cairo_line_to(cr, x2, y2);
            cairo_stroke(cr);
        };
        seg(0,  Lx + gp, Ty,      Rx - gp, Ty);        // A  top
        seg(1,  Rx,      Ty + gp, Rx,      My - gp);   // B  upper-right
        seg(2,  Rx,      My + gp, Rx,      By - gp);   // C  lower-right
        seg(3,  Lx + gp, By,      Rx - gp, By);        // D  bottom
        seg(4,  Lx,      My + gp, Lx,      By - gp);   // E  lower-left
        seg(5,  Lx,      Ty + gp, Lx,      My - gp);   // F  upper-left
        seg(6,  Lx + gp, My,      Cx - gp, My);        // G1 middle-left
        seg(7,  Cx + gp, My,      Rx - gp, My);        // G2 middle-right
        seg(8,  Lx + gp, Ty + gp, Cx - gp, My - gp);   // H  NW diagonal
        seg(9,  Cx,      Ty + gp, Cx,      My - gp);   // I  upper-centre vert
        seg(10, Rx - gp, Ty + gp, Cx + gp, My - gp);   // J  NE diagonal
        seg(11, Lx + gp, By - gp, Cx - gp, My + gp);   // K  SW diagonal
        seg(12, Cx,      My + gp, Cx,      By - gp);   // L  lower-centre vert
        seg(13, Rx - gp, By - gp, Cx + gp, My + gp);   // M  SE diagonal
    };

    // Draw the big note glyph (letter + sharp mark + octave digit) for a MIDI
    // note, centred in the note box, in colour `col`. Returns the y of the glyph
    // bottom so callers can place text (cents, or "REF") a fixed gap below it.
    auto draw_note_glyph = [&](int midi, const double col[3]) -> double {
        const int pc    = ((midi % 12) + 12) % 12;
        const int oct   = midi / 12 - 1;
        const char* name = NOTE_NAMES[pc];
        const bool  sharp = name[1] == '#';

        // Big letter, with a small right-hand column for sharp (top) + octave.
        const double base_h = nbh * 0.46;
        const double base_w = base_h * 0.62;
        const double col_w  = base_w * 0.52;
        const double ggap   = base_w * 0.24;
        const double grp_w  = base_w + ggap + col_w;
        const double gx     = ncx - grp_w * 0.5;
        const double ly     = (cy - nbh * 0.13) - base_h * 0.5;

        seg14(gx, ly, base_w, base_h, seg14_mask(name[0]), col);

        const double rx = gx + base_w + ggap;
        if (oct >= 0 && oct <= 9) {
            const double oh = col_w * 1.55;
            seg14(rx, ly + base_h - oh, col_w, oh, seg14_mask((char)('0' + oct)), col);
        }
        if (sharp) {
            const double hs = col_w * 0.95;
            const double hx = rx + (col_w - hs) * 0.5;
            const double hy = ly;
            cairo_set_source_rgba(cr, col[0], col[1], col[2], 1.0);
            cairo_set_line_width(cr, std::fmax(1.5, hs * 0.14));
            cairo_move_to(cr, hx + hs * 0.34, hy);          cairo_line_to(cr, hx + hs * 0.34, hy + hs);
            cairo_move_to(cr, hx + hs * 0.66, hy);          cairo_line_to(cr, hx + hs * 0.66, hy + hs);
            cairo_move_to(cr, hx, hy + hs * 0.34);          cairo_line_to(cr, hx + hs, hy + hs * 0.34);
            cairo_move_to(cr, hx, hy + hs * 0.66);          cairo_line_to(cr, hx + hs, hy + hs * 0.66);
            cairo_stroke(cr);
        }
        return ly + base_h;
    };

    if (ui->ref_active > 0.5f) {
        // Reference-tone mode: show the string being sounded (amber) + a REF tag.
        // Tap the note box to cycle strings; the plugin drones this pitch.
        int tgt[8];
        const int n = ref_build_targets(ui, tgt);
        if (ui->ref_string < 0)  ui->ref_string = 0;
        if (ui->ref_string >= n) ui->ref_string = n - 1;
        const double gbot = draw_note_glyph(tgt[ui->ref_string], COL_SHARP);
        // REF tag, with the octave shift appended when raised. Tapping this line
        // cycles the octave (0/+1/+2) so a low string's drone can be heard in a
        // comfortable register; tapping elsewhere in the box cycles strings.
        char rtag[24];
        if (ui->ref_octave > 0) std::snprintf(rtag, sizeof rtag, "REF  8va+%d", ui->ref_octave);
        else                    std::snprintf(rtag, sizeof rtag, "REF");
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, sh * 0.085);
        cairo_set_source_rgba(cr, COL_SHARP[0], COL_SHARP[1], COL_SHARP[2], 0.9);
        cairo_text_extents(cr, rtag, &te);
        const double rry = gbot + sh * 0.15;
        cairo_move_to(cr, ncx - (te.width * 0.5 + te.x_bearing), rry);
        cairo_show_text(cr, rtag);
        ui->oct_bx = ncx - te.width * 0.5 - sh * 0.05;
        ui->oct_by = rry + te.y_bearing - sh * 0.02;
        ui->oct_bw = te.width + sh * 0.10;
        ui->oct_bh = te.height + sh * 0.04;
    } else if (voiced && ui->note >= 0.0f) {
        const double gbot = draw_note_glyph((int)std::lround(ui->note), led);

        // Cents: crisp small text, anchored a fixed gap *below the glyph*.
        char cbuf[24];
        std::snprintf(cbuf, sizeof cbuf, "%+.1f\xC2\xA2", ui->cents);  // U+00A2
        cairo_set_font_size(cr, sh * 0.13);
        cairo_set_source_rgba(cr, led[0], led[1], led[2], 0.85);
        cairo_text_extents(cr, cbuf, &te);
        cairo_move_to(cr, ncx - (te.width * 0.5 + te.x_bearing), gbot + sh * 0.16);
        cairo_show_text(cr, cbuf);
    } else {
        // No note: leave the LED window dark. Nothing to show, so show nothing.
    }

    // ---- Tuning reference (A4) readout: plain text (MUTE-button style), centred
    // under the note box, midway between the box bottom and the screen's bottom
    // edge. Always shown — it's the tuning standard, not signal-dependent. ----
    {
        char fbuf[24];
        std::snprintf(fbuf, sizeof fbuf, "%.0f Hz", ui->a4_ref);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        const double fsz = sh * 0.10;
        cairo_set_font_size(cr, fsz);
        cairo_text_extents_t fe;
        cairo_text_extents(cr, fbuf, &fe);
        const double mid_y = (cy + nbh * 0.5 + sy + sh) * 0.5;
        const double tw    = fsz * 0.6;                  // triangle width
        const double tgap  = fsz * 0.4;                  // space before triangle
        const double total = fe.width + tgap + tw;
        const double gleft = ncx - total * 0.5;          // group left edge
        const double fx = gleft - fe.x_bearing;          // text baseline x
        const double fy = mid_y - (fe.height * 0.5 + fe.y_bearing);
        const double* fc = intune ? COL_INTUNE : COL_SHARP;  // sharp/amber, in-tune colour when locked
        cairo_set_source_rgb(cr, fc[0], fc[1], fc[2]);
        cairo_move_to(cr, fx, fy);
        cairo_show_text(cr, fbuf);
        // Down-pointing triangle marking it as a menu.
        const double tx = gleft + fe.width + tgap;
        cairo_move_to(cr, tx,            mid_y - tw * 0.30);
        cairo_line_to(cr, tx + tw,       mid_y - tw * 0.30);
        cairo_line_to(cr, tx + tw * 0.5, mid_y + tw * 0.40);
        cairo_close_path(cr);
        cairo_fill(cr);
        // Hit-rect spanning the whole "440 Hz v".
        const double padx = fsz * 0.35, pady = fsz * 0.45;
        ui->freq_bx = gleft - padx;
        ui->freq_by = mid_y - fe.height * 0.5 - pady;
        ui->freq_bw = total + 2 * padx;
        ui->freq_bh = fe.height + 2 * pady;
    }

    // ---------------- Chevron meter (centred on cy) ----------------
    const int    NCH        = 7;
    const double center_gap = mhalf * 0.15;
    const double span       = mhalf - center_gap;
    const double step       = span / NCH;
    const double chev_w     = step * 0.74;
    const double chev_h     = sh * 0.22;

    // A convex vertical shade shared by every meter element so the whole row
    // reads as one bulging ridge: bright highlight up top, full colour through
    // the middle, shadow at the bottom. The same y-extent for all elements keeps
    // the highlight on one horizontal line running across the meter.
    const double ridge_top = cy - chev_h * 1.45;
    const double ridge_bot = cy + chev_h * 1.45;
    auto convex = [&](const double col[3], double base) {
        cairo_pattern_t* g = cairo_pattern_create_linear(0, ridge_top, 0, ridge_bot);
        cairo_pattern_add_color_stop_rgba(g, 0.0,
            col[0] + (1 - col[0]) * 0.55, col[1] + (1 - col[1]) * 0.55,
            col[2] + (1 - col[2]) * 0.55, base);
        cairo_pattern_add_color_stop_rgba(g, 0.45, col[0], col[1], col[2], base);
        cairo_pattern_add_color_stop_rgba(g, 1.0,
            col[0] * 0.30, col[1] * 0.30, col[2] * 0.30, base);
        return g;
    };

    auto chevron = [&](double x, bool point_right, const double col[3], bool lit) {
        cairo_set_line_width(cr, std::fmax(2.5, sh * 0.05));
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        cairo_pattern_t* g = convex(col, lit ? 1.0 : 0.34);
        cairo_set_source(cr, g);
        if (point_right) {
            cairo_move_to(cr, x - chev_w * 0.5, cy - chev_h);
            cairo_line_to(cr, x + chev_w * 0.5, cy);
            cairo_line_to(cr, x - chev_w * 0.5, cy + chev_h);
        } else {
            cairo_move_to(cr, x + chev_w * 0.5, cy - chev_h);
            cairo_line_to(cr, x - chev_w * 0.5, cy);
            cairo_line_to(cr, x + chev_w * 0.5, cy + chev_h);
        }
        cairo_stroke(cr);
        if (g) cairo_pattern_destroy(g);
    };

    double cclamp = ui->cents;
    if (cclamp >  RANGE) cclamp =  RANGE;
    if (cclamp < -RANGE) cclamp = -RANGE;

    for (int k = 1; k <= NCH; ++k) {
        const double d      = center_gap + (k - 0.5) * step;
        const double thresh = (d / mhalf) * RANGE;
        const bool on_l = voiced && !intune && (-cclamp >= thresh);
        chevron(mcx - d, /*point_right=*/true,  on_l ? COL_FLAT : COL_DIM, on_l);
        const bool on_r = voiced && !intune && (cclamp >= thresh);
        chevron(mcx + d, /*point_right=*/false, on_r ? COL_SHARP : COL_DIM, on_r);
    }

    // Centre zone: only when in tune (a discrete "you're there"), glowing from
    // its core outward.
    if (intune) {
        const double zw = center_gap * 1.5;
        const double zh = chev_h * 2.7;
        rrect(mcx - zw * 0.5, cy - zh * 0.5, zw, zh, zw * 0.42);
        cairo_pattern_t* g = convex(COL_INTUNE, 1.0);
        cairo_set_source(cr, g);
        cairo_fill(cr);
        cairo_pattern_destroy(g);
    }

    // Fine pointer: a wide bar at the exact cents position, glowing from core.
    if (voiced && !intune && std::fabs(ui->cents) <= RANGE) {
        const double px = mcx + (cclamp / RANGE) * mhalf;
        const double* pc = (cclamp < 0 ? COL_FLAT : COL_SHARP);
        const double pw = std::fmax(6.0, sh * 0.051);
        const double ph = chev_h * 0.746;   // compact translucent indicator (15% smaller)
        rrect(px - pw * 0.5, cy - ph * 0.5, pw, ph, pw * 0.45);
        cairo_pattern_t* g = convex(pc, 0.35);   // 65% transparent
        cairo_set_source(cr, g);
        cairo_fill(cr);
        cairo_pattern_destroy(g);
    }

    // ---------------- Tuning reference row (string targets) ----------------
    // The selected tuning's notes drawn as an evenly-spaced row spanning the
    // meter's full width: the first/last notes sit on the outermost chevrons,
    // the rest distribute between them. The span is fixed, so a tuning with
    // fewer strings (e.g. 4-string bass) simply spreads wider. Always shown —
    // it's a tuning reference, not signal-dependent.
    {
        int ti = (int)std::lround(ui->tuning);
        if (ti < 0) ti = 0;
        if (ti >= TUNING_COUNT) ti = TUNING_COUNT - 1;
        const Tuning& tu = TUNINGS[ti];
        const int n = tu.count;

        const double d_max  = center_gap + (NCH - 0.5) * step;  // outermost chevron
        const double left   = mcx - d_max;
        const double right  = mcx + d_max;
        const double row_cy = sy + sh * 0.78;                   // below the meter

        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        // Shrink the glyphs as the string count grows so a 7- or 8-string row
        // doesn't crowd or overlap.
        double rfs = sh * 0.115;
        if      (n >= 8) rfs = sh * 0.088;
        else if (n == 7) rfs = sh * 0.100;
        cairo_set_font_size(cr, rfs);

        const int played = voiced ? (int)std::lround(ui->note) : -1;

        for (int i = 0; i < n; ++i) {
            const double x = (n <= 1) ? mcx
                           : left + (right - left) * (double)i / (n - 1);
            const bool lit    = complete || (i < ui->tgt_count && ui->tgt_lit[i]);
            const bool active = !lit && i < ui->tgt_count && played == ui->tgt_midi[i];
            // Three states: amber = tuned (latched); bright = the string you're
            // sounding right now; dim = an untuned reference.
            if      (lit)    cairo_set_source_rgb (cr, COL_SHARP[0], COL_SHARP[1], COL_SHARP[2]);
            else if (active) cairo_set_source_rgb (cr, COL_LED[0],   COL_LED[1],   COL_LED[2]);
            else             cairo_set_source_rgba(cr, COL_LED[0],   COL_LED[1],   COL_LED[2], 0.55);
            cairo_text_extents_t ne;
            cairo_text_extents(cr, tu.notes[i], &ne);
            cairo_move_to(cr, x - (ne.width * 0.5 + ne.x_bearing),
                              row_cy - (ne.height * 0.5 + ne.y_bearing));
            cairo_show_text(cr, tu.notes[i]);
        }

        // Trigger line below the row: the active tuning's name + a down-caret,
        // matching the A4 readout's "440 Hz v" affordance — shows which tuning
        // is selected at a glance, and that the row is clickable.
        const double name_cy = row_cy + sh * 0.138;
        cairo_set_font_size(cr, sh * 0.072);
        cairo_text_extents_t nme;
        cairo_text_extents(cr, tu.name, &nme);
        const double tcw   = sh * 0.05;                  // caret width
        const double tgap  = sh * 0.035;
        const double total = nme.width + tgap + tcw;
        const double gleft = mcx - total * 0.5;
        const double* nc = intune ? COL_INTUNE : COL_SHARP;  // match the A4 readout
        cairo_set_source_rgb(cr, nc[0], nc[1], nc[2]);
        cairo_move_to(cr, gleft - nme.x_bearing,
                          name_cy - (nme.height * 0.5 + nme.y_bearing));
        cairo_show_text(cr, tu.name);
        const double tcx = gleft + nme.width + tgap;
        cairo_move_to(cr, tcx,             name_cy - tcw * 0.28);
        cairo_line_to(cr, tcx + tcw,       name_cy - tcw * 0.28);
        cairo_line_to(cr, tcx + tcw * 0.5, name_cy + tcw * 0.42);
        cairo_close_path(cr);
        cairo_fill(cr);

        // Clickable hit-rect: the whole note row plus the name/caret line.
        ui->row_bx = left   - sh * 0.10;
        ui->row_bw = (right - left) + sh * 0.20;
        ui->row_by = row_cy - sh * 0.13;
        ui->row_bh = (name_cy + sh * 0.06) - (row_cy - sh * 0.13);
    }

    // ---------------- Logo: circular badge (right region, on cy) -----------
    if (ui->logo) {
        const double iw  = cairo_image_surface_get_width(ui->logo);
        const double ih  = cairo_image_surface_get_height(ui->logo);
        const double s   = std::fmax((2.0 * Rl) / iw, (2.0 * Rl) / ih);   // cover
        const double dw  = iw * s, dh = ih * s;
        // Tint: sharp/amber on tuning complete (or while settings are open), the
        // in-tune colour when locked, else the resting LED-note colour.
        const double* tint = (complete || ui->settings_open)
                             ? COL_SHARP : (intune ? COL_INTUNE : COL_LED);

        cairo_save(cr);
        cairo_new_sub_path(cr);
        cairo_arc(cr, lcx, cy, Rl, 0, 2 * M_PI);
        cairo_clip(cr);
        cairo_translate(cr, lcx - dw * 0.5, cy - dh * 0.5);
        cairo_scale(cr, s, s);
        // Paint the drawing, then tint it via MULTIPLY (keeps the pencil detail).
        cairo_set_source_surface(cr, ui->logo, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_MULTIPLY);
        cairo_set_source_rgb(cr, tint[0], tint[1], tint[2]);
        cairo_paint(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        cairo_restore(cr);

        // Ring, drawn on top of the image so it clearly frames it. Grey
        // (unlit-chevron COL_DIM) by default, lit to match the badge tint.
        cairo_new_sub_path(cr);
        cairo_arc(cr, lcx, cy, Rl, 0, 2 * M_PI);
        if      (complete || ui->settings_open) cairo_set_source_rgb(cr, COL_SHARP[0], COL_SHARP[1], COL_SHARP[2]);
        else if (intune)   cairo_set_source_rgb(cr, COL_INTUNE[0], COL_INTUNE[1], COL_INTUNE[2]);
        else               cairo_set_source_rgba(cr, COL_DIM[0], COL_DIM[1], COL_DIM[2], 0.55);
        cairo_set_line_width(cr, std::fmax(1.5, sh * 0.022));
        cairo_stroke(cr);
    }

    // ----------- TONE + MUTE buttons (plain text, under the logo) ----------
    // Two output toggles sharing one baseline row, centred under the logo. TONE
    // engages the reference tone (pitch-pipe); MUTE the dim/mute. Both light amber
    // when active, grey when off.
    {
        static const double off_grey[3] = { 0.50, 0.50, 0.55 };
        const bool muted = ui->mute       > 0.5f;
        const bool tone  = ui->ref_active > 0.5f;

        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, sh * 0.10);
        cairo_text_extents_t te_t, te_m;
        cairo_text_extents(cr, "TONE", &te_t);
        cairo_text_extents(cr, "MUTE", &te_m);

        const double mid_y = (cy + nbh * 0.5 + sy + sh) * 0.5;   // shared with A4 readout
        const double bgap  = sh * 0.07;
        const double total = te_t.width + bgap + te_m.width;
        const double gx    = lcx - total * 0.5;                  // group left edge
        const double padx  = sh * 0.035, pady = sh * 0.035;

        // TONE (left).
        const double* tc = tone ? COL_SHARP : off_grey;
        cairo_set_source_rgb(cr, tc[0], tc[1], tc[2]);
        cairo_move_to(cr, gx - te_t.x_bearing,
                          mid_y - (te_t.height * 0.5 + te_t.y_bearing));
        cairo_show_text(cr, "TONE");
        ui->tone_bx = gx - padx;
        ui->tone_by = mid_y - te_t.height * 0.5 - pady;
        ui->tone_bw = te_t.width + 2 * padx;
        ui->tone_bh = te_t.height + 2 * pady;

        // MUTE (right).
        const double mx0 = gx + te_t.width + bgap;
        const double* mc = muted ? COL_SHARP : off_grey;
        cairo_set_source_rgb(cr, mc[0], mc[1], mc[2]);
        cairo_move_to(cr, mx0 - te_m.x_bearing,
                          mid_y - (te_m.height * 0.5 + te_m.y_bearing));
        cairo_show_text(cr, "MUTE");
        ui->mute_bx = mx0 - padx;
        ui->mute_by = mid_y - te_m.height * 0.5 - pady;
        ui->mute_bw = te_m.width + 2 * padx;
        ui->mute_bh = te_m.height + 2 * pady;
    }

    // ---------------- Dropdown panel (overlay, when open) ------------------
    // Shared by the A4 (dd_which==0) and tuning (dd_which==1) selectors. A4 items
    // are short ("440 Hz"), centred under the note box; tuning names are long, so
    // that menu is wider, smaller-font, left-aligned and centred on the meter.
    if (ui->dd_open) {
        const bool is_tuning = (ui->dd_which == 1);
        if (is_tuning) ensure_disp();
        const int  count   = is_tuning ? g_disp_n : A4_COUNT;
        const int  cur_tun = (int)std::lround(ui->tuning);              // tuning menu
        const int  cur_idx = (int)std::lround(ui->a4_ref) - A4_MIN;     // A4 menu

        // Dim everything behind the menu.
        cairo_set_source_rgba(cr, 0, 0, 0, 0.45);
        cairo_rectangle(cr, 0, 0, W, H);
        cairo_fill(cr);

        const double item_h = (sh * 0.92) / DD_VISIBLE;
        const double ph     = item_h * DD_VISIBLE;

        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, item_h * 0.7);   // same item font as the A4 menu

        // Panel width + horizontal centre.
        double pw, pcx;
        if (is_tuning) {
            double maxw = 0;
            for (int i = 0; i < TUNING_COUNT; ++i) {
                cairo_text_extents_t e;
                cairo_text_extents(cr, TUNINGS[i].name, &e);
                if (e.width > maxw) maxw = e.width;
            }
            pw  = maxw + item_h * 2.4;   // room for the indent + side padding
            pcx = mcx;
        } else {
            pw  = std::fmax(note_w * 1.25, sw * 0.18);
            pcx = ncx;
        }
        double px = pcx - pw * 0.5;                       // clamp inside the screen
        if (px < sx + 4)               px = sx + 4;
        if (px + pw > sx + sw - 4)     px = sx + sw - 4 - pw;
        const double py = sy + (sh - ph) * 0.5;

        if (ui->dd_scroll < 0) ui->dd_scroll = 0;
        if (ui->dd_scroll > count - DD_VISIBLE) ui->dd_scroll = count - DD_VISIBLE;

        ui->dd_px = px; ui->dd_py = py; ui->dd_pw = pw; ui->dd_ph = ph;
        ui->dd_item_h = item_h;

        rrect(px, py, pw, ph, sh * 0.03);
        cairo_set_source_rgba(cr, 0.04, 0.04, 0.05, 0.55);   // translucent, no border
        cairo_fill(cr);

        const double ind = item_h * 0.55;   // indent step per hierarchy level

        for (int row = 0; row < DD_VISIBLE; ++row) {
            const int idx = ui->dd_scroll + row;
            if (idx < 0 || idx >= count) continue;
            const double iy = py + row * item_h;

            if (is_tuning) {
                const DispRow& dr  = g_disp[idx];
                const bool     sel = (dr.kind == 2 && dr.tuning == cur_tun);
                if (sel) {
                    rrect(px + 2, iy + 1.5, pw - 4, item_h - 3, item_h * 0.18);
                    cairo_set_source_rgba(cr, COL_SHARP[0], COL_SHARP[1], COL_SHARP[2], 0.30);
                    cairo_fill(cr);
                }
                if      (dr.kind == 3) cairo_set_source_rgb(cr, COL_SHARP[0], COL_SHARP[1], COL_SHARP[2]);  // action (amber)
                else if (dr.kind == 0) cairo_set_source_rgb(cr, COL_LED[0], COL_LED[1], COL_LED[2]);
                else if (dr.kind == 1) cairo_set_source_rgb(cr, 0.66, 0.70, 0.80);
                else if (sel)          cairo_set_source_rgb(cr, COL_SHARP[0], COL_SHARP[1], COL_SHARP[2]);
                else                   cairo_set_source_rgb(cr, 0.85, 0.85, 0.88);
                cairo_text_extents_t ie;
                cairo_text_extents(cr, dr.label, &ie);
                const double tx = px + item_h * 0.4 + dr.indent * ind - ie.x_bearing;
                cairo_move_to(cr, tx, iy + item_h * 0.5 - (ie.height * 0.5 + ie.y_bearing));
                cairo_show_text(cr, dr.label);
            } else {
                const bool sel = (idx == cur_idx);
                if (sel) {
                    rrect(px + 2, iy + 1.5, pw - 4, item_h - 3, item_h * 0.18);
                    cairo_set_source_rgba(cr, COL_SHARP[0], COL_SHARP[1], COL_SHARP[2], 0.30);
                    cairo_fill(cr);
                }
                char ib[24];
                std::snprintf(ib, sizeof ib, "%d Hz", A4_MIN + idx);
                cairo_text_extents_t ie;
                cairo_text_extents(cr, ib, &ie);
                if (sel) cairo_set_source_rgb(cr, COL_SHARP[0], COL_SHARP[1], COL_SHARP[2]);
                else     cairo_set_source_rgb(cr, 0.85, 0.85, 0.88);
                cairo_move_to(cr, px + pw * 0.5 - (ie.width * 0.5 + ie.x_bearing),
                                  iy + item_h * 0.5 - (ie.height * 0.5 + ie.y_bearing));
                cairo_show_text(cr, ib);
            }
        }
    }

    // ---------------- Auto-detect overlay ----------------------------------
    if (ui->detect_mode) {
        cairo_set_source_rgba(cr, 0, 0, 0, 0.55);
        cairo_rectangle(cr, 0, 0, W, H);
        cairo_fill(cr);

        const double pw = sw * 0.80, ph = sh * 0.86;
        const double px = sx + (sw - pw) * 0.5, py = sy + (sh - ph) * 0.5;
        rrect(px, py, pw, ph, sh * 0.04);
        cairo_set_source_rgba(cr, 0.05, 0.05, 0.06, 0.94);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, COL_SHARP[0], COL_SHARP[1], COL_SHARP[2], 0.5);
        cairo_set_line_width(cr, 1.5);
        cairo_stroke(cr);

        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        auto ctext = [&](const char* s, double yy, double fs, const double* col, double a) {
            cairo_set_font_size(cr, fs);
            cairo_set_source_rgba(cr, col[0], col[1], col[2], a);
            cairo_text_extents_t e;
            cairo_text_extents(cr, s, &e);
            cairo_move_to(cr, sx + sw * 0.5 - (e.width * 0.5 + e.x_bearing),
                              yy - (e.height * 0.5 + e.y_bearing));
            cairo_show_text(cr, s);
        };

        ctext("DETECT TUNING", py + ph * 0.15, sh * 0.11, COL_SHARP, 1.0);

        if (ui->detect_count == 0) {
            ctext("Play your open strings,", py + ph * 0.40, sh * 0.085, COL_LED, 0.80);
            ctext("one at a time...",        py + ph * 0.55, sh * 0.085, COL_LED, 0.80);
        } else {
            char nb[80]; int p = 0;
            for (int i = 0; i < ui->detect_count && p < (int)sizeof(nb) - 4; ++i) {
                const int pc = ((ui->detect_notes[i] % 12) + 12) % 12;
                p += std::snprintf(nb + p, sizeof(nb) - p, "%s%s", i ? "  " : "", NOTE_NAMES[pc]);
            }
            ctext(nb, py + ph * 0.40, sh * 0.115, COL_LED, 0.92);
            if (ui->detect_best >= 0) {
                char rb[48];
                std::snprintf(rb, sizeof(rb), "%s%s",
                              ui->detect_exact ? "" : "~ ", TUNINGS[ui->detect_best].name);
                ctext(rb, py + ph * 0.60, sh * 0.10,
                      ui->detect_exact ? COL_SHARP : COL_LED, 1.0);
            }
        }

        // Buttons: APPLY / CLEAR / CANCEL.
        const double bw = pw * 0.27, bh = sh * 0.16;
        const double by = py + ph - bh - sh * 0.05;
        const double gp = (pw - 3 * bw) / 4.0;
        const double ax = px + gp, clx = px + 2 * gp + bw, cnx = px + 3 * gp + 2 * bw;
        auto button = [&](double bx, const char* lbl, bool on) {
            rrect(bx, by, bw, bh, bh * 0.25);
            cairo_set_source_rgba(cr, 1, 1, 1, on ? 0.10 : 0.03);
            cairo_fill_preserve(cr);
            cairo_set_source_rgba(cr, 1, 1, 1, 0.15);
            cairo_set_line_width(cr, 1.0);
            cairo_stroke(cr);
            cairo_set_font_size(cr, sh * 0.072);
            cairo_set_source_rgba(cr, 0.90, 0.90, 0.92, on ? 1.0 : 0.4);
            cairo_text_extents_t e;
            cairo_text_extents(cr, lbl, &e);
            cairo_move_to(cr, bx + bw * 0.5 - (e.width * 0.5 + e.x_bearing),
                              by + bh * 0.5 - (e.height * 0.5 + e.y_bearing));
            cairo_show_text(cr, lbl);
        };
        button(ax,  "APPLY",  ui->detect_best >= 0);
        button(clx, "CLEAR",  true);
        button(cnx, "CANCEL", true);
        ui->det_apply_bx = ax;  ui->det_apply_by = by;  ui->det_apply_bw = bw;  ui->det_apply_bh = bh;
        ui->det_clear_bx = clx; ui->det_clear_by = by;  ui->det_clear_bw = bw;  ui->det_clear_bh = bh;
        ui->det_cancel_bx = cnx; ui->det_cancel_by = by; ui->det_cancel_bw = bw; ui->det_cancel_bh = bh;
    }

    // ---------------- Advanced Settings overlay ----------------------------
    if (ui->settings_open) {
        // Dim everything EXCEPT the logo, which stays lit amber as the trigger.
        cairo_new_path(cr);
        cairo_rectangle(cr, 0, 0, W, H);
        cairo_new_sub_path(cr);
        cairo_arc(cr, ui->logo_cx, ui->logo_cy, ui->logo_r, 0, 2 * M_PI);
        cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.62);
        cairo_fill(cr);
        cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);

        // Panel occupies the left area, clear of the logo on the right.
        const double pbx = sx + sw * 0.04;
        const double pbw = (ui->logo_cx - ui->logo_r - sw * 0.05) - pbx;
        const double pby = sy + sh * 0.09;
        const double pbh = sh * 0.82;
        rrect(pbx, pby, pbw, pbh, sh * 0.04);
        cairo_set_source_rgba(cr, 0.06, 0.06, 0.07, 0.95);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, COL_SHARP[0], COL_SHARP[1], COL_SHARP[2], 0.5);
        cairo_set_line_width(cr, 1.5);
        cairo_stroke(cr);
        ui->set_panel_bx = pbx; ui->set_panel_by = pby;
        ui->set_panel_bw = pbw; ui->set_panel_bh = pbh;

        const double pad = pbw * 0.05;
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        cairo_text_extents_t e;

        cairo_set_font_size(cr, sh * 0.095);
        cairo_set_source_rgb(cr, COL_SHARP[0], COL_SHARP[1], COL_SHARP[2]);
        cairo_text_extents(cr, "ADVANCED SETTINGS", &e);
        cairo_move_to(cr, pbx + pad, pby + pbh * 0.13 - (e.height * 0.5 + e.y_bearing));
        cairo_show_text(cr, "ADVANCED SETTINGS");

        // Slider row: label (left), click-to-set track (mid), value (right).
        auto slider = [&](double ry, const char* lbl, double frac, const char* vstr,
                          double& hx, double& hy, double& hw, double& hh) {
            cairo_text_extents_t te;
            cairo_set_font_size(cr, sh * 0.072);
            cairo_set_source_rgb(cr, 0.86, 0.86, 0.90);
            cairo_text_extents(cr, lbl, &te);
            cairo_move_to(cr, pbx + pad, ry - (te.height * 0.5 + te.y_bearing));
            cairo_show_text(cr, lbl);

            const double tx = pbx + pbw * 0.42, tw = pbw * 0.40, th = sh * 0.022;
            rrect(tx, ry - th * 0.5, tw, th, th * 0.5);
            cairo_set_source_rgba(cr, 1, 1, 1, 0.12);
            cairo_fill(cr);
            rrect(tx, ry - th * 0.5, tw * frac, th, th * 0.5);
            cairo_set_source_rgb(cr, COL_SHARP[0], COL_SHARP[1], COL_SHARP[2]);
            cairo_fill(cr);
            cairo_arc(cr, tx + tw * frac, ry, th * 1.5, 0, 2 * M_PI);
            cairo_set_source_rgb(cr, COL_SHARP[0], COL_SHARP[1], COL_SHARP[2]);
            cairo_fill(cr);

            cairo_set_source_rgb(cr, 0.90, 0.90, 0.92);
            cairo_text_extents(cr, vstr, &te);
            cairo_move_to(cr, pbx + pbw - pad - te.width - te.x_bearing,
                              ry - (te.height * 0.5 + te.y_bearing));
            cairo_show_text(cr, vstr);

            hx = tx; hy = ry - sh * 0.07; hw = tw; hh = sh * 0.14;   // padded hit-rect
        };

        // On/off toggle: label (left) + a pill switch with a sliding knob (amber +
        // knob-right when on, grey + knob-left when off). Writes its padded
        // hit-rect through the out-params. (Used by the Ping toggle.)
        auto draw_toggle = [&](double ry, const char* label, bool on,
                               double& hx, double& hy, double& hw, double& hh) {
            cairo_text_extents_t le;
            cairo_set_font_size(cr, sh * 0.072);
            cairo_set_source_rgb(cr, 0.86, 0.86, 0.90);
            cairo_text_extents(cr, label, &le);
            cairo_move_to(cr, pbx + pad, ry - (le.height * 0.5 + le.y_bearing));
            cairo_show_text(cr, label);

            const double sw_h = sh * 0.08, sw_w = sw_h * 1.9;   // pill switch
            const double sx0  = pbx + pad + le.width + sh * 0.08;
            const double sy0  = ry - sw_h * 0.5;
            rrect(sx0, sy0, sw_w, sw_h, sw_h * 0.5);
            if (on) cairo_set_source_rgb(cr, COL_SHARP[0], COL_SHARP[1], COL_SHARP[2]);
            else    cairo_set_source_rgba(cr, 1, 1, 1, 0.15);
            cairo_fill(cr);
            const double kr = sw_h * 0.5 - sh * 0.008;
            const double kx = on ? (sx0 + sw_w - kr - sh * 0.008)
                                 : (sx0 + kr + sh * 0.008);
            cairo_arc(cr, kx, ry, kr, 0, 2 * M_PI);
            cairo_set_source_rgb(cr, 0.95, 0.95, 0.97);
            cairo_fill(cr);
            hx = sx0 - sh * 0.025; hy = sy0 - sh * 0.025;
            hw = sw_w + sh * 0.05; hh = sw_h + sh * 0.05;
        };

        char vb[24];
        std::snprintf(vb, sizeof vb, "%.0f dB", ui->dim_db);
        slider(pby + pbh * 0.34, "Dim Amount", (ui->dim_db + 60.0) / 60.0, vb,
               ui->set_dim_bx, ui->set_dim_by, ui->set_dim_bw, ui->set_dim_bh);

        std::snprintf(vb, sizeof vb, "%.1f\xC2\xA2", ui->tolerance);
        slider(pby + pbh * 0.50, "In-Tune Tolerance", (ui->tolerance - 0.5) / 9.5, vb,
               ui->set_tol_bx, ui->set_tol_by, ui->set_tol_bw, ui->set_tol_bh);

        std::snprintf(vb, sizeof vb, "%.0f%%", ui->sensitivity * 100.0);
        slider(pby + pbh * 0.66, "Sensitivity", ui->sensitivity, vb,
               ui->set_sens_bx, ui->set_sens_by, ui->set_sens_bw, ui->set_sens_bh);

        // CLOSE button (bottom-right of the panel).
        const double cbw = pbw * 0.22, cbh = sh * 0.14;
        const double cbx = pbx + pbw - cbw - pad, cby = pby + pbh - cbh - sh * 0.04;

        // Ping on/off toggle — bottom-left, sharing CLOSE's row.
        draw_toggle(cby + cbh * 0.5, "Ping", ui->ping_enable > 0.5f,
                    ui->set_ping_bx, ui->set_ping_by, ui->set_ping_bw, ui->set_ping_bh);

        rrect(cbx, cby, cbw, cbh, cbh * 0.25);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.10);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.15);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);
        cairo_set_font_size(cr, sh * 0.072);
        cairo_set_source_rgb(cr, 0.90, 0.90, 0.92);
        cairo_text_extents(cr, "CLOSE", &e);
        cairo_move_to(cr, cbx + cbw * 0.5 - (e.width * 0.5 + e.x_bearing),
                          cby + cbh * 0.5 - (e.height * 0.5 + e.y_bearing));
        cairo_show_text(cr, "CLOSE");
        ui->set_close_bx = cbx; ui->set_close_by = cby;
        ui->set_close_bw = cbw; ui->set_close_bh = cbh;
    }

    cairo_pop_group_to_source(cr);  // composite the finished frame in one paint
    cairo_paint(cr);
    cairo_destroy(cr);
    if (ui->dpy) XFlush(ui->dpy);
}

// ---- LV2 UI entry points -------------------------------------------------

LV2UI_Handle instantiate(const LV2UI_Descriptor*, const char*,
                         const char* bundle_path,
                         LV2UI_Write_Function write_function, LV2UI_Controller controller,
                         LV2UI_Widget* widget, const LV2_Feature* const* features)
{
    Window        parent = 0;
    LV2UI_Resize* host_resize = nullptr;

    for (int i = 0; features && features[i]; ++i) {
        if (!std::strcmp(features[i]->URI, LV2_UI__parent))
            parent = (Window)(uintptr_t)features[i]->data;
        else if (!std::strcmp(features[i]->URI, LV2_UI__resize))
            host_resize = (LV2UI_Resize*)features[i]->data;
    }
    if (!parent) return nullptr;  // X11 embedding needs a parent window

    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) return nullptr;

    HawkUI* ui     = new HawkUI();
    ui->dpy        = dpy;
    ui->screen     = DefaultScreen(dpy);
    ui->host_resize= host_resize;
    ui->write      = write_function;
    ui->controller = controller;

    // Match the parent's visual/depth so Cairo renders correctly under any host.
    XWindowAttributes pattr;
    XGetWindowAttributes(dpy, parent, &pattr);
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

    // Load the logo from the plugin bundle (optional; drawn if it loads).
    if (bundle_path) {
        std::string path = bundle_path;
        if (!path.empty() && path.back() != '/') path += '/';
        path += "Hawk.png";
        cairo_surface_t* png = cairo_image_surface_create_from_png(path.c_str());
        if (cairo_surface_status(png) == CAIRO_STATUS_SUCCESS) {
            ui->logo = png;                           // keep the artwork
        } else {
            cairo_surface_destroy(png);
        }
    }

    if (host_resize && host_resize->ui_resize)
        host_resize->ui_resize(host_resize->handle, ui->width, ui->height);

    *widget = (LV2UI_Widget)(uintptr_t)ui->win;
    return ui;
}

void cleanup(LV2UI_Handle handle)
{
    HawkUI* ui = (HawkUI*)handle;
    if (ui->logo)      cairo_surface_destroy(ui->logo);
    if (ui->surface)   cairo_surface_destroy(ui->surface);
    if (ui->win)     XDestroyWindow(ui->dpy, ui->win);
    if (ui->cmap)    XFreeColormap(ui->dpy, ui->cmap);
    if (ui->dpy)     XCloseDisplay(ui->dpy);
    delete ui;
}

void port_event(LV2UI_Handle handle, uint32_t port, uint32_t bufsize,
                uint32_t format, const void* buffer)
{
    if (format != 0 || bufsize != sizeof(float)) return;  // plain control port
    HawkUI* ui = (HawkUI*)handle;
    const float v = *(const float*)buffer;
    switch (port) {
        case PORT_ACTIVE:  ui->mute    = v; break;
        case PORT_DIM_DB:    ui->dim_db    = v; break;
        case PORT_TOLERANCE: ui->tolerance = v; break;
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
        default: break;
    }
    // Redraw happens on the next idle tick.
}

// Set the currently-dragged Advanced-Settings slider from a content-space x, and
// write the matching port. Shared by the initial click and the drag motion.
void set_slider(HawkUI* ui, double mx)
{
    if (ui->drag == 0) {                                    // Dim Amount (-60..0 dB)
        double f = (mx - ui->set_dim_bx) / ui->set_dim_bw;
        f = f < 0 ? 0 : (f > 1 ? 1 : f);
        ui->dim_db = (float)(-60.0 + f * 60.0);
        if (ui->write)
            ui->write(ui->controller, PORT_DIM_DB, sizeof(float), 0, &ui->dim_db);
    } else if (ui->drag == 1) {                             // Tolerance (0.5..10 c)
        double f = (mx - ui->set_tol_bx) / ui->set_tol_bw;
        f = f < 0 ? 0 : (f > 1 ? 1 : f);
        ui->tolerance = (float)(0.5 + f * 9.5);
        if (ui->write)
            ui->write(ui->controller, PORT_TOLERANCE, sizeof(float), 0, &ui->tolerance);
    } else if (ui->drag == 2) {                             // Sensitivity (0..1)
        double f = (mx - ui->set_sens_bx) / ui->set_sens_bw;
        f = f < 0 ? 0 : (f > 1 ? 1 : f);
        ui->sensitivity = (float)f;
        if (ui->write)
            ui->write(ui->controller, PORT_SENSITIVITY, sizeof(float), 0, &ui->sensitivity);
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
                // Map window coords into the letterboxed content box.
                const double mx = ev.xbutton.x - ui->ox, my = ev.xbutton.y - ui->oy;
                const unsigned b = ev.xbutton.button;
                auto in_rect = [&](double bx, double by, double bw, double bh) {
                    return mx >= bx && mx <= bx + bw && my >= by && my <= by + bh;
                };
                const double ldx = mx - ui->logo_cx, ldy = my - ui->logo_cy;
                const bool on_logo = (ldx * ldx + ldy * ldy) <= ui->logo_r * ui->logo_r;
                if (ui->detect_mode) {
                    if (b == Button1) {
                        if (in_rect(ui->det_apply_bx, ui->det_apply_by,
                                    ui->det_apply_bw, ui->det_apply_bh)
                            && ui->detect_best >= 0) {
                            ui->tuning = (float)ui->detect_best;
                            if (ui->write)
                                ui->write(ui->controller, PORT_TUNING,
                                          sizeof(float), 0, &ui->tuning);
                            ui->detect_mode = false;
                        } else if (in_rect(ui->det_clear_bx, ui->det_clear_by,
                                           ui->det_clear_bw, ui->det_clear_bh)) {
                            ui->detect_count = 0;
                            ui->detect_best  = -1;
                            ui->detect_exact = false;
                            ui->detect_pluck_done = false;
                        } else if (in_rect(ui->det_cancel_bx, ui->det_cancel_by,
                                           ui->det_cancel_bw, ui->det_cancel_bh)) {
                            ui->detect_mode = false;
                        }
                    }
                } else if (ui->settings_open) {
                    if (b == Button1) {
                        if (in_rect(ui->set_dim_bx, ui->set_dim_by,
                                    ui->set_dim_bw, ui->set_dim_bh)) {
                            ui->drag = 0;                 // grab Dim Amount
                            set_slider(ui, mx);
                        } else if (in_rect(ui->set_tol_bx, ui->set_tol_by,
                                           ui->set_tol_bw, ui->set_tol_bh)) {
                            ui->drag = 1;                 // grab Tolerance
                            set_slider(ui, mx);
                        } else if (in_rect(ui->set_sens_bx, ui->set_sens_by,
                                           ui->set_sens_bw, ui->set_sens_bh)) {
                            ui->drag = 2;                 // grab Sensitivity
                            set_slider(ui, mx);
                        } else if (in_rect(ui->set_ping_bx, ui->set_ping_by,
                                           ui->set_ping_bw, ui->set_ping_bh)) {
                            ui->ping_enable = (ui->ping_enable > 0.5f) ? 0.0f : 1.0f;
                            if (ui->write)
                                ui->write(ui->controller, PORT_PING_ENABLE,
                                          sizeof(float), 0, &ui->ping_enable);
                        } else if (in_rect(ui->set_close_bx, ui->set_close_by,
                                           ui->set_close_bw, ui->set_close_bh)) {
                            ui->settings_open = false;
                        } else if (!in_rect(ui->set_panel_bx, ui->set_panel_by,
                                            ui->set_panel_bw, ui->set_panel_bh)) {
                            ui->settings_open = false;   // click outside (incl. logo) closes
                        }
                    }
                } else if (ui->dd_open) {
                    if (b == Button1) {
                        bool close = true;             // click-away closes by default
                        if (in_rect(ui->dd_px, ui->dd_py, ui->dd_pw, ui->dd_ph)) {
                            const int row = (int)((my - ui->dd_py) / ui->dd_item_h);
                            const int idx = ui->dd_scroll + row;
                            if (ui->dd_which == 1) {   // tuning menu: rows are headers or tunings
                                ensure_disp();
                                if (idx >= 0 && idx < g_disp_n && g_disp[idx].kind == 2) {
                                    ui->tuning = (float)g_disp[idx].tuning;
                                    if (ui->write)
                                        ui->write(ui->controller, PORT_TUNING,
                                                  sizeof(float), 0, &ui->tuning);
                                } else if (idx >= 0 && idx < g_disp_n && g_disp[idx].kind == 3) {
                                    ui->detect_mode  = true;   // enter auto-detect
                                    ui->detect_count = 0;
                                    ui->detect_best  = -1;
                                    ui->detect_exact = false;
                                    ui->detect_pluck_done = false;
                                } else {
                                    close = false;     // clicked a header (or gap): stay open
                                }
                            } else if (idx >= 0 && idx < A4_COUNT) {
                                ui->a4_ref = (float)(A4_MIN + idx);
                                if (ui->write)
                                    ui->write(ui->controller, PORT_A4,
                                              sizeof(float), 0, &ui->a4_ref);
                            }
                        }
                        if (close) ui->dd_open = false;
                    } else if (b == Button4) {        // wheel up
                        ui->dd_scroll -= 1;
                    } else if (b == Button5) {        // wheel down
                        ui->dd_scroll += 1;
                    }
                } else if (b == Button1) {
                    if (in_rect(ui->freq_bx, ui->freq_by, ui->freq_bw, ui->freq_bh)) {
                        ui->dd_open   = true;          // open the A4 dropdown
                        ui->dd_which  = 0;
                        ui->dd_scroll = (int)std::lround(ui->a4_ref) - A4_MIN
                                        - DD_VISIBLE / 2;
                    } else if (in_rect(ui->row_bx, ui->row_by, ui->row_bw, ui->row_bh)) {
                        ui->dd_open   = true;          // open the tuning dropdown
                        ui->dd_which  = 1;
                        ui->dd_scroll = disp_row_for_tuning((int)std::lround(ui->tuning))
                                        - DD_VISIBLE / 2;
                    } else if (in_rect(ui->mute_bx, ui->mute_by, ui->mute_bw, ui->mute_bh)) {
                        ui->mute = (ui->mute > 0.5f) ? 0.0f : 1.0f;
                        if (ui->write)
                            ui->write(ui->controller, PORT_ACTIVE,
                                      sizeof(float), 0, &ui->mute);
                    } else if (in_rect(ui->tone_bx, ui->tone_by, ui->tone_bw, ui->tone_bh)) {
                        // Toggle the reference tone. On engaging, start on the
                        // string nearest whatever you were last playing.
                        ui->ref_active = (ui->ref_active > 0.5f) ? 0.0f : 1.0f;
                        if (ui->ref_active > 0.5f) {
                            int tgt[8];
                            const int n = ref_build_targets(ui, tgt);
                            ui->ref_string = (ui->note >= 0.0f)
                                ? ref_nearest_string(tgt, n, (int)std::lround(ui->note))
                                : 0;
                            ref_apply_note(ui);
                        }
                        if (ui->write)
                            ui->write(ui->controller, PORT_REF_ACTIVE,
                                      sizeof(float), 0, &ui->ref_active);
                    } else if (ui->ref_active > 0.5f &&
                               in_rect(ui->oct_bx, ui->oct_by, ui->oct_bw, ui->oct_bh)) {
                        // Reference mode: tap the REF tag to cycle the octave shift.
                        ui->ref_octave = (ui->ref_octave + 1) % 3;   // 0 -> +1 -> +2
                        ref_apply_note(ui);
                    } else if (ui->ref_active > 0.5f &&
                               in_rect(ui->nbox_bx, ui->nbox_by, ui->nbox_bw, ui->nbox_bh)) {
                        // Reference mode: tap the note box to cycle strings.
                        int tgt[8];
                        const int n = ref_build_targets(ui, tgt);
                        ui->ref_string = (n > 0) ? (ui->ref_string + 1) % n : 0;
                        ref_apply_note(ui);
                    } else if (on_logo) {
                        ui->settings_open = true;      // open Advanced Settings
                    }
                }
            } else if (ev.type == MotionNotify) {
                if (ui->settings_open && ui->drag >= 0)
                    set_slider(ui, ev.xmotion.x - ui->ox);
            } else if (ev.type == ButtonRelease) {
                ui->drag = -1;                         // end any slider drag
            }
            // Expose is covered by the unconditional redraw below.
        }
    }
    draw(ui);
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
