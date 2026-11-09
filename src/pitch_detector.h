/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) The NL Sounds contributors */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "fft.h"


// Result of analysing one frame of audio for its fundamental pitch.
typedef struct PitchEstimate {
    double frequency;  // estimated fundamental (Hz), 0 if none found
    double clarity;    // 0..1, how periodic the frame is (NSDF peak)
    bool   voiced;     // true when a confident pitch was found
} PitchEstimate;

// Monophonic fundamental-frequency estimator using the McLeod Pitch Method
// (MPM): a normalized autocorrelation (NSDF) with first-peak-above-threshold
// selection to resist octave errors, plus parabolic interpolation for
// sub-sample (hence sub-cent) precision.
//
// This is the "coarse stage" of the detection core. The NSDF is computed in O(N log N) via an
// FFT-based autocorrelation -- see the fft_ workspace below and the derivation in analyze().
// ⚠ ONE INSTANCE PER THREAD.
//
// analyze() takes a const detector and is a pure function of its input, and that is true of the RESULT -- but it
// writes through the detector's workspace, so two threads sharing one instance would tear each other's scratch
// buffers apart. The const is about the logical value, not about thread safety.
//
// Every per-call buffer lives in the object (no heap traffic per analysis), so there is substantial
// mutable state behind that const.
// The plugin uses a single instance on a single worker thread, which is fine; anything else needs
// its own instance.
// A workspace buffer that only grows (the capacity is kept, so steady state allocates nothing). Mutable through a const detector: a per-call cache.
typedef struct HawkBuf { void* p; size_t cap; } HawkBuf;

typedef struct PitchDetector {
    double sample_rate_;
    double peak_threshold_ratio_;  // fraction of the best peak a candidate must
                                   // reach to be eligible (octave-error guard)
    double min_clarity_;           // clarity below this is reported as unvoiced
    double min_frequency_;         // lowest fundamental we will report
    double max_frequency_;         // highest fundamental we will report

    // FFT workspace for the O(N log N) autocorrelation (lazily sized to the
    // padded frame on first use; reused thereafter): a per-call cache that doesn't
    // change the logical (pure-function) result of analyze().
    RealFFTf fft_;
    bool     fft_ready_;
    HawkBuf  re_;        /* float: padded frame, then reused for the power spectrum */
    HawkBuf  xr_, xi_;   /* float: half spectrum, n/2 + 1 bins */
    HawkBuf  prefix_;    /* double */
    // High-passed copy of the frame. The NSDF is computed from THIS, never from the caller's
    // buffer -- see the rationale in analyze().
    HawkBuf  hp_;        /* float */
    HawkBuf  nsdf_;      /* double: hoisted with the rest, see fine_tuner.h */
    HawkBuf  maxima_;    /* size_t */
} PitchDetector;

// min_frequency / max_frequency bound the search to a musical range, so the
// detector can never lock onto sub-audio rumble or ultrasonic noise. The
// usual values (A0 to roughly the top of a guitar's range) are HAWK_PD_DEFAULTS.
#define HAWK_PD_DEFAULTS 0.8, 0.85, 27.5, 1500.0
void hawk_pitch_detector_init(PitchDetector* d, double sample_rate,
                              double peak_threshold_ratio, double min_clarity,
                              double min_frequency, double max_frequency);
void hawk_pitch_detector_free(PitchDetector* d);

// Analyse one frame of mono float samples. Pure function of its input (see above for the workspace).
PitchEstimate hawk_pitch_detector_analyze(const PitchDetector* d, const float* samples, size_t count);

// Grows a workspace buffer to hold n elements of `size` bytes; aborts on allocation failure (the worker has no
// way to carry on without its workspace).
void* hawk_buf_reserve(HawkBuf* b, size_t n, size_t size);
