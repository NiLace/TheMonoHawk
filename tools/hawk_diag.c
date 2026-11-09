// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
// Offline diagnostic: run the real detector across a recorded signal and
// dissect it, so we can see why the cents reading wanders / reads sharp.
//
// Input: raw 32-bit float mono at 48 kHz on argv[1].

#include "fine_tuner.h"
#include "note_mapping.h"
#include "pitch_detector.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const double pi = 3.14159265358979323846;
static const double sr = 48000.0;

// Magnitude of the DFT at one frequency over a window (Goertzel-style).
static double mag_at(const float* x, size_t n, double f)
{
    double re = 0.0, im = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double a = 2.0 * pi * f * (double)i / sr;
        re += x[i] * cos(a);
        im -= x[i] * sin(a);
    }
    return sqrt(re * re + im * im) * 2.0 / (double)n;
}

// Find the spectral peak near `f_guess` (+/- 4%), returning its frequency.
static double refine_peak(const float* x, size_t n, double f_guess, double* amp)
{
    double best_f = f_guess, best_m = -1.0;
    for (int k = -80; k <= 80; ++k) {
        const double f = f_guess * (1.0 + 0.0005 * k);
        const double m = mag_at(x, n, f);
        if (m > best_m) { best_m = m; best_f = f; }
    }
    if (amp) *amp = best_m;
    return best_f;
}

int main(int argc, char** argv)
{
    const char* path = (argc > 1) ? argv[1] : "/tmp/D.f32";
    FILE* fp = fopen(path, "rb");
    if (!fp) { printf("cannot open %s\n", path); return 1; }
    float* x = NULL;
    size_t x_size = 0, x_cap = 0;
    float buf[4096];
    size_t got;
    while ((got = fread(buf, sizeof(float), 4096, fp)) > 0) {
        if (x_size + got > x_cap) {
            x_cap = (x_size + got) * 2;
            float* grown = (float*)realloc(x, x_cap * sizeof(float));
            if (!grown) { free(x); fclose(fp); printf("out of memory\n"); return 1; }
            x = grown;
        }
        memcpy(x + x_size, buf, got * sizeof(float));
        x_size += got;
    }
    fclose(fp);
    printf("loaded %zu samples (%.2f s)\n", x_size, x_size / sr);

    PitchDetector coarse;
    hawk_pitch_detector_init(&coarse, sr, HAWK_PD_DEFAULTS);
    FineTuner fine;
    hawk_fine_tuner_init(&fine, sr);
    const size_t WIN = 8192, HOP = 2048;
    char name[HAWK_NOTE_NAME_LEN];

    // Frame-by-frame sweep: quantify bias and jitter of coarse vs fine cents.
    printf("\n  t(s)   note  coarseHz  coarse_c  fine_c  clarity\n");
    double sum_fine = 0, sum_fine2 = 0, sum_coarse = 0;
    int    voiced = 0, printed = 0;
    size_t best_off = 0; double best_rms = 0;
    for (size_t off = 0; off + WIN <= x_size; off += HOP) {
        double rms = 0; for (size_t i = 0; i < WIN; ++i) rms += x[off+i]*x[off+i];
        rms = sqrt(rms / WIN);
        if (rms > best_rms) { best_rms = rms; best_off = off; }

        PitchEstimate c = hawk_pitch_detector_analyze(&coarse, &x[off], WIN);
        if (!c.voiced) continue;
        NoteReading nr = hawk_frequency_to_note(c.frequency, 440.0);
        double target = hawk_note_to_frequency(nr.midi_note, 440.0);
        FineEstimate f = hawk_fine_tuner_refine(&fine, &x[off], WIN, target);
        if (!f.valid) continue;
        ++voiced;
        sum_fine += f.cents; sum_fine2 += f.cents * f.cents;
        sum_coarse += nr.cents;
        if ((voiced % 12) == 1 && printed < 24) {  // sample ~every 0.5 s
            printf("  %5.2f  %-4s  %8.3f  %+7.2f  %+6.2f  %.3f\n",
                   off / sr, hawk_note_name(nr.midi_note, name),
                   c.frequency, nr.cents, f.cents, c.clarity);
            ++printed;
        }
    }
    if (voiced) {
        double mean = sum_fine / voiced;
        double var  = sum_fine2 / voiced - mean * mean;
        printf("\nover %d voiced frames:\n", voiced);
        printf("  fine   cents: mean %+.2f, stddev %.2f\n", mean,
               sqrt(var > 0 ? var : 0));
        printf("  coarse cents: mean %+.2f\n", sum_coarse / voiced);
    }

    // Harmonic dissection on the strongest window.
    printf("\n=== harmonic structure at t=%.2f s (strongest) ===\n",
           best_off / sr);
    PitchEstimate c = hawk_pitch_detector_analyze(&coarse, &x[best_off], WIN);
    NoteReading nr = hawk_frequency_to_note(c.frequency, 440.0);
    double f0 = hawk_note_to_frequency(nr.midi_note, 440.0);
    printf("detected note %s, ET f0 = %.3f Hz\n",
           hawk_note_name(nr.midi_note, name), f0);
    printf("  n   freq(Hz)   amp     amp/A1   f_n/n(Hz)  stretch(cents)\n");
    double a1 = 0, f1n = 0;
    for (int n = 1; n <= 9; ++n) {
        double amp = 0;
        double fn = refine_peak(&x[best_off], WIN, f0 * n, &amp);
        if (n == 1) { a1 = amp > 0 ? amp : 1e-9; f1n = fn; }
        double implied = fn / n;
        double stretch = 1200.0 * log2(implied / f1n);
        printf("  %d  %9.3f  %.4f  %6.2f   %9.3f  %+7.2f\n",
               n, fn, amp, amp / a1, implied, stretch);
    }
    hawk_fine_tuner_free(&fine);
    hawk_pitch_detector_free(&coarse);
    free(x);
    return 0;
}
