/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) The NL Sounds contributors */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "pitch_detector.h"   // HawkBuf


// Result of the fine stage: a sub-cent refinement around a known target note,
// plus the strobe phase that drives the display.
typedef struct FineEstimate {
    double frequency;  // refined fundamental frequency (Hz)
    double cents;      // deviation from target_frequency, in cents
    double phase;      // strobe phase at end of frame, radians (-pi, pi]
    double coherence;  // how steadily the fundamental turned, 0..1 (see refine())
    bool   valid;
} FineEstimate;

// Fine pitch refinement by heterodyne phase tracking.
//
// Given a target frequency (the fundamental of the note the coarse stage chose),
// this shifts the signal so the fundamental sits near DC, low-passes away the
// other partials and images, and measures the residual phase rotation. The rate
// of rotation is the frequency offset (hence cents); the rotation angle is the
// strobe position. Because it tracks only the fundamental, it is immune to the
// upper-partial pull that biases the coarse stage under inharmonicity.
// ⚠ ONE INSTANCE PER THREAD.
//
// refine() takes a const tuner and is a pure function of its input, and that is true of the RESULT -- but it
// writes through the tuner's workspace, so two threads sharing one instance would tear each other's scratch
// buffers apart. The const is about the logical value, not about thread safety.
//
// Every per-call buffer lives in the object (no heap traffic per analysis), so there is substantial
// mutable state behind that const.
// The plugin uses a single instance on a single worker thread, which is fine; anything else needs
// its own instance.
typedef struct FineTuner {
    double sample_rate_;

    // Reusable workspace. refine() runs ~23 times a second for the life of the plugin, and used to
    // allocate and free eight vectors of up to 16384 doubles on every call -- measured at 9
    // allocations and 800 KB per analysis cycle, i.e. 207 allocations/s and 18.4 MB/s of churn on
    // a worker thread that never stops. These are members so that steady state allocates nothing.
    // Written through a const tuner for the same reason the detector's workspace is: a cache does
    // not change the logical (pure-function) result of refine().
    HawkBuf re_, im_, sre_, sim_;   /* double each */
} FineTuner;

void hawk_fine_tuner_init(FineTuner* t, double sample_rate);
void hawk_fine_tuner_free(FineTuner* t);

// Refine pitch near target_frequency. Pure function of its input (see above for the workspace).
FineEstimate hawk_fine_tuner_refine(const FineTuner* t, const float* samples, size_t count, double target_frequency);
