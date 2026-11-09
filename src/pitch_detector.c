/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) The NL Sounds contributors */
#include "pitch_detector.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void* hawk_buf_reserve(HawkBuf* b, size_t n, size_t size)
{
    if (n > b->cap) {
        void* p = realloc(b->p, (n ? n : 1) * size);
        if (!p) { fputs("the hawk: out of memory\n", stderr); abort(); }
        b->p = p; b->cap = n;
    }
    return b->p;
}

static void hawk_buf_free(HawkBuf* b) { free(b->p); b->p = NULL; b->cap = 0; }


void hawk_pitch_detector_init(PitchDetector* d, double sample_rate,
                              double peak_threshold_ratio, double min_clarity,
                              double min_frequency, double max_frequency)
{
    memset(d, 0, sizeof *d);
    d->sample_rate_          = sample_rate;
    d->peak_threshold_ratio_ = peak_threshold_ratio;
    d->min_clarity_          = min_clarity;
    d->min_frequency_        = min_frequency;
    d->max_frequency_        = max_frequency;
}

void hawk_pitch_detector_free(PitchDetector* d)
{
    if (d->fft_ready_) rfftf_free(&d->fft_);
    d->fft_ready_ = false;
    hawk_buf_free(&d->re_); hawk_buf_free(&d->xr_); hawk_buf_free(&d->xi_); hawk_buf_free(&d->prefix_);
    hawk_buf_free(&d->hp_); hawk_buf_free(&d->nsdf_); hawk_buf_free(&d->maxima_);
}

PitchEstimate hawk_pitch_detector_analyze(const PitchDetector* cd, const float* samples, size_t count)
{
    PitchDetector* d = (PitchDetector*)cd;   // the workspace is a cache: see pitch_detector.h
    const double sample_rate_ = d->sample_rate_, peak_threshold_ratio_ = d->peak_threshold_ratio_;
    const double min_clarity_ = d->min_clarity_, min_frequency_ = d->min_frequency_, max_frequency_ = d->max_frequency_;
    PitchEstimate estimate = { 0.0, 0.0, false };
    if (samples == NULL || count < 4) {
        return estimate;
    }

    // --- 0. Remove the frame's mean before anything looks at it -----------
    //
    // WHY. The lag range below bounds the SEARCH, not the SIGNAL, so out-of-band energy still
    // reaches the NSDF. A DC offset is the worst of it: it removes the NSDF's zero crossings, and
    // the hump search in step 2 needs them to find any maximum at all. Measured on a clean E2 with
    // an offset: at DC = 0.20 the detector is still perfect, at DC = 0.30 it is completely BLIND
    // (f = 0, clarity = 0). It does not degrade -- it stops.
    //
    // Subtracting the mean is the exactly right tool for that: zero phase, no transient, no
    // frequency response to argue about, one pass. Measured after: identical readings at every
    // offset from 0.00 to 0.50 (82.407 Hz, clarity 1.000, 0.00 cents).
    //
    // ⚠ WHAT THIS DOES *NOT* FIX, and why the filter for it is not here. Sub-audio RUMBLE (a mic stand,
    // a footfall) is not DC and survives this. The obvious answer is a 20-25 Hz high-pass, and it
    // was tried four ways -- causal, zero-phase, warmed, and warmed-and-zero-phase. Every one of
    // them buys rumble immunity by biasing the pitch of clean LOW notes, because the corner has to
    // sit right next to the lowest note the plugin claims (B0 = 30.87 Hz) and this window is only
    // ~170 ms, i.e. about 1.5 cycles of 9 Hz. Separating the two inside that window is a
    // time-frequency limit, not an implementation problem, and the measured trade is real: the
    // best variant lifted the rumble case over the clarity gate but shifted a clean B0 by
    // -16.8 cents (warmed) or a clean E2 by -2.8 cents (warmed + zero phase).
    // The plugin takes that decision on the whole stream instead, BEFORE this window: a 4th-order
    // 20 Hz high-pass that runs continuously (HighPass20 in lv2/hawk_lv2.c), so it is warm and
    // its phase is the same for every window. This class stays filter-free.
    float* hp_ = (float*)hawk_buf_reserve(&d->hp_, count, sizeof(float));
    {
        double mean = 0.0;
        for (size_t j = 0; j < count; ++j) mean += samples[j];
        mean /= (double)count;
        for (size_t j = 0; j < count; ++j)
            hp_[j] = (float)(samples[j] - mean);
    }
    const float* sig = hp_;     // everything below reads THIS, not `samples`

    // --- 1. Normalized Square Difference Function -------------------------
    // For each lag tau, measure how well the frame matches a copy of itself
    // delayed by tau. nsdf[tau] lands in [-1, 1]; +1 means perfect periodicity
    // at that lag. We only need lags up to half the frame (one period must fit
    // twice), and we further restrict to the lag range matching our musical
    // frequency bounds -- this stops the detector ever locking onto ultrasonic
    // noise (tiny lags) or sub-audio rumble (huge lags).
    size_t tau_hi = (size_t)(sample_rate_ / min_frequency_) + 1;
    if (tau_hi > count / 2) tau_hi = count / 2;
    size_t tau_lo = (size_t)(sample_rate_ / max_frequency_);
    if (tau_lo < 1) tau_lo = 1;
    if (tau_hi <= tau_lo + 2) return estimate;

    double* nsdf = (double*)hawk_buf_reserve(&d->nsdf_, tau_hi, sizeof(double));
    for (size_t k = 0; k < tau_hi; ++k) nsdf[k] = 0.0;

    // The autocorrelation r[tau] = sum_j x[j]*x[j+tau] is computed in O(N log N)
    // as r = IFFT(|FFT(x)|^2) on a zero-padded frame (padding to >= 2*count makes
    // the circular correlation equal the linear one). The NSDF normalizer
    // m[tau] = sum (x[j]^2 + x[j+tau]^2) over the overlap comes from a prefix sum
    // of squares: m[tau] = P[count-tau] + (S - P[tau]). nsdf = 2r/m, exactly as
    // the old direct double-loop produced, just far cheaper.
    size_t m = 1;
    while (m < 2 * count) m <<= 1;
    if (!d->fft_ready_ || rfftf_size(&d->fft_) != m) {
        if (d->fft_ready_) rfftf_free(&d->fft_);
        if (!rfftf_init(&d->fft_, m)) { fputs("the hawk: out of memory\n", stderr); abort(); }
        d->fft_ready_ = true;
    }
    float* re_ = (float*)hawk_buf_reserve(&d->re_, m, sizeof(float));
    float* xr_ = (float*)hawk_buf_reserve(&d->xr_, m / 2 + 1, sizeof(float));
    float* xi_ = (float*)hawk_buf_reserve(&d->xi_, m / 2 + 1, sizeof(float));
    for (size_t k = 0; k < m; ++k) re_[k] = 0.0f;
    for (size_t k = 0; k < m / 2 + 1; ++k) { xr_[k] = 0.0f; xi_[k] = 0.0f; }
    for (size_t j = 0; j < count; ++j) re_[j] = sig[j];

    // Both transforms here have REAL input -- the padded frame, and then its power spectrum, which
    // is real because the frame is. A complex FFT would carry an all-zero imaginary half through
    // both, so each is done with a half-length complex transform plus an untangle pass instead.
    rfftf_forward(&d->fft_, re_, xr_, xi_);        // X, bins 0..m/2

    // |X|^2, mirrored back to full length. It is real AND EVEN (x is real), and the unscaled
    // inverse transform of a real even sequence equals its FORWARD transform -- so the same
    // real-input routine serves for the second step, and its output is the autocorrelation.
    for (size_t k = 0; k <= m / 2; ++k)
        re_[k] = xr_[k] * xr_[k] + xi_[k] * xi_[k];
    for (size_t k = m / 2 + 1; k < m; ++k) re_[k] = re_[m - k];
    rfftf_forward(&d->fft_, re_, xr_, xi_);        // r[tau] = xr_[tau]

    double* prefix_ = (double*)hawk_buf_reserve(&d->prefix_, count + 1, sizeof(double));   // P[k] = sum of first k squares
    for (size_t k = 0; k <= count; ++k) prefix_[k] = 0.0;
    for (size_t j = 0; j < count; ++j) {
        const double s = (double)sig[j];
        prefix_[j + 1] = prefix_[j] + s * s;
    }
    const double total_sq = prefix_[count];          // S
    const double inv_m = 1.0 / (double)m;
    for (size_t tau = 0; tau < tau_hi; ++tau) {
        const double autocorrelation = xr_[tau] * inv_m;
        const double normalizer = prefix_[count - tau] + (total_sq - prefix_[tau]);
        nsdf[tau] = (normalizer > 0.0) ? (2.0 * autocorrelation / normalizer) : 0.0;
    }

    // --- 2. Key maxima (one per positive hump of the NSDF) ----------------
    // one slot per lag: more than any frame can produce (a maximum is a lag, at most one per lag)
    size_t* maxima = (size_t*)hawk_buf_reserve(&d->maxima_, tau_hi + 1, sizeof(size_t));
    size_t n_maxima = 0;
    size_t pos = 0;

    // Skip the initial lobe around tau = 0 (the trivial "self at lag 0" peak),
    // then skip the first negative region, so we start at the first real hump.
    while (pos < tau_hi - 1 && nsdf[pos] > 0.0) ++pos;
    while (pos < tau_hi - 1 && nsdf[pos] <= 0.0) ++pos;
    if (pos == 0) pos = 1;

    size_t current_max = 0;  // 0 doubles as "none yet" (pos is always >= 1)
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
                maxima[n_maxima++] = current_max;
                current_max = 0;
            }
            while (pos < tau_hi - 1 && nsdf[pos] <= 0.0) ++pos;
        }
    }
    if (current_max > 0) {
        maxima[n_maxima++] = current_max;
    }

    if (n_maxima == 0) {
        return estimate;
    }

    // --- 3. Choose the first peak that clears the threshold ---------------
    // Selecting the *first* eligible peak (not the tallest) is what keeps us on
    // the true fundamental instead of dropping to a strong harmonic an octave
    // away.
    // Only consider peaks within the musical lag range [tau_lo, tau_hi).
    double highest = 0.0;
    for (size_t q = 0; q < n_maxima; ++q) {
        const size_t p = maxima[q];
        if (p < tau_lo) continue;
        highest = (highest < nsdf[p]) ? nsdf[p] : highest;   // max
    }
    const double threshold = peak_threshold_ratio_ * highest;

    size_t chosen = 0;
    bool found = false;
    for (size_t q = 0; q < n_maxima; ++q) {
        const size_t p = maxima[q];
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

    double tau_peak = (double)chosen;
    double clarity  = y1;
    if (a < 0.0) {  // concave -> a genuine maximum we can refine
        const double offset = -b / (2.0 * a);
        tau_peak = (double)chosen + offset;
        clarity  = y1 - (b * b) / (4.0 * a);
    }

    if (tau_peak <= 0.0) {
        return estimate;
    }

    estimate.frequency = sample_rate_ / tau_peak;
    estimate.clarity   = (1.0 < clarity) ? 1.0 : clarity;   // min
    estimate.voiced    = estimate.clarity >= min_clarity_;
    return estimate;
}

