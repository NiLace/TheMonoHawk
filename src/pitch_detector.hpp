#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "fft.hpp"

namespace hawk {

// Result of analysing one frame of audio for its fundamental pitch.
struct PitchEstimate {
    double frequency = 0.0;  // estimated fundamental (Hz), 0 if none found
    double clarity   = 0.0;  // 0..1, how periodic the frame is (NSDF peak)
    bool   voiced    = false;// true when a confident pitch was found
};

// Monophonic fundamental-frequency estimator using the McLeod Pitch Method
// (MPM): a normalized autocorrelation (NSDF) with first-peak-above-threshold
// selection to resist octave errors, plus parabolic interpolation for
// sub-sample (hence sub-cent) precision.
//
// This is the "coarse stage" of the detection core described in design-spec.md
// §3.1. The implementation favours clarity over speed; the O(N^2) NSDF will be
// replaced by an FFT-based autocorrelation when real-time cost matters.
class PitchDetector {
public:
    // min_frequency / max_frequency bound the search to a musical range, so the
    // detector can never lock onto sub-audio rumble or ultrasonic noise. The
    // defaults span A0 to roughly the top of a guitar's range.
    explicit PitchDetector(double sample_rate,
                           double peak_threshold_ratio = 0.8,
                           double min_clarity          = 0.85,
                           double min_frequency        = 27.5,
                           double max_frequency        = 1500.0);

    // Analyse one frame of mono float samples. Pure function of its input.
    PitchEstimate analyze(const float* samples, std::size_t count) const;

private:
    double sample_rate_;
    double peak_threshold_ratio_;  // fraction of the best peak a candidate must
                                   // reach to be eligible (octave-error guard)
    double min_clarity_;           // clarity below this is reported as unvoiced
    double min_frequency_;         // lowest fundamental we will report
    double max_frequency_;         // highest fundamental we will report

    // FFT workspace for the O(N log N) autocorrelation (lazily sized to the
    // padded frame on first use; reused thereafter). Mutable: a per-call cache
    // that doesn't change the logical (pure-function) result of analyze().
    mutable std::unique_ptr<FFT<float>> fft_;
    mutable std::vector<float>   re_, im_;
    mutable std::vector<double>  prefix_;
};

} // namespace hawk
