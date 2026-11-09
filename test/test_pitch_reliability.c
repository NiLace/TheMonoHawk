// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
// Reliability stress tests for the MPM pitch detector.
//
// Pure sines (test_pitch_detector.c) prove precision on the easy case. Real
// strings are not sines: they have a stack of harmonics, sometimes a weak or
// missing fundamental, an attack/decay envelope, and noise. This is where
// octave errors and jitter live. Every signal here is deterministic, so the
// pass/fail numbers are reproducible.

#include "note_mapping.h"
#include "pitch_detector.h"
#include "../tools/hawk_rng.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static const double pi          = 3.14159265358979323846;
static const double sample_rate = 48000.0;

static int checks_run    = 0;
static int checks_failed = 0;

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

static double rms_of_harmonics(const double* amps, size_t n_amps)
{
    double sum_sq = 0.0;
    for (size_t h = 0; h < n_amps; ++h) sum_sq += amps[h] * amps[h];
    return sqrt(sum_sq / 2.0);  // RMS of a sum of incoherent sinusoids
}

// A harmonic stack: its amplitudes and how many there are.
typedef struct Stack { const double* amps; size_t n; } Stack;
#define STACK(a) ((Stack){ (a), sizeof(a) / sizeof((a)[0]) })

// A frame of samples (malloc'd; the check that takes it frees it).
typedef struct Frame { float* x; size_t n; } Frame;

// Build a realistic tone: a stack of harmonics of f0, with optional string
// inharmonicity (stretched partials), an optional exponential decay envelope,
// and optional white noise at a target SNR (snr_db >= 200: none). Deterministic for a given seed.
static Frame make_tone_full(double f0,
                            size_t count,
                            Stack harmonic_amps,
                            double inharmonicity_b,
                            double decay_tau_seconds,
                            double snr_db,
                            uint32_t seed)
{
    Frame f = { (float*)calloc(count, sizeof(float)), count };
    HawkMT rng;
    hawk_mt_seed(&rng, seed);
    HawkNormal gaussian = hawk_normal(0.0, 1.0);

    const double sig_rms   = rms_of_harmonics(harmonic_amps.amps, harmonic_amps.n);
    const double noise_amp = (snr_db < 200.0)
        ? sig_rms / pow(10.0, snr_db / 20.0)
        : 0.0;

    for (size_t i = 0; i < count; ++i) {
        const double t = (double)i / sample_rate;
        double sample = 0.0;
        for (size_t h = 0; h < harmonic_amps.n; ++h) {
            const int    n  = (int)h + 1;
            double       fn = n * f0;
            if (inharmonicity_b > 0.0) {
                fn = n * f0 * sqrt(1.0 + inharmonicity_b * n * n);
            }
            sample += harmonic_amps.amps[h] * sin(2.0 * pi * fn * t);
        }
        if (decay_tau_seconds > 0.0) {
            sample *= exp(-t / decay_tau_seconds);
        }
        if (noise_amp > 0.0) {
            sample += noise_amp * hawk_normal_draw(&gaussian, &rng);
        }
        f.x[i] = (float)(0.3 * sample);
    }
    return f;
}

static Frame make_tone(double f0, size_t count, Stack harmonic_amps)
{
    return make_tone_full(f0, count, harmonic_amps, 0.0, 0.0, 1e9, 1234u);
}

// Check that a voiced signal lands on the *correct note* (no octave error) and
// within `tol_cents` of f0.
static void check_voiced(const PitchDetector* detector,
                         const char* label,
                         double f0,
                         Frame samples,
                         double tol_cents)
{
    ++checks_run;
    const PitchEstimate e = hawk_pitch_detector_analyze(detector, samples.x, samples.n);
    free(samples.x);
    const int expected_note = hawk_frequency_to_note(f0, 440.0).midi_note;
    const NoteReading got = hawk_frequency_to_note(e.frequency, 440.0);
    const double cents = e.voiced ? cents_between(e.frequency, f0) : 0.0;

    const int ok = e.voiced &&
                   got.midi_note == expected_note &&
                   fabs(cents) <= tol_cents;
    if (!ok) ++checks_failed;

    char name[HAWK_NOTE_NAME_LEN];
    printf("  [%s] %-26s f0=%9.3f -> %-4s %+7.2fc  "
           "(detected %9.3f Hz, clarity %.3f, voiced %d)\n",
           ok ? "PASS" : "FAIL", label, f0,
           e.voiced ? hawk_note_name(got.midi_note, name) : "----",
           cents, e.frequency, e.clarity, e.voiced ? 1 : 0);
}

// Check octave-correctness only. The coarse MPM stage's contract is to identify
// the right note; exact cents under heavy inharmonicity is the fine stage's job
// (design-spec.md §3.2), which does not exist yet. Cents are printed for
// information but not asserted here.
static void check_note_only(const PitchDetector* detector,
                            const char* label,
                            double f0,
                            Frame samples)
{
    ++checks_run;
    const PitchEstimate e = hawk_pitch_detector_analyze(detector, samples.x, samples.n);
    free(samples.x);
    const int expected_note = hawk_frequency_to_note(f0, 440.0).midi_note;
    const NoteReading got = hawk_frequency_to_note(e.frequency, 440.0);
    const double cents = e.voiced ? cents_between(e.frequency, f0) : 0.0;

    const int ok = e.voiced && got.midi_note == expected_note;
    if (!ok) ++checks_failed;

    char name[HAWK_NOTE_NAME_LEN];
    printf("  [%s] %-26s f0=%9.3f -> %-4s  (note octave checked; "
           "cents %+.2f informational, awaits fine stage)\n",
           ok ? "PASS" : "FAIL", label, f0,
           e.voiced ? hawk_note_name(got.midi_note, name) : "----",
           cents);
}

static void check_unvoiced(const PitchDetector* detector,
                           const char* label,
                           const float* samples, size_t count)
{
    ++checks_run;
    const PitchEstimate e = hawk_pitch_detector_analyze(detector, samples, count);
    const int ok = !e.voiced;
    if (!ok) ++checks_failed;
    printf("  [%s] %-26s -> voiced %d (want 0, clarity %.3f)\n",
           ok ? "PASS" : "FAIL", label,
           e.voiced ? 1 : 0, e.clarity);
}

int main(void)
{
    PitchDetector detector;
    hawk_pitch_detector_init(&detector, sample_rate, HAWK_PD_DEFAULTS);

    // A plucked-string-like harmonic stack and some pathological variants.
    static const double guitar_a[]  = {1.0, 0.6, 0.45, 0.3, 0.22, 0.16,
                                       0.12, 0.09};
    static const double bass_a[]    = {1.0, 0.85, 0.7, 0.55, 0.45, 0.35,
                                       0.28, 0.22, 0.18};
    static const double missing_a[] = {0.0, 0.9, 0.7, 0.5, 0.35, 0.25};
    static const double octtrap_a[] = {0.3, 1.0, 0.5, 0.35, 0.25};
    const Stack guitar  = STACK(guitar_a);
    const Stack bass    = STACK(bass_a);
    const Stack missing = STACK(missing_a);
    const Stack octtrap = STACK(octtrap_a);

    printf("== full harmonic stack (realistic timbre) ==\n");
    static const struct { const char* name; double f0; } strings[] = {
            {"E2 low E", 82.4069}, {"A2", 110.0}, {"D3", 146.8324},
            {"G3", 195.9977}, {"B3", 246.9417}, {"E4 high E", 329.6276}};
    for (size_t k = 0; k < sizeof strings / sizeof strings[0]; ++k) {
        const double f0 = strings[k].f0;
        check_voiced(&detector, strings[k].name, f0,
                     make_tone(f0, frame_length(f0), guitar), 1.0);
    }

    printf("== missing fundamental (octave-error trap) ==\n");
    check_voiced(&detector, "E2 no fundamental", 82.4069,
                 make_tone(82.4069, frame_length(82.4069), missing), 1.0);
    check_voiced(&detector, "A1 no fundamental", 55.0,
                 make_tone(55.0, frame_length(55.0), missing), 1.0);
    check_voiced(&detector, "B0 no fundamental", 30.8677,
                 make_tone(30.8677, frame_length(30.8677), missing), 1.0);

    printf("== 2nd harmonic louder than 1st (octave-up trap) ==\n");
    check_voiced(&detector, "A2 octave trap", 110.0,
                 make_tone(110.0, frame_length(110.0), octtrap), 1.0);
    check_voiced(&detector, "E2 octave trap", 82.4069,
                 make_tone(82.4069, frame_length(82.4069), octtrap), 1.0);

    printf("== decaying note ==\n");
    check_voiced(&detector, "A2 decay tau=0.25s", 110.0,
                 make_tone_full(110.0, frame_length(110.0), guitar, 0.0, 0.25, 1e9, 1234u), 1.5);

    printf("== string inharmonicity (stretched partials) ==\n");
    // Realistic wound-bass inharmonicity: the coarse stage must stay tight.
    check_voiced(&detector, "E1 bass realistic B=6e-5", 41.2034,
                 make_tone_full(41.2034, frame_length(41.2034), bass, 6e-5, 0.0, 1e9, 1234u), 3.0);
    // Piano-grade extreme inharmonicity: octave must be right; exact cents is
    // the fine stage's job, so we only assert the note here.
    check_note_only(&detector, "E1 extreme B=4e-4", 41.2034,
                    make_tone_full(41.2034, frame_length(41.2034), bass, 4e-4, 0.0, 1e9, 1234u));

    printf("== noise robustness ==\n");
    check_voiced(&detector, "A2 + noise 20 dB", 110.0,
                 make_tone_full(110.0, frame_length(110.0), guitar, 0.0, 0.0, 20.0, 1234u),
                 5.0);
    check_voiced(&detector, "E2 + noise 12 dB", 82.4069,
                 make_tone_full(82.4069, frame_length(82.4069), guitar, 0.0, 0.0,
                                12.0, 1234u),
                 15.0);

    printf("== gating: noise only must read unvoiced ==\n");
    {
        HawkMT rng;
        hawk_mt_seed(&rng, 99u);
        HawkNormal g = hawk_normal(0.0, 1.0);
        static float noise[8192];
        for (size_t i = 0; i < 8192; ++i) noise[i] = (float)(0.1 * hawk_normal_draw(&g, &rng));
        check_unvoiced(&detector, "white noise only", noise, 8192);
    }

    hawk_pitch_detector_free(&detector);
    printf("\n%d checks, %d failed\n", checks_run, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
