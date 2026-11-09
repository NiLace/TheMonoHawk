// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
// ui_ports_probe.c -- gate: what the host sends the GUI through port_event is shown as it is, and garbage is not.
//
// 1. The host owns ref_note (session restore, automation): with TONE on and ref_note = G4 (67) on Standard, the note
//    screen must show G (string 4, G3, one octave up); and a tuning change with TONE on must re-write ref_note.
// 2. NaN and infinities on every port must leave the GUI's state finite: the strobe disc keeps moving after a NaN phase,
//    a NaN level does not fill the meter, a NaN tuning or A4 is ignored.
// Negative arms: built with -DHAWK_UI_IGNORE_REF_NOTE=1 or -DHAWK_UI_TRUST_PORTS=1 it must FAIL (test/meson.build).
// run: hawk_ui_ports_probe      exit 0 = PASS
#include "../lv2/hawk_ui.c"

static int writes_ref = 0;
static float last_ref = -1.0f;
static void rec_write(LV2UI_Controller controller, uint32_t port, uint32_t size, uint32_t protocol, const void* buf)
{
    (void)controller; (void)size; (void)protocol;
    if (port == PORT_REF_NOTE) { ++writes_ref; last_ref = *(const float*)buf; }
}
static void pe(HawkUI* ui, int port, float v) { port_event(ui, port, sizeof(float), 0, &v); }

static int bad = 0;
static void check(const char* what, bool ok) { printf("%-62s %s\n", what, ok ? "ok" : "FAIL"); if (!ok) ++bad; }

int main(void)
{
    {   // 1. ref_note
        HawkUI ui;
        hawk_ui_init(&ui);
        ui.write = rec_write;
        pe(&ui, PORT_TUNING, 0);
        pe(&ui, PORT_REF_ACTIVE, 1);
        pe(&ui, PORT_REF_NOTE, 67);                       // G4 = G3 (string 4) + one octave
        update_tuning_progress(&ui, false, false);         // the first frame: builds the targets
        check("ref_note 67 on Standard: the screen shows G4's string, G3", ref_draw_note(&ui) == 55);
        check("ref_note 67 on Standard: octave tag +1", ui.ref_octave == 1);
        writes_ref = 0;
        pe(&ui, PORT_TUNING, 2);                           // Drop C#: the host changes the tuning
        update_tuning_progress(&ui, false, false);
        int tgt[8];
        build_targets(2, tgt);
        check("a tuning change with TONE on re-writes ref_note", writes_ref > 0);
        check("... to the same string of the new tuning", writes_ref > 0 && (int)last_ref == tgt[ui.ref_string] + 12 * ui.ref_octave);
    }
    {   // 2. garbage
        HawkUI ui;
        hawk_ui_init(&ui);
        const float nan = NAN, inf = INFINITY;
        pe(&ui, PORT_TUNING, 0); pe(&ui, PORT_NOTE, 40); pe(&ui, PORT_CENTS, 8); pe(&ui, PORT_VOICED, 1);
        pe(&ui, PORT_STROBE_MODE, 1); pe(&ui, PORT_A4, 440);
        bool s_in = false;
        for (int p = 2; p <= 24; ++p) { pe(&ui, p, nan); pe(&ui, p, inf); pe(&ui, p, -inf); }
        pe(&ui, PORT_NOTE, 40); pe(&ui, PORT_CENTS, 8); pe(&ui, PORT_VOICED, 1); pe(&ui, PORT_STROBE_MODE, 1);
        for (int i = 0; i < 20; ++i) { pe(&ui, PORT_STROBE, 0.1f * i); update_strobe(&ui, true, false, &s_in); }
        check("strobe disc position finite after NaN/Inf phases", isfinite(ui.strobe_pos));
        check("level finite and within 0..1", isfinite(ui.level) && ui.level >= 0 && ui.level <= 1);
        check("tuning finite", isfinite(ui.tuning));
        check("A4 finite and within 380..500", isfinite(ui.a4_ref) && ui.a4_ref >= 380 && ui.a4_ref <= 500);
        check("settings finite", isfinite(ui.dim_db) && isfinite(ui.tolerance) && isfinite(ui.sensitivity));
    }
    printf("%s\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
