// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
// Test harness for the note-mapping core.
//
// This program is meant to be *read as numbers*: it feeds known frequencies in,
// prints what the core reports back, and marks each line PASS or FAIL against a
// tolerance. It exits 0 only if every check passes, so Meson treats a single
// wrong number as a failed test.

#include "note_mapping.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int checks_run    = 0;
static int checks_failed = 0;

static void report(const char* description,
                   double expected,
                   double got,
                   double tolerance)
{
    ++checks_run;
    const double error = got - expected;
    const int    ok    = fabs(error) <= tolerance;
    if (!ok) ++checks_failed;

    printf("  [%s] %-38s expected %12.5f  got %12.5f  err %+.5f\n",
           ok ? "PASS" : "FAIL",
           description, expected, got, error);
}

static void report_text(const char* description,
                        const char* expected,
                        const char* got)
{
    ++checks_run;
    const int ok = (strcmp(expected, got) == 0);
    if (!ok) ++checks_failed;

    printf("  [%s] %-38s expected %12s  got %12s\n",
           ok ? "PASS" : "FAIL",
           description, expected, got);
}

// A frequency that is `cents` away from `midi_note` at the given A4 reference.
static double detuned(int midi_note, double cents, double a4_hz)
{
    return hawk_note_to_frequency(midi_note, a4_hz) *
           pow(2.0, cents / 1200.0);
}

int main(void)
{
    char name[HAWK_NOTE_NAME_LEN];

    printf("== note_to_frequency (A4 = 440 Hz) ==\n");
    report("A4  (midi 69)",  440.000, hawk_note_to_frequency(69, 440.0), 1e-3);
    report("A2  (midi 45)",  110.000, hawk_note_to_frequency(45, 440.0), 1e-3);
    report("E2  guitar low E", 82.407, hawk_note_to_frequency(40, 440.0), 1e-2);
    report("B0  5-string low B", 30.868, hawk_note_to_frequency(23, 440.0), 1e-2);

    printf("== note_to_frequency (A4 = 432 Hz) ==\n");
    report("A4  at 432",     432.000, hawk_note_to_frequency(69, 432.0), 1e-3);

    printf("== frequency_to_note: note + cents (A4 = 440 Hz) ==\n");
    {
        NoteReading r = hawk_frequency_to_note(440.0, 440.0);
        report("440 Hz -> note",   69.0, r.midi_note, 0.0);
        report("440 Hz -> cents",   0.0, r.cents,     1e-6);
    }
    {
        NoteReading r = hawk_frequency_to_note(261.6256, 440.0);   // middle C
        report("261.63 Hz -> note", 60.0, r.midi_note, 0.0);
        report("261.63 Hz -> cents", 0.0, r.cents,     1e-2);
    }
    {
        NoteReading r = hawk_frequency_to_note(detuned(69, +20.0, 440.0), 440.0);
        report("A4 +20c -> note",  69.0, r.midi_note, 0.0);
        report("A4 +20c -> cents", 20.0, r.cents,     1e-3);
    }
    {
        NoteReading r = hawk_frequency_to_note(detuned(40, -7.3, 440.0), 440.0);
        report("E2 -7.3c -> note", 40.0, r.midi_note, 0.0);
        report("E2 -7.3c -> cents", -7.3, r.cents,    1e-3);
    }
    {
        NoteReading r = hawk_frequency_to_note(detuned(23, +0.0, 440.0), 440.0);
        report("B0 (low B) -> note", 23.0, r.midi_note, 0.0);
    }

    printf("== invalid input ==\n");
    {
        NoteReading r = hawk_frequency_to_note(0.0, 440.0);
        report("0 Hz -> valid flag", 0.0, r.valid ? 1.0 : 0.0, 0.0);
    }

    printf("== note_name ==\n");
    report_text("midi 69", "A4",  hawk_note_name(69, name));
    report_text("midi 60", "C4",  hawk_note_name(60, name));
    report_text("midi 61", "C#4", hawk_note_name(61, name));
    report_text("midi 40", "E2",  hawk_note_name(40, name));
    report_text("midi 23", "B0",  hawk_note_name(23, name));

    printf("\n%d checks, %d failed\n", checks_run, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
