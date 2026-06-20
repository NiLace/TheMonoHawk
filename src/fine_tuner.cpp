#include "fine_tuner.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace hawk {

namespace {

constexpr double two_pi = 6.28318530717958647692;

// In-place moving average of length `window` over a complex sequence, computed
// with a running sum. Used as a low-pass; cascading it twice deepens rejection
// at the target's partials, which is where the unwanted energy sits.
void moving_average(std::vector<double>& real,
                    std::vector<double>& imag,
                    std::size_t window)
{
    double sum_real = 0.0;
    double sum_imag = 0.0;
    std::vector<double> out_real(real.size());
    std::vector<double> out_imag(imag.size());
    for (std::size_t n = 0; n < real.size(); ++n) {
        sum_real += real[n];
        sum_imag += imag[n];
        if (n >= window) {
            sum_real -= real[n - window];
            sum_imag -= imag[n - window];
        }
        const double len = static_cast<double>(std::min(n + 1, window));
        out_real[n] = sum_real / len;
        out_imag[n] = sum_imag / len;
    }
    real.swap(out_real);
    imag.swap(out_imag);
}

} // namespace

FineTuner::FineTuner(double sample_rate)
    : sample_rate_(sample_rate)
{
}

FineEstimate FineTuner::refine(const float* samples,
                               std::size_t count,
                               double target_frequency) const
{
    FineEstimate estimate;
    if (samples == nullptr || target_frequency <= 0.0 || count < 8) {
        return estimate;
    }

    // Reference angular frequency. Multiplying the real signal by e^{-j w0 n}
    // moves the fundamental (near w0) down to near DC.
    const double w0 = two_pi * target_frequency / sample_rate_;

    // Low-pass = moving average whose length is one period of the target. Its
    // nulls fall exactly on the target frequency and its multiples, which is
    // where every unwanted term lands (other partials, and the -2*w0 image),
    // while the near-DC fundamental deviation passes through.
    const std::size_t window = static_cast<std::size_t>(
        std::lround(sample_rate_ / target_frequency));
    if (window < 1 || count <= window) {
        return estimate;
    }

    // Two moving averages cannot fit in the frame if the window is too long.
    if (count <= 2 * window) {
        return estimate;
    }

    // Heterodyne: z[n] = x[n] * e^{-j w0 n}.
    std::vector<double> filt_real(count);
    std::vector<double> filt_imag(count);
    for (std::size_t n = 0; n < count; ++n) {
        const double angle = w0 * static_cast<double>(n);
        const double x     = samples[n];
        filt_real[n] = x * std::cos(angle);
        filt_imag[n] = x * -std::sin(angle);
    }

    // Cascaded moving-average low-pass: two passes deepen rejection of the
    // partials and the -2*w0 image, leaving only the near-DC fundamental.
    moving_average(filt_real, filt_imag, window);
    moving_average(filt_real, filt_imag, window);

    // Frequency discriminator (Kay's estimator): the mean per-sample phase
    // advance of the filtered signal is the residual frequency. Averaging the
    // complex products z[n] * conj(z[n-1]) before taking the angle is robust to
    // noise. Start past the two filters' combined warm-up region.
    double prod_real = 0.0;
    double prod_imag = 0.0;
    for (std::size_t n = 2 * window; n < count; ++n) {
        const double ar = filt_real[n];
        const double ai = filt_imag[n];
        const double br = filt_real[n - 1];
        const double bi = filt_imag[n - 1];
        prod_real += ar * br + ai * bi;   // real( z[n] * conj(z[n-1]) )
        prod_imag += ai * br - ar * bi;   // imag( z[n] * conj(z[n-1]) )
    }

    const double delta_phase   = std::atan2(prod_imag, prod_real);
    const double freq_offset   = delta_phase * sample_rate_ / two_pi;
    const double frequency     = target_frequency + freq_offset;

    if (frequency <= 0.0) {
        return estimate;
    }

    estimate.frequency = frequency;
    estimate.cents     = 1200.0 * std::log2(frequency / target_frequency);
    estimate.phase     = std::atan2(filt_imag[count - 1], filt_real[count - 1]);
    estimate.valid     = true;
    return estimate;
}

} // namespace hawk
