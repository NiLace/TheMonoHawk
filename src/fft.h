/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) The NL Sounds contributors */
#pragma once

#include "math_constants.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// Radix-2 FFT, Stockham autosort form. Size must be a power of two. The twiddle factors are
// precomputed at construction, so each transform() is allocation- and trig-free. Own code, no
// dependency.
//
// transform(re, im, inverse=false) is the forward DFT; inverse=true is the
// conjugate transform WITHOUT the 1/n scaling (the caller divides by size() when
// it wants a true inverse). Used to turn the detector's O(N^2) autocorrelation
// into O(N log N) via r = IFFT(|FFT(x)|^2).
//
// Stockham, not in-place Cooley-Tukey: each stage reads two contiguous runs and writes two, ping-
// ponging between the caller's arrays and a scratch pair, so there is no bit-reversal permutation.
// That permutation is a random-access swap per element, and at the plugin's sizes it missed the cache
// on almost every one: the in-place version spent ~a quarter of its time there. Measured against it
// (i7-8700K, float, same twiddles, same error against a double DFT, ~1.3e-7 relative): x2.1 at 8192
// points, x2.8 at 16384, x3.2 at 65536. The first two stages, whose inner runs are 1 and 2 long, get
// loops of their own over the outer index. The scratch makes an instance usable by ONE thread at a
// time, as RealFFT already was.
//
// Single precision: single
// precision halves the FLOP/bandwidth cost of double, and for a
// tuner it is far more than enough -- 24-bit mantissa (~7 digits) versus the
// ~1e-5 relative accuracy a cent needs, with peak locations refined by parabolic
// interpolation on top. Twiddles are computed in double then cast, so the table
// itself carries no single-precision rounding.
typedef struct FFTf {
    size_t n;
    float* cos_;  float* sin_;   // n/2 twiddles
    float* sr;    float* si;     // ping-pong scratch, n each
} FFTf;

// Allocates and fills the twiddles; false if an allocation failed (then fftf_free() is still safe).
static inline bool fftf_init(FFTf* f, size_t n)
{
    f->n = n;
    const size_t h = n / 2 ? n / 2 : 1, m = n ? n : 1;
    f->cos_ = (float*)calloc(h, sizeof(float));
    f->sin_ = (float*)calloc(h, sizeof(float));
    f->sr   = (float*)calloc(m, sizeof(float));
    f->si   = (float*)calloc(m, sizeof(float));
    if (!f->cos_ || !f->sin_ || !f->sr || !f->si) return false;
    for (size_t k = 0; k < n / 2; ++k) {
        f->cos_[k] = (float)(cos(-HAWK_TWO_PI * (double)k / (double)n));
        f->sin_[k] = (float)(sin(-HAWK_TWO_PI * (double)k / (double)n));
    }
    return true;
}

static inline void fftf_free(FFTf* f)
{
    free(f->cos_); free(f->sin_); free(f->sr); free(f->si);
    f->cos_ = f->sin_ = f->sr = f->si = NULL;
}

static inline size_t fftf_size(const FFTf* f) { return f->n; }

// transform(re, im, inverse=false) is the forward DFT; inverse=true is the conjugate transform WITHOUT the 1/n
// scaling. Uses the instance's scratch: one thread at a time.
static inline void fftf_transform(const FFTf* f, float* re, float* im, bool inverse)
{
    const size_t n_ = f->n;
    if (n_ < 2) return;
    const float* cos_ = f->cos_;
    const float* sin_ = f->sin_;
    float* xr = re;     float* xi = im;
    float* yr = f->sr;  float* yi = f->si;
    float* t;
    const float sgn = inverse ? -1.0f : 1.0f;
    // Each stage: n = 2*l*m; block j of m points pairs with block j + l and writes blocks 2j and
    // 2j + 1, the second turned by the twiddle e^{-2 pi i j m / n} = cos_[j*m] + i sin_[j*m].
    size_t l = n_ / 2, m = 1;
    {   // m == 1
        for (size_t j = 0; j < l; ++j) {
            const float wr = cos_[j], wi = sgn * sin_[j];
            const float c0r = xr[j], c0i = xi[j], c1r = xr[j + l], c1i = xi[j + l];
            yr[2 * j] = c0r + c1r;  yi[2 * j] = c0i + c1i;
            const float dr = c0r - c1r, di = c0i - c1i;
            yr[2 * j + 1] = dr * wr - di * wi;
            yi[2 * j + 1] = dr * wi + di * wr;
        }
        t = xr; xr = yr; yr = t;  t = xi; xi = yi; yi = t;
        l >>= 1; m <<= 1;
    }
    if (l >= 1) {   // m == 2
        for (size_t j = 0; j < l; ++j) {
            const float wr = cos_[2 * j], wi = sgn * sin_[2 * j];
            for (size_t k = 0; k < 2; ++k) {
                const float c0r = xr[2 * j + k],         c0i = xi[2 * j + k];
                const float c1r = xr[2 * j + 2 * l + k], c1i = xi[2 * j + 2 * l + k];
                yr[4 * j + k] = c0r + c1r;  yi[4 * j + k] = c0i + c1i;
                const float dr = c0r - c1r, di = c0i - c1i;
                yr[4 * j + 2 + k] = dr * wr - di * wi;
                yi[4 * j + 2 + k] = dr * wi + di * wr;
            }
        }
        t = xr; xr = yr; yr = t;  t = xi; xi = yi; yi = t;
        l >>= 1; m <<= 1;
    }
    for (; l >= 1; l >>= 1, m <<= 1) {
        for (size_t j = 0; j < l; ++j) {
            const float wr = cos_[j * m], wi = sgn * sin_[j * m];
            const float* ar = xr + j * m;          const float* ai = xi + j * m;
            const float* br = xr + (j + l) * m;    const float* bi = xi + (j + l) * m;
            float* o0r = yr + 2 * j * m;           float* o0i = yi + 2 * j * m;
            float* o1r = o0r + m;                  float* o1i = o0i + m;
            for (size_t k = 0; k < m; ++k) {
                const float c0r = ar[k], c0i = ai[k], c1r = br[k], c1i = bi[k];
                o0r[k] = c0r + c1r;  o0i[k] = c0i + c1i;
                const float dr = c0r - c1r, di = c0i - c1i;
                o1r[k] = dr * wr - di * wi;
                o1i[k] = dr * wi + di * wr;
            }
        }
        t = xr; xr = yr; yr = t;  t = xi; xi = yi; yi = t;
    }
    if (xr != re) {                     // an odd number of stages ended in the scratch
        memcpy(re, xr, n_ * sizeof(float));
        memcpy(im, xi, n_ * sizeof(float));
    }
}

// Forward DFT of a REAL sequence of length n, done with a complex FFT of length n/2.
//
// WHY. Both transforms the detector needs have real input: the frame itself, and then the power
// spectrum |X|^2 (real, and even, because the frame is real). Feeding real data to a complex FFT
// throws away half the work -- the imaginary input is all zeros and the output is conjugate
// symmetric. Packing the even samples into the real part and the odd ones into the imaginary part
// halves the transform, at the cost of one O(n) untangle pass.
//
// out_re/out_im receive n/2 + 1 bins, which is all there is: X[n-k] = conj(X[k]).
typedef struct RealFFTf {
    size_t n, half;
    FFTf   fft;
    float* zr;  float* zi;   // half each
    float* cs;  float* sn;   // half each
} RealFFTf;

static inline bool rfftf_init(RealFFTf* r, size_t n)
{
    r->n = n; r->half = n / 2;
    const size_t h = r->half ? r->half : 1;
    const bool ok = fftf_init(&r->fft, n / 2);
    r->zr = (float*)calloc(h, sizeof(float));
    r->zi = (float*)calloc(h, sizeof(float));
    r->cs = (float*)calloc(h, sizeof(float));
    r->sn = (float*)calloc(h, sizeof(float));
    if (!ok || !r->zr || !r->zi || !r->cs || !r->sn) return false;
    for (size_t k = 0; k < r->half; ++k) {
        r->cs[k] = (float)(cos(-HAWK_TWO_PI * (double)k / (double)n));
        r->sn[k] = (float)(sin(-HAWK_TWO_PI * (double)k / (double)n));
    }
    return true;
}

static inline void rfftf_free(RealFFTf* r)
{
    fftf_free(&r->fft);
    free(r->zr); free(r->zi); free(r->cs); free(r->sn);
    r->zr = r->zi = r->cs = r->sn = NULL;
}

static inline size_t rfftf_size(const RealFFTf* r) { return r->n; }

static inline void rfftf_forward(const RealFFTf* r, const float* x, float* out_re, float* out_im)
{
    const size_t half_ = r->half;
    float* zr_ = r->zr;  float* zi_ = r->zi;
    const float* cs_ = r->cs;  const float* sn_ = r->sn;
    // pack: z[j] = x[2j] + i*x[2j+1]
    for (size_t j = 0; j < half_; ++j) { zr_[j] = x[2 * j]; zi_[j] = x[2 * j + 1]; }
    fftf_transform(&r->fft, zr_, zi_, /*inverse=*/false);

    // untangle: split Z into the transforms of the even and odd subsequences, then recombine.
    //   Ze = (Z[k] + conj(Z[k'])) / 2        Zo = (Z[k] - conj(Z[k'])) / (2i)
    //   X[k] = Ze + e^{-2*pi*i*k/n} * Zo,    with k' = (n/2 - k) mod (n/2)
    for (size_t k = 0; k <= half_; ++k) {
        const size_t kk = (half_ - k) % half_;
        const size_t ki = (k == half_) ? 0 : k;
        const float ar = zr_[ki], ai = zi_[ki];
        const float br = zr_[kk], bi = -zi_[kk];              // conj(Z[k'])
        const float er = 0.5f * (ar + br), ei = 0.5f * (ai + bi);     // Ze
        const float orr = 0.5f * (ai - bi), oi = -0.5f * (ar - br);   // Zo = (..)/(2i)
        const float c = (k == half_) ? -1.0f : cs_[k];
        const float sg = (k == half_) ? 0.0f  : sn_[k];
        out_re[k] = er + (orr * c - oi * sg);
        out_im[k] = ei + (orr * sg + oi * c);
    }
}
