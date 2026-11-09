// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
// Test harness for the fine heterodyne stage, exercised through the real
// pipeline: coarse detector picks the note, the note's exact frequency becomes
// the heterodyne target, the fine stage reports sub-cent deviation.
//
// The headline case is inharmonicity: the coarse stage read the piano-grade
// E1 string +10.67 cents sharp (pulled by stretched partials); the fine stage,
// tracking only the fundamental, should recover its true ~+0.35 cent offset.

#include "fine_tuner.h"
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

// A harmonic stack: its amplitudes and how many there are.
typedef struct Stack { const double* amps; size_t n; } Stack;
#define STACK(a) ((Stack){ (a), sizeof(a) / sizeof((a)[0]) })

// A frame of samples (malloc'd; free it with free(f.x)).
typedef struct Frame { float* x; size_t n; } Frame;

// snr_db >= 200: no noise.
static Frame make_tone_full(double f0, size_t count, Stack amps,
                            double inharmonicity_b, double snr_db, uint32_t seed)
{
    Frame b = { (float*)calloc(count, sizeof(float)), count };
    HawkMT rng;
    hawk_mt_seed(&rng, seed);
    HawkNormal gaussian = hawk_normal(0.0, 1.0);

    double sum_sq = 0.0;
    for (size_t h = 0; h < amps.n; ++h) sum_sq += amps.amps[h] * amps.amps[h];
    const double sig_rms   = sqrt(sum_sq / 2.0);
    const double noise_amp = (snr_db < 200.0)
        ? sig_rms / pow(10.0, snr_db / 20.0) : 0.0;

    for (size_t i = 0; i < count; ++i) {
        const double t = (double)i / sample_rate;
        double sample = 0.0;
        for (size_t h = 0; h < amps.n; ++h) {
            const int n  = (int)h + 1;
            double    fn = n * f0;
            if (inharmonicity_b > 0.0)
                fn = n * f0 * sqrt(1.0 + inharmonicity_b * n * n);
            sample += amps.amps[h] * sin(2.0 * pi * fn * t);
        }
        if (noise_amp > 0.0) sample += noise_amp * hawk_normal_draw(&gaussian, &rng);
        b.x[i] = (float)(0.3 * sample);
    }
    return b;
}

static Frame make_tone(double f0, size_t count, Stack amps, double inharmonicity_b)
{
    return make_tone_full(f0, count, amps, inharmonicity_b, 1e9, 7u);
}

// Run coarse -> target note -> fine, and check the fine cents against an
// expected value. `label` is for the printout. Frees the samples.
static void check_fine(const PitchDetector* coarse,
                       const FineTuner* fine,
                       const char* label,
                       double f0,
                       Frame samples,
                       double expected_cents,
                       double tol_cents)
{
    ++checks_run;
    const PitchEstimate c = hawk_pitch_detector_analyze(coarse, samples.x, samples.n);
    const int    note   = hawk_frequency_to_note(c.frequency, 440.0).midi_note;
    const double target = hawk_note_to_frequency(note, 440.0);
    const FineEstimate f = hawk_fine_tuner_refine(fine, samples.x, samples.n, target);
    free(samples.x);

    const int ok = f.valid &&
                   fabs(f.cents - expected_cents) <= tol_cents;
    if (!ok) ++checks_failed;

    char name[HAWK_NOTE_NAME_LEN];
    printf("  [%s] %-26s target %-4s  fine %+7.3fc "
           "(want %+6.2f +/- %.2f)  f=%9.3f Hz\n",
           ok ? "PASS" : "FAIL", label,
           hawk_note_name(note, name), f.cents,
           expected_cents, tol_cents, f.frequency);
    (void)f0;
}

// A frequency `cents` away from f0.
static double detune(double f0, double cents)
{
    return f0 * pow(2.0, cents / 1200.0);
}

int main(void)
{
    PitchDetector coarse;
    hawk_pitch_detector_init(&coarse, sample_rate, HAWK_PD_DEFAULTS);
    FineTuner fine;
    hawk_fine_tuner_init(&fine, sample_rate);

    static const double sine_a[]   = {1.0};
    static const double guitar_a[] = {1.0, 0.6, 0.45, 0.3, 0.22, 0.16};
    static const double bass_a[]   = {1.0, 0.85, 0.7, 0.55, 0.45, 0.35,
                                      0.28, 0.22, 0.18};
    const Stack sine   = STACK(sine_a);
    const Stack guitar = STACK(guitar_a);
    const Stack bass   = STACK(bass_a);

    printf("== pure tones, sub-cent recovery ==\n");
    check_fine(&coarse, &fine, "A2 in tune", 110.0,
               make_tone(110.0, frame_length(110.0), sine, 0.0), 0.0, 0.10);
    check_fine(&coarse, &fine, "A4 +15c", 440.0,
               make_tone(detune(440.0, 15.0), frame_length(440.0), sine, 0.0),
               15.0, 0.10);
    check_fine(&coarse, &fine, "E2 -7c", 82.4069,
               make_tone(detune(82.4069, -7.0), frame_length(82.4069), sine, 0.0),
               -7.0, 0.10);

    printf("== harmonic-rich, detuned ==\n");
    check_fine(&coarse, &fine, "A2 guitar +10c", 110.0,
               make_tone(detune(110.0, 10.0), frame_length(110.0), guitar, 0.0),
               10.0, 0.30);

    printf("== inharmonicity: the headline ==\n");
    // True fundamental partial of a B=4e-4 string sits at f0*sqrt(1+B) ~ +0.35c.
    // The coarse stage read this +10.67c; the fine stage should find ~+0.35c.
    check_fine(&coarse, &fine, "E1 extreme B=4e-4", 41.2034,
               make_tone(41.2034, frame_length(41.2034), bass, 4e-4),
               0.346, 0.50);
    check_fine(&coarse, &fine, "E1 realistic B=6e-5", 41.2034,
               make_tone(41.2034, frame_length(41.2034), bass, 6e-5),
               0.052, 0.30);

    printf("== noise robustness ==\n");
    check_fine(&coarse, &fine, "A2 guitar +5c, 20 dB", 110.0,
               make_tone_full(detune(110.0, 5.0), frame_length(110.0), guitar,
                              /*inharmonicity*/ 0.0, /*snr_db*/ 20.0, 7u),
               5.0, 2.0);

    printf("== strobe direction (sign of offset) ==\n");
    {
        ++checks_run;
        Frame sharp = make_tone(detune(220.0, 8.0), frame_length(220.0), sine, 0.0);
        Frame flat  = make_tone(detune(220.0, -8.0), frame_length(220.0), sine, 0.0);
        double tgt = hawk_note_to_frequency(
            hawk_frequency_to_note(
                hawk_pitch_detector_analyze(&coarse, sharp.x, sharp.n).frequency, 440.0).midi_note, 440.0);
        FineEstimate fs = hawk_fine_tuner_refine(&fine, sharp.x, sharp.n, tgt);
        FineEstimate ff = hawk_fine_tuner_refine(&fine, flat.x,  flat.n,  tgt);
        free(sharp.x);
        free(flat.x);
        const int ok = fs.valid && ff.valid &&
                       fs.cents > 0.0 && ff.cents < 0.0;
        if (!ok) ++checks_failed;
        printf("  [%s] sharp -> %+0.2fc (>0), flat -> %+0.2fc (<0)\n",
               ok ? "PASS" : "FAIL", fs.cents, ff.cents);
    }

    // ---- the octave trap, and the guard against it ------------------------
    // Handed a target an octave (or a semitone) away from what is actually playing, the
    // heterodyne locks onto the wrong thing and returns a confident, precise, wrong reading.
    // The guard must reject those. Note the returned magnitudes are NOT stable across material,
    // so the test asserts only that the reading is REJECTED, never a particular value.
    printf("\n== fine stage must not trust a wrong target ==\n");
    {
        static const double partials_a[] = { 1.0, 0.5, 0.3, 0.15 };
        const Stack partials = STACK(partials_a);
        Frame tone = make_tone(82.407, frame_length(82.407), partials, 0.0);
        static const struct { const char* nm; double target; } bad[] = {
            { "octave up",   164.814 },
            { "octave down",  41.203 },
            { "semitone up",  87.307 },
        };
        for (size_t k = 0; k < sizeof bad / sizeof bad[0]; ++k) {
            ++checks_run;
            FineEstimate f = hawk_fine_tuner_refine(&fine, tone.x, tone.n, bad[k].target);
            const int ok = !f.valid;
            if (!ok) ++checks_failed;
            printf("  [%s] %-13s target -> %+8.2fc, valid=%d (want 0)\n",
                   ok ? "PASS" : "FAIL", bad[k].nm, f.cents, f.valid ? 1 : 0);
        }
        free(tone.x);
        // ...and the other half, which matters just as much: a tuner exists to read strings that
        // are OUT of tune. The coarse stage snaps to the nearest note, so an honest deviation is
        // bounded by +-50 cents. If the guard fired on any of these it would break the product.
        ++checks_run;
        int spurious = 0, skipped = 0;
        double worst = 0.0;
        for (double dc = -49.0; dc <= 49.5; dc += 7.0) {
            Frame x = make_tone(detune(82.407, dc), frame_length(82.407), partials, 0.0);
            PitchEstimate c = hawk_pitch_detector_analyze(&coarse, x.x, x.n);
            if (c.frequency <= 0.0) { ++skipped; free(x.x); continue; }   // a deaf coarse stage must not pass this arm
            double tgt = hawk_note_to_frequency(
                hawk_frequency_to_note(c.frequency, 440.0).midi_note, 440.0);
            FineEstimate f = hawk_fine_tuner_refine(&fine, x.x, x.n, tgt);
            free(x.x);
            if (!f.valid) ++spurious;
            if (fabs(f.cents) > worst) worst = fabs(f.cents);
        }
        const int ok = (spurious == 0 && skipped == 0);
        if (!ok) ++checks_failed;
        printf("  [%s] legitimate detunings -49..+49c: %d rejected, %d not detected (want 0, 0), "
               "worst reading %.2fc vs the 60c guard\n",
               ok ? "PASS" : "FAIL", spurious, skipped, worst);
    }

    hawk_fine_tuner_free(&fine);
    hawk_pitch_detector_free(&coarse);
    printf("\n%d checks, %d failed\n", checks_run, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
