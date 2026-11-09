/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) The NL Sounds contributors */
#pragma once

#include <stdbool.h>


// Result of mapping a detected frequency onto the equal-tempered scale.
typedef struct NoteReading {
    int    midi_note;  // MIDI note number (69 = A4)
    double cents;      // deviation from that note, in range [-50, +50)
    double frequency;  // the input frequency (Hz), echoed back
    bool   valid;      // false when the input frequency is non-positive
} NoteReading;

// Exact frequency (Hz) of a MIDI note for a given A4 reference pitch.
//   hawk_note_to_frequency(69, 440.0) == 440.0
double hawk_note_to_frequency(int midi_note, double a4_hz);

// Map a frequency (Hz) to the nearest equal-tempered note, given the A4
// reference pitch. Returns the note number and the cents deviation from it.
NoteReading hawk_frequency_to_note(double frequency_hz, double a4_hz);

// Scientific-pitch name of a MIDI note, e.g. 69 -> "A4", 23 -> "B0", 61 -> "C#4", written into `out` (at least
// HAWK_NOTE_NAME_LEN bytes: two letters and an octave of up to 11 characters) and returned.
#define HAWK_NOTE_NAME_LEN 16
const char* hawk_note_name(int midi_note, char* out);
