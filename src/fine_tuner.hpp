#pragma once

#include <cstddef>

namespace hawk {

// Result of the fine stage: a sub-cent refinement around a known target note,
// plus the strobe phase that drives the display.
struct FineEstimate {
    double frequency = 0.0;  // refined fundamental frequency (Hz)
    double cents     = 0.0;  // deviation from target_frequency, in cents
    double phase     = 0.0;  // strobe phase at end of frame, radians (-pi, pi]
    bool   valid     = false;
};

// Fine pitch refinement by heterodyne phase tracking (design-spec.md §3.2).
//
// Given a target frequency (the fundamental of the note the coarse stage chose),
// this shifts the signal so the fundamental sits near DC, low-passes away the
// other partials and images, and measures the residual phase rotation. The rate
// of rotation is the frequency offset (hence cents); the rotation angle is the
// strobe position. Because it tracks only the fundamental, it is immune to the
// upper-partial pull that biases the coarse stage under inharmonicity.
class FineTuner {
public:
    explicit FineTuner(double sample_rate);

    // Refine pitch near target_frequency. Pure function of its input.
    FineEstimate refine(const float* samples,
                        std::size_t count,
                        double target_frequency) const;

private:
    double sample_rate_;
};

} // namespace hawk
