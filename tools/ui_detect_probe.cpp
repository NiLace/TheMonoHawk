// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
// ui_detect_probe.cpp -- gate: DETECT TUNING names the tuning of open strings played one after another.
//
// The GUI's detect logic (lv2/hawk_ui.cpp: update_detect, match_detect) is compiled in and fed the plugin's outputs as
// a guitarist produces them: each open string plucked while the previous one still rings, so the note changes but
// `voiced` never drops. Cases: Drop D low to high (after the first note alone the best match must already be a GUITAR
// tuning, not a 4-string bass; after all six, Drop D, exact); Standard high to low; a 4-string bass (E1 A1 D2 G2);
// Drop D with every string MUTED quickly (150 ms of reading, then silence); and a strum (several strings at once)
// that must add nothing.
// Negative arm: built with -DHAWK_UI_DETECT_OLD=1 (capture only after a silence, ties to the fewest strings) it must
// FAIL (test/meson.build). Takes ~5 s: the dwell is real time.
// run: hawk_ui_detect_probe      exit 0 = PASS
#include "../lv2/hawk_ui.cpp"

#include <thread>

namespace {
int index_of(const char* name)
{
    for (int i = 0; i < TUNING_COUNT; ++i)
        if (!std::strcmp(TUNINGS[i].name, name)) return i;
    return -1;
}

// Play the notes one after another, each read for `hold_ms`, then `gap_ms` of silence (0: the next string follows
// while this one rings, `voiced` never dropping); return the best match.
int play(HawkUI& ui, const int* notes, int n, int hold_ms, int* after_first, int gap_ms = 0, bool strum = false)
{
    ui.detect_mode = true; ui.detect_count = 0; ui.detect_best = -1; ui.detect_exact = false;
    ui.detect_cap_note = -1; ui.detect_pluck_done = false; ui.detect_prev_voiced = false;
    for (int k = 0; k < n; ++k) {
        ui.note = (float)notes[k];
        for (int t = 0; t < hold_ms; t += 20) {            // the GUI's frames
            update_detect(&ui, true, strum);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        for (int t = 0; t < gap_ms; t += 20) {
            update_detect(&ui, false, false);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (k == 0 && after_first) *after_first = ui.detect_best;
    }
    update_detect(&ui, false, false);                      // the last string stops
    return ui.detect_best;
}
}  // namespace

int main()
{
    int bad = 0;
    auto check = [&](const char* what, int got, const char* want) {
        const int w = index_of(want);
        const bool ok = got == w;
        std::printf("%-44s %-22s want %-22s %s\n", what, got >= 0 ? TUNINGS[got].name : "(none)", want, ok ? "ok" : "FAIL");
        if (!ok) ++bad;
    };
    HawkUI ui;
    const int dropd[6] = { 38, 45, 50, 55, 59, 64 };           // D2 A2 D3 G3 B3 E4
    int first = -1;
    const int got = play(ui, dropd, 6, 400, &first);
    check("Drop D, low to high: after the low D alone", first, "Drop D");
    check("Drop D, low to high: all six", got, "Drop D");
    if (!ui.detect_exact) { std::printf("Drop D: not reported exact\n"); ++bad; }
    const int std_down[6] = { 64, 59, 55, 50, 45, 40 };        // E4 B3 G3 D3 A2 E2
    check("Standard, high to low: all six", play(ui, std_down, 6, 400, nullptr), "Standard");
    const int bass4[4] = { 28, 33, 38, 43 };                    // E1 A1 D2 G2
    check("4-string bass: all four", play(ui, bass4, 4, 400, nullptr), "Bass (4-string)");
    check("Drop D, each string muted after 150 ms", play(ui, dropd, 6, 150, nullptr, 150), "Drop D");
    play(ui, dropd, 6, 400, nullptr, 0, true);
    if (ui.detect_count != 0) { std::printf("a strum added %d notes (want 0)\n", ui.detect_count); ++bad; }
    else std::printf("%-44s %-22s want %-22s ok\n", "a strum (several strings at once)", "(nothing taken)", "(nothing)");
    std::printf("%s\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
