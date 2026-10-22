/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) The NL Sounds contributors */
#pragma once

#include <cstddef>
#include <vector>

namespace hawk {

// Result of the fine stage: a sub-cent refinement around a known target note,
// plus the strobe phase that drives the display.
struct FineEstimate {
    double frequency = 0.0;  // refined fundamental frequency (Hz)
    double cents     = 0.0;  // deviation from target_frequency, in cents
    double phase     = 0.0;  // strobe phase at end of frame, radians (-pi, pi]
    double coherence = 0.0;  // how steadily the fundamental turned, 0..1 (see refine())
    bool   valid     = false;
};

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
// analyze()/refine() are marked const and described as pure functions of their input, and that is
// true of the RESULT -- but they write through a mutable workspace, so two threads sharing one
// instance would tear each other's scratch buffers apart. The const is about the logical value, not
// about thread safety, and the two are easy to confuse in exactly this shape.
//
// Every per-call vector lives in the object (no heap traffic per analysis), so there is substantial
// mutable state behind that const.
// The plugin uses a single instance on a single worker thread, which is fine; anything else needs
// its own instance (or thread_local).
class FineTuner {
public:
    explicit FineTuner(double sample_rate);

    // Refine pitch near target_frequency. Pure function of its input.
    FineEstimate refine(const float* samples,
                        std::size_t count,
                        double target_frequency) const;

private:
    double sample_rate_;

    // Reusable workspace. refine() runs ~23 times a second for the life of the plugin, and used to
    // allocate and free eight vectors of up to 16384 doubles on every call -- measured at 9
    // allocations and 800 KB per analysis cycle, i.e. 207 allocations/s and 18.4 MB/s of churn on
    // a worker thread that never stops. These are members so that steady state allocates nothing.
    // Mutable for the same reason the detector's workspace is: a cache does not change the logical
    // (pure-function) result of refine().
    mutable std::vector<double> re_, im_, sre_, sim_;
};

} // namespace hawk
