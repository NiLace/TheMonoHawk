/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) The NL Sounds contributors */
#include "fine_tuner.hpp"
#include "math_constants.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace hawk {

namespace {

constexpr double two_pi = TWO_PI;

// Moving average of length `window` over a complex sequence, computed with a running sum. Used as
// a low-pass; cascading it twice deepens rejection at the target's partials, which is where the
// unwanted energy sits.
//
// The output buffers are passed IN rather than allocated here: this runs twice per refine(), and
// refine() runs ~23 times a second forever. It cannot be done in place -- the running sum needs
// the untouched input at n - window.
void moving_average(const std::vector<double>& real, const std::vector<double>& imag,
                    std::vector<double>& out_real, std::vector<double>& out_imag,
                    std::size_t window, std::size_t count)
{
    double sum_real = 0.0;
    double sum_imag = 0.0;
    for (std::size_t n = 0; n < count; ++n) {
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
    //
    // The rotation is stepped INCREMENTALLY (c += ... one complex multiply per sample) instead of
    // calling cos and sin per sample. Two transcendentals per sample over a 16384-sample frame,
    // 23 times a second, was measured at ~45 % of refine().
    // ⚠ An incremental rotation drifts in magnitude, and the drift is what makes the naive version
    // wrong rather than merely imprecise: |z| creeps away from 1 and the amplitude envelope tilts
    // across the frame. It is renormalised every RENORM samples, which is cheap (one sqrt per 256
    // samples) and bounds the error far below what a cent needs.
    // sized only: the loop below writes every re_/im_ entry, and moving_average every output entry
    re_.resize(count); im_.resize(count);
    sre_.resize(count); sim_.resize(count);
    {
        constexpr std::size_t RENORM = 256;
        const double cs = std::cos(w0), sn = std::sin(w0);
        double zr = 1.0, zi = 0.0;                    // e^{-j w0 n}, n = 0
        for (std::size_t n = 0; n < count; ++n) {
            const double x = samples[n];
            re_[n] = x * zr;
            im_[n] = x * zi;
            const double nzr = zr * cs + zi * sn;     // multiply by e^{-j w0}
            const double nzi = zi * cs - zr * sn;
            zr = nzr; zi = nzi;
            if (((n + 1) % RENORM) == 0) {
                const double inv = 1.0 / std::sqrt(zr * zr + zi * zi);
                zr *= inv; zi *= inv;
            }
        }
    }

    // Cascaded moving-average low-pass: two passes deepen rejection of the
    // partials and the -2*w0 image, leaving only the near-DC fundamental.
    moving_average(re_, im_, sre_, sim_, window, count);
    moving_average(sre_, sim_, re_, im_, window, count);
    std::vector<double>& filt_real = re_;
    std::vector<double>& filt_imag = im_;

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

    // The fine stage's own confidence. Did the filtered fundamental turn by the SAME angle
    // over one period all through the frame?
    //   coherence = |sum z[n] conj(z[n-L])| / sum |z[n]| |z[n-L]|,   L = one period (the filter length)
    // 1 for one steady fundamental, low when the turns point every which way. The lag is one period,
    // not one sample, on purpose: after the two moving averages consecutive samples are nearly equal,
    // so at lag 1 even white noise scored 0.9995 (measured) -- the proposal as written could not tell
    // a note from nothing. At one period: clean notes 1.0000, a note in white noise down to 0 dB SNR
    // >= 0.994, white noise alone 0.04-0.87 (median 0.40), a strummed chord ~0.85, 9 Hz rumble
    // 0.82-0.99. Two or more periods do not fit the frame for the lowest notes.
    {
        const std::size_t L = window;
        double pr = 0.0, pi = 0.0, pm = 0.0;
        for (std::size_t n = 2 * window + L; n < count; ++n) {
            const double ar = filt_real[n], ai = filt_imag[n];
            const double br = filt_real[n - L], bi = filt_imag[n - L];
            pr += ar * br + ai * bi;
            pi += ai * br - ar * bi;
            pm += std::sqrt((ar * ar + ai * ai) * (br * br + bi * bi));
        }
        // a frame too short to hold the lag cannot judge: 1 means "no veto", not "certainly a note"
        estimate.coherence = (2 * window + L >= count) ? 1.0
                           : (pm > 0.0) ? std::sqrt(pr * pr + pi * pi) / pm : 0.0;
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

    // Sanity bound on the refinement, and it is a bound rather than a calibration.
    //
    // This stage heterodynes against `target_frequency`, which the caller derives from the COARSE
    // estimate. If the coarse stage picked the wrong octave, the heterodyne locks onto the wrong
    // thing and returns a confident, precise, completely wrong reading -- measured at -345, +248
    // and -100 cents for an octave up, an octave down and a semitone up, all with valid = true.
    //
    // Why 60 cents is safe and not arbitrary: the caller snaps the coarse estimate to the NEAREST
    // note, so an honest deviation cannot exceed +-50 cents by construction. Measured across 15
    // legitimate detunings from -49 to +49 cents, this guard fires ZERO times and the worst
    // legitimate reading is 49.08 cents -- about 11 cents of headroom.
    //
    // ⚠ It is deliberately NOT calibrated on the magnitude of the error, because that magnitude is
    // not reproducible: it depends on the partial structure of the note being played. It is only
    // ever asked "is this further out than a correctly-tracked string could possibly be?".
    //
    // On false, the caller falls back to the coarse stage's own cents -- less precise, but right.
    estimate.valid     = std::fabs(estimate.cents) <= 60.0;
    return estimate;
}

} // namespace hawk
