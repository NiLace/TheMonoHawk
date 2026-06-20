#include "pitch_detector.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace hawk {

PitchDetector::PitchDetector(double sample_rate,
                             double peak_threshold_ratio,
                             double min_clarity,
                             double min_frequency,
                             double max_frequency)
    : sample_rate_(sample_rate)
    , peak_threshold_ratio_(peak_threshold_ratio)
    , min_clarity_(min_clarity)
    , min_frequency_(min_frequency)
    , max_frequency_(max_frequency)
{
}

PitchEstimate PitchDetector::analyze(const float* samples,
                                     std::size_t count) const
{
    PitchEstimate estimate;
    if (samples == nullptr || count < 4) {
        return estimate;
    }

    // --- 1. Normalized Square Difference Function -------------------------
    // For each lag tau, measure how well the frame matches a copy of itself
    // delayed by tau. nsdf[tau] lands in [-1, 1]; +1 means perfect periodicity
    // at that lag. We only need lags up to half the frame (one period must fit
    // twice), and we further restrict to the lag range matching our musical
    // frequency bounds -- this stops the detector ever locking onto ultrasonic
    // noise (tiny lags) or sub-audio rumble (huge lags).
    std::size_t tau_hi = static_cast<std::size_t>(sample_rate_ / min_frequency_) + 1;
    if (tau_hi > count / 2) tau_hi = count / 2;
    std::size_t tau_lo = static_cast<std::size_t>(sample_rate_ / max_frequency_);
    if (tau_lo < 1) tau_lo = 1;
    if (tau_hi <= tau_lo + 2) return estimate;

    std::vector<double> nsdf(tau_hi, 0.0);

    // The autocorrelation r[tau] = sum_j x[j]*x[j+tau] is computed in O(N log N)
    // as r = IFFT(|FFT(x)|^2) on a zero-padded frame (padding to >= 2*count makes
    // the circular correlation equal the linear one). The NSDF normalizer
    // m[tau] = sum (x[j]^2 + x[j+tau]^2) over the overlap comes from a prefix sum
    // of squares: m[tau] = P[count-tau] + (S - P[tau]). nsdf = 2r/m, exactly as
    // the old direct double-loop produced, just far cheaper.
    std::size_t m = 1;
    while (m < 2 * count) m <<= 1;
    if (!fft_ || fft_->size() != m) fft_ = std::make_unique<FFT<float>>(m);
    re_.assign(m, 0.0f);
    im_.assign(m, 0.0f);
    for (std::size_t j = 0; j < count; ++j) re_[j] = static_cast<float>(samples[j]);
    fft_->transform(re_.data(), im_.data(), /*inverse=*/false);
    for (std::size_t k = 0; k < m; ++k) {
        re_[k] = re_[k] * re_[k] + im_[k] * im_[k];   // |X|^2 (power spectrum)
        im_[k] = 0.0f;
    }
    fft_->transform(re_.data(), im_.data(), /*inverse=*/true);   // unscaled IFFT

    prefix_.assign(count + 1, 0.0);                  // P[k] = sum of first k squares
    for (std::size_t j = 0; j < count; ++j) {
        const double s = static_cast<double>(samples[j]);
        prefix_[j + 1] = prefix_[j] + s * s;
    }
    const double total_sq = prefix_[count];          // S
    const double inv_m = 1.0 / static_cast<double>(m);
    for (std::size_t tau = 0; tau < tau_hi; ++tau) {
        const double autocorrelation = re_[tau] * inv_m;
        const double normalizer = prefix_[count - tau] + (total_sq - prefix_[tau]);
        nsdf[tau] = (normalizer > 0.0) ? (2.0 * autocorrelation / normalizer) : 0.0;
    }

    // --- 2. Key maxima (one per positive hump of the NSDF) ----------------
    std::vector<std::size_t> maxima;
    std::size_t pos = 0;

    // Skip the initial lobe around tau = 0 (the trivial "self at lag 0" peak),
    // then skip the first negative region, so we start at the first real hump.
    while (pos < tau_hi - 1 && nsdf[pos] > 0.0) ++pos;
    while (pos < tau_hi - 1 && nsdf[pos] <= 0.0) ++pos;
    if (pos == 0) pos = 1;

    std::size_t current_max = 0;  // 0 doubles as "none yet" (pos is always >= 1)
    while (pos < tau_hi - 1) {
        if (nsdf[pos] > nsdf[pos - 1] && nsdf[pos] >= nsdf[pos + 1]) {
            if (current_max == 0 || nsdf[pos] > nsdf[current_max]) {
                current_max = pos;
            }
        }
        ++pos;
        // Falling below zero ends the current hump: commit its maximum and
        // skip ahead to the next positive region.
        if (pos < tau_hi - 1 && nsdf[pos] <= 0.0) {
            if (current_max > 0) {
                maxima.push_back(current_max);
                current_max = 0;
            }
            while (pos < tau_hi - 1 && nsdf[pos] <= 0.0) ++pos;
        }
    }
    if (current_max > 0) {
        maxima.push_back(current_max);
    }

    if (maxima.empty()) {
        return estimate;
    }

    // --- 3. Choose the first peak that clears the threshold ---------------
    // Selecting the *first* eligible peak (not the tallest) is what keeps us on
    // the true fundamental instead of dropping to a strong harmonic an octave
    // away.
    // Only consider peaks within the musical lag range [tau_lo, tau_hi).
    double highest = 0.0;
    for (std::size_t p : maxima) {
        if (p < tau_lo) continue;
        highest = std::max(highest, nsdf[p]);
    }
    const double threshold = peak_threshold_ratio_ * highest;

    std::size_t chosen = 0;
    bool found = false;
    for (std::size_t p : maxima) {
        if (p < tau_lo) continue;
        if (nsdf[p] >= threshold) {
            chosen = p;
            found  = true;
            break;
        }
    }
    if (!found) {
        return estimate;
    }

    // --- 4. Parabolic interpolation for sub-sample precision --------------
    const double y0 = nsdf[chosen - 1];
    const double y1 = nsdf[chosen];
    const double y2 = nsdf[chosen + 1];
    const double a  = (y0 + y2 - 2.0 * y1) / 2.0;  // curvature
    const double b  = (y2 - y0) / 2.0;             // slope

    double tau_peak = static_cast<double>(chosen);
    double clarity  = y1;
    if (a < 0.0) {  // concave -> a genuine maximum we can refine
        const double offset = -b / (2.0 * a);
        tau_peak = static_cast<double>(chosen) + offset;
        clarity  = y1 - (b * b) / (4.0 * a);
    }

    if (tau_peak <= 0.0) {
        return estimate;
    }

    estimate.frequency = sample_rate_ / tau_peak;
    estimate.clarity   = std::min(clarity, 1.0);
    estimate.voiced    = estimate.clarity >= min_clarity_;
    return estimate;
}

} // namespace hawk
