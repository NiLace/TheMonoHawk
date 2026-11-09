// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
// Test harness for the MPM pitch detector.
//
// Feed synthetic sine waves at exactly known frequencies, detect them, and
// report the error in cents (1 cent = 1/100 of a semitone). This is the §11
// "synthetic sweep" test: the detector's correctness is a number you can read.

#include "note_mapping.h"
#include "pitch_detector.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static const double pi          = 3.14159265358979323846;
static const double sample_rate = 48000.0;

static int checks_run    = 0;
static int checks_failed = 0;

// Generate a pure sine of `frequency` Hz, `count` samples long (malloc'd; the caller frees it).
static float* make_sine(double frequency, size_t count, double amplitude)
{
    float* buffer = (float*)malloc(count * sizeof(float));
    for (size_t i = 0; i < count; ++i) {
        buffer[i] = (float)(
            amplitude * sin(2.0 * pi * frequency *
                            (double)i / sample_rate));
    }
    return buffer;
}

// Frame long enough to hold several periods of `frequency`, clamped to a
// sensible range. (The real plugin sizes this adaptively from the coarse
// estimate; here we just give the detector a fair window.)
static size_t frame_length(double frequency)
{
    double n = 8.0 * sample_rate / frequency;
    if (n < 2048.0)  n = 2048.0;
    if (n > 16384.0) n = 16384.0;
    return (size_t)n;
}

static double cents_between(double detected, double reference)
{
    return 1200.0 * log2(detected / reference);
}

// Detect a pure tone of `frequency` and check the cents error is within
// `tolerance_cents`, and that the frame is reported voiced.
static void check_tone(const PitchDetector* detector,
                       const char* label,
                       double frequency,
                       double tolerance_cents)
{
    ++checks_run;
    const size_t n = frame_length(frequency);
    float* s = make_sine(frequency, n, 0.5);
    const PitchEstimate e = hawk_pitch_detector_analyze(detector, s, n);
    free(s);

    const double cents = e.voiced ? cents_between(e.frequency, frequency) : 0.0;
    const int    ok    = e.voiced && fabs(cents) <= tolerance_cents;
    if (!ok) ++checks_failed;

    printf("  [%s] %-22s f=%9.3f Hz  detected=%9.3f  err=%+7.3f cent  "
           "clarity=%.3f  voiced=%d\n",
           ok ? "PASS" : "FAIL", label,
           frequency, e.frequency, cents, e.clarity, e.voiced ? 1 : 0);
}

int main(void)
{
    PitchDetector detector;
    hawk_pitch_detector_init(&detector, sample_rate, HAWK_PD_DEFAULTS);

    printf("== guitar standard tuning (A4 = 440) ==\n");
    check_tone(&detector, "E2 low E",  82.4069, 1.0);
    check_tone(&detector, "A2",       110.0000, 1.0);
    check_tone(&detector, "D3",       146.8324, 1.0);
    check_tone(&detector, "G3",       195.9977, 1.0);
    check_tone(&detector, "B3",       246.9417, 1.0);
    check_tone(&detector, "E4 high E", 329.6276, 1.0);

    printf("== bass, including 5-string low B ==\n");
    check_tone(&detector, "B0 low B",  30.8677, 1.0);
    check_tone(&detector, "E1",        41.2034, 1.0);
    check_tone(&detector, "A1",        55.0000, 1.0);

    printf("== upper range ==\n");
    check_tone(&detector, "A4",       440.0000, 1.0);
    check_tone(&detector, "A5",       880.0000, 1.0);
    check_tone(&detector, "E6",      1318.5102, 1.0);

    // End-to-end: detector feeds the note mapper. Take A4 sharpened by exactly
    // +15 cents, detect it, and confirm we recover "A4, +15 cents".
    printf("== end-to-end: detect -> note + cents ==\n");
    {
        ++checks_run;
        const double freq = hawk_note_to_frequency(69, 440.0) *
                            pow(2.0, 15.0 / 1200.0);
        const size_t n = frame_length(freq);
        float* s = make_sine(freq, n, 0.5);
        const PitchEstimate e = hawk_pitch_detector_analyze(&detector, s, n);
        free(s);
        const NoteReading r = hawk_frequency_to_note(e.frequency, 440.0);

        const int ok = e.voiced && r.midi_note == 69 &&
                       fabs(r.cents - 15.0) <= 1.0;
        if (!ok) ++checks_failed;
        char name[HAWK_NOTE_NAME_LEN];
        printf("  [%s] A4 +15c            -> %s %+6.2f cents\n",
               ok ? "PASS" : "FAIL",
               hawk_note_name(r.midi_note, name), r.cents);
    }

    // Silence must read as unvoiced, not chase noise into a phantom pitch.
    printf("== gating ==\n");
    {
        ++checks_run;
        float* silence = (float*)calloc(4096, sizeof(float));
        const PitchEstimate e = hawk_pitch_detector_analyze(&detector, silence, 4096);
        free(silence);
        const int ok = !e.voiced;
        if (!ok) ++checks_failed;
        printf("  [%s] silence             -> voiced=%d (want 0)\n",
               ok ? "PASS" : "FAIL", e.voiced ? 1 : 0);
    }

    // ---- DC offset must not blind the detector ----------------------------
    // Before the mean removal in analyze(), an offset of 0.30 made this return f = 0 and
    // clarity = 0 -- not a degraded reading, no reading at all, because the hump search needs
    // the NSDF's zero crossings and DC removes them. The reading must now be independent of
    // the offset, so the test asserts across the whole range rather than at one value.
    printf("\n== DC offset must not blind the detector ==\n");
    static const double dcs[] = { 0.0, 0.15, 0.30, 0.50, -0.40 };
    for (size_t k = 0; k < sizeof dcs / sizeof dcs[0]; ++k) {
        const double dc = dcs[k];
        ++checks_run;
        const size_t n = frame_length(82.407);
        float* x = make_sine(82.407, n, 0.4);
        for (size_t i = 0; i < n; ++i) x[i] += (float)dc;
        const PitchEstimate e = hawk_pitch_detector_analyze(&detector, x, n);
        free(x);
        const double cents = e.frequency > 0.0
                           ? 1200.0 * log2(e.frequency / 82.407) : 1e9;
        const int ok = e.voiced && fabs(cents) < 1.0 && e.clarity > 0.95;
        if (!ok) ++checks_failed;
        printf("  [%s] DC %+0.2f -> %8.3f Hz  %+6.2f c  clarity %.3f\n",
               ok ? "PASS" : "FAIL", dc, e.frequency, e.frequency > 0 ? cents : 0.0,
               e.clarity);
    }

    hawk_pitch_detector_free(&detector);
    printf("\n%d checks, %d failed\n", checks_run, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
