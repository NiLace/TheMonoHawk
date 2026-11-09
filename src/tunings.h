// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
// Shared tuning table — the single source of truth for both the GUI (draws the
// string row, the tuning menu) and the DSP (derives the per-string targets used
// to gate the in-tune ping). Keeping it here means the note data can never drift
// between the picture on screen and what the detector actually looks for.
//
// Note glyphs are sharps-only to match the live detector readout (flat keys
// spelled with sharps: Eb->D#, Bb->A#, etc.); the dropdown labels keep
// conventional names.
#pragma once

#include <stddef.h>

typedef struct Tuning {
    const char* name;
    int         count;
    const char* notes[8];   // up to 8 strings (8-string guitar)
    // An all-unison tuning: the targets spread over the strings' natural registers (see build_targets)
    int         spread_registers;
} Tuning;

// Index order MUST match the scalePoints in hawk.ttl's "tuning" port.
static const Tuning TUNINGS[] = {
    { "Standard",            6, { "E",  "A",  "D",  "G",  "B",  "E" }, 0 },
    // Drop tunings (lower the 6th, sometimes more).
    { "Drop D",              6, { "D",  "A",  "D",  "G",  "B",  "E" }, 0 },
    { "Drop C#",             6, { "C#", "G#", "C#", "F#", "A#", "D#" }, 0 },
    { "Drop C",              6, { "C",  "G",  "C",  "F",  "A",  "D" }, 0 },
    { "Drop B",              6, { "B",  "F#", "B",  "E",  "G#", "C#" }, 0 },
    { "Drop A",              6, { "A",  "E",  "A",  "D",  "F#", "B" }, 0 },
    { "Drop G",              6, { "G",  "D",  "G",  "C",  "E",  "A" }, 0 },
    // Open (major).
    { "Open A",              6, { "E",  "A",  "C#", "E",  "A",  "E" }, 0 },
    { "Open B",              6, { "B",  "F#", "B",  "F#", "B",  "D#" }, 0 },
    { "Open C",              6, { "C",  "G",  "C",  "G",  "C",  "E" }, 0 },
    { "Open D",              6, { "D",  "A",  "D",  "F#", "A",  "D" }, 0 },
    { "Open E",              6, { "E",  "B",  "E",  "G#", "B",  "E" }, 0 },
    { "Open G",              6, { "D",  "G",  "D",  "G",  "B",  "D" }, 0 },
    // Open (minor / cross-note).
    { "Cross-Note Am",       6, { "E",  "A",  "E",  "A",  "C",  "E" }, 0 },
    { "Cross-Note Cm",       6, { "C",  "G",  "C",  "G",  "C",  "D#" }, 0 },
    { "Cross-Note Dm",       6, { "D",  "A",  "D",  "F",  "A",  "D" }, 0 },
    { "Cross-Note Em",       6, { "E",  "B",  "E",  "G",  "B",  "E" }, 0 },
    { "Cross-Note Gm",       6, { "D",  "G",  "D",  "G",  "A#", "D" }, 0 },
    // Modal / suspended.
    { "DADGAD",              6, { "D",  "A",  "D",  "G",  "A",  "D" }, 0 },
    { "Asus4",               6, { "E",  "A",  "D",  "E",  "A",  "E" }, 0 },
    { "Gsus4",               6, { "D",  "G",  "D",  "G",  "C",  "D" }, 0 },
    { "Csus4",               6, { "C",  "G",  "C",  "F",  "G",  "C" }, 0 },
    { "Esus2",               6, { "E",  "B",  "E",  "F#", "B",  "E" }, 0 },
    // Lowered whole-instrument (Standard transposed down N semitones).
    { "Eb (Half Step Down)", 6, { "D#", "G#", "C#", "F#", "A#", "D#" }, 0 },
    { "D (Whole Step Down)", 6, { "D",  "G",  "C",  "F",  "A",  "D" }, 0 },
    { "C# Tuning",           6, { "C#", "F#", "B",  "E",  "G#", "C#" }, 0 },
    { "C Tuning",            6, { "C",  "F",  "A#", "D#", "G",  "C" }, 0 },
    { "B Tuning",            6, { "B",  "E",  "A",  "D",  "F#", "B" }, 0 },
    { "Bb Tuning",           6, { "A#", "D#", "G#", "C#", "F",  "A#" }, 0 },
    { "A Tuning",            6, { "A",  "D",  "G",  "C",  "E",  "A" }, 0 },
    { "Ab Tuning",           6, { "G#", "C#", "F#", "B",  "D#", "G#" }, 0 },
    { "G Tuning",            6, { "G",  "C",  "F",  "A#", "D",  "G" }, 0 },
    { "F# Tuning",           6, { "F#", "B",  "E",  "A",  "C#", "F#" }, 0 },
    { "F Tuning",            6, { "F",  "A#", "D#", "G#", "C",  "F" }, 0 },
    // Regular (uniform-interval).
    { "All Fourths",         6, { "E",  "A",  "D",  "G",  "C",  "F" }, 0 },
    { "Major Thirds",        6, { "E",  "G#", "C",  "E",  "G#", "C" }, 0 },
    { "All Fifths",          6, { "C",  "G",  "D",  "A",  "E",  "B" }, 0 },
    { "New Standard (Fripp)",6, { "C",  "G",  "D",  "A",  "E",  "G" }, 0 },
    { "Ostrich",             6, { "E",  "E",  "E",  "E",  "E",  "E" }, 1 },
    // Bass.
    { "Bass (4-string)",     4, { "E",  "A",  "D",  "G" }, 0 },
    { "Bass Drop D (4)",     4, { "D",  "A",  "D",  "G" }, 0 },
    { "Bass Eb (4)",         4, { "D#", "G#", "C#", "F#" }, 0 },
    { "Bass D (4)",          4, { "D",  "G",  "C",  "F" }, 0 },
    { "Bass (5-string)",     5, { "B",  "E",  "A",  "D",  "G" }, 0 },
    { "Bass Drop A (5)",     5, { "A",  "E",  "A",  "D",  "G" }, 0 },
    { "Bass (6-string)",     6, { "B",  "E",  "A",  "D",  "G",  "C" }, 0 },
    // Extended-range guitar.
    { "7-String Standard",   7, { "B",  "E",  "A",  "D",  "G",  "B",  "E" }, 0 },
    { "7-String Drop A",     7, { "A",  "E",  "A",  "D",  "G",  "B",  "E" }, 0 },
    { "8-String Standard",   8, { "F#", "B",  "E",  "A",  "D",  "G",  "B",  "E" }, 0 },
    { "8-String Drop E",     8, { "E",  "B",  "E",  "A",  "D",  "G",  "B",  "E" }, 0 },
};
#define TUNING_COUNT ((int)(sizeof(TUNINGS) / sizeof(TUNINGS[0])))

// MIDI note of the LOWEST string for each tuning (same index order as TUNINGS).
// The other strings' octaves are derived (each string = the lowest pitch of its
// letter that is >= the previous string), which reproduces how a guitar/bass
// actually ascends. MIDI: C-1=0, so E2=40, E1=28, B0=23. These are the only
// octave data we author; everything else falls out of the note letters.
static const int TUNING_ROOT[] = {
    40,                              // Standard          E2
    38, 37, 36, 35, 33, 31,          // Drop D/C#/C/B/A/G
    40, 35, 36, 38, 40, 38,          // Open A/B/C/D/E/G   (Open B tuned down to B1)
    40, 36, 38, 40, 38,              // Cross-Note Am/Cm/Dm/Em/Gm
    38, 40, 38, 36, 40,              // DADGAD / Asus4 / Gsus4 / Csus4 / Esus2
    39, 38, 37, 36, 35, 34, 33, 32, 31, 30, 29,  // Eb..F lowered whole-instrument
    40, 40, 36, 36, 40,              // All Fourths / Major Thirds / All Fifths / Fripp NST / Ostrich
    28, 26, 27, 26, 23, 21, 23,      // Bass 4/Drop D/Eb/D, 5/Drop A, 6
    35, 33,                          // 7-String Standard / Drop A   B1 / A1
    30, 28,                          // 8-String Standard / Drop E   F#1 / E1
};
// Longest tuning in the table.
//
// ⚠ This is not decoration. build_targets() writes tu->count entries into a buffer whose size it is
// never told. The longest tuning is the 8-string standard, EXACTLY 8, and the per-string arrays of
// the plugin and the GUI are sized 8: adding one nine-string tuning would corrupt
// them silently, and nine-string guitars exist. C cannot compute the maximum of the table at compile
// time, so TUNING_MAX_STRINGS is written here and test/test_tunings.c fails if any tuning has more
// strings than it (or if it is larger than the longest tuning), which turns a ninth string into a
// red `meson test`. Callers size their buffers from TUNING_MAX_STRINGS, not from a guess.
#define TUNING_MAX_STRINGS 8

_Static_assert(sizeof(TUNING_ROOT) / sizeof(int) == sizeof(TUNINGS) / sizeof(TUNINGS[0]),
               "TUNING_ROOT must have one entry per tuning");

// Pitch class (0..11) of a note-name string like "C", "F#".
static inline int pitch_class(const char* s) {
    static const int base[7] = { 9, 11, 0, 2, 4, 5, 7 };  // A B C D E F G
    int pc = base[(s[0] - 'A' + 7) % 7];
    if (s[1] == '#') pc = (pc + 1) % 12;
    return pc;
}

// Fill out[] with the MIDI note of each string for tuning t; returns the count.
static inline int build_targets(int t, int* out) {
    if (t < 0 || t >= TUNING_COUNT) t = 0;   // never index TUNINGS[]/TUNING_ROOT[] out of bounds
    const Tuning* tu = &TUNINGS[t];
    // Ostrich is an all-unison drone, so the "ascend to the next string" rule
    // would collapse every string onto one pitch (and the per-string checklist
    // would light all at once). Spread the E across the strings' natural
    // registers instead, so each target is a real, separately-tunable string.
    if (tu->spread_registers) {
        static const int spread[6] = { 40, 40, 52, 52, 64, 64 };  // E2 E2 E3 E3 E4 E4
        for (int i = 0; i < 6; ++i) out[i] = spread[i];
        return 6;
    }
    out[0] = TUNING_ROOT[t];
    for (int i = 1; i < tu->count; ++i) {
        const int pc   = pitch_class(tu->notes[i]);
        const int prev = out[i - 1];
        const int rise = ((pc - prev) % 12 + 12) % 12;    // 0..11 above prev
        out[i] = prev + rise;                              // lowest of this pc >= prev
    }
    return tu->count;
}
