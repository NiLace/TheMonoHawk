/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) The NL Sounds contributors */
#pragma once

#include "math_constants.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace hawk {

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
// Templated on the working scalar. The hot paths use FFT<float>: single
// precision halves the FLOP/bandwidth cost of double, and for a
// tuner it is far more than enough -- 24-bit mantissa (~7 digits) versus the
// ~1e-5 relative accuracy a cent needs, with peak locations refined by parabolic
// interpolation on top. Twiddles are computed in double then cast, so the table
// itself carries no single-precision rounding.
template <class Real = float>
class FFT {
public:
    explicit FFT(std::size_t n)
        : n_(n), cos_(n / 2), sin_(n / 2), sr_(n), si_(n)
    {
        for (std::size_t k = 0; k < n / 2; ++k) {
            cos_[k] = static_cast<Real>(std::cos(-TWO_PI * static_cast<double>(k) / static_cast<double>(n)));
            sin_[k] = static_cast<Real>(std::sin(-TWO_PI * static_cast<double>(k) / static_cast<double>(n)));
        }
    }

    std::size_t size() const { return n_; }

    void transform(Real* re, Real* im, bool inverse) const
    {
        if (n_ < 2) return;
        Real* xr = re;         Real* xi = im;
        Real* yr = sr_.data(); Real* yi = si_.data();
        const Real sgn = inverse ? Real(-1) : Real(1);
        // Each stage: n = 2*l*m; block j of m points pairs with block j + l and writes blocks 2j and
        // 2j + 1, the second turned by the twiddle e^{-2 pi i j m / n} = cos_[j*m] + i sin_[j*m].
        std::size_t l = n_ / 2, m = 1;
        {   // m == 1
            for (std::size_t j = 0; j < l; ++j) {
                const Real wr = cos_[j], wi = sgn * sin_[j];
                const Real c0r = xr[j], c0i = xi[j], c1r = xr[j + l], c1i = xi[j + l];
                yr[2 * j] = c0r + c1r;  yi[2 * j] = c0i + c1i;
                const Real dr = c0r - c1r, di = c0i - c1i;
                yr[2 * j + 1] = dr * wr - di * wi;
                yi[2 * j + 1] = dr * wi + di * wr;
            }
            std::swap(xr, yr); std::swap(xi, yi);
            l >>= 1; m <<= 1;
        }
        if (l >= 1) {   // m == 2
            for (std::size_t j = 0; j < l; ++j) {
                const Real wr = cos_[2 * j], wi = sgn * sin_[2 * j];
                for (std::size_t k = 0; k < 2; ++k) {
                    const Real c0r = xr[2 * j + k],         c0i = xi[2 * j + k];
                    const Real c1r = xr[2 * j + 2 * l + k], c1i = xi[2 * j + 2 * l + k];
                    yr[4 * j + k] = c0r + c1r;  yi[4 * j + k] = c0i + c1i;
                    const Real dr = c0r - c1r, di = c0i - c1i;
                    yr[4 * j + 2 + k] = dr * wr - di * wi;
                    yi[4 * j + 2 + k] = dr * wi + di * wr;
                }
            }
            std::swap(xr, yr); std::swap(xi, yi);
            l >>= 1; m <<= 1;
        }
        for (; l >= 1; l >>= 1, m <<= 1) {
            for (std::size_t j = 0; j < l; ++j) {
                const Real wr = cos_[j * m], wi = sgn * sin_[j * m];
                const Real* ar = xr + j * m;          const Real* ai = xi + j * m;
                const Real* br = xr + (j + l) * m;    const Real* bi = xi + (j + l) * m;
                Real* o0r = yr + 2 * j * m;           Real* o0i = yi + 2 * j * m;
                Real* o1r = o0r + m;                  Real* o1i = o0i + m;
                for (std::size_t k = 0; k < m; ++k) {
                    const Real c0r = ar[k], c0i = ai[k], c1r = br[k], c1i = bi[k];
                    o0r[k] = c0r + c1r;  o0i[k] = c0i + c1i;
                    const Real dr = c0r - c1r, di = c0i - c1i;
                    o1r[k] = dr * wr - di * wi;
                    o1i[k] = dr * wi + di * wr;
                }
            }
            std::swap(xr, yr); std::swap(xi, yi);
        }
        if (xr != re) {                     // an odd number of stages ended in the scratch
            std::copy(xr, xr + n_, re);
            std::copy(xi, xi + n_, im);
        }
    }

private:
    std::size_t n_;
    std::vector<Real> cos_, sin_;
    mutable std::vector<Real> sr_, si_;   // ping-pong scratch
};

// Forward DFT of a REAL sequence of length n, done with a complex FFT of length n/2.
//
// WHY. Both transforms the detector needs have real input: the frame itself, and then the power
// spectrum |X|^2 (real, and even, because the frame is real). Feeding real data to a complex FFT
// throws away half the work -- the imaginary input is all zeros and the output is conjugate
// symmetric. Packing the even samples into the real part and the odd ones into the imaginary part
// halves the transform, at the cost of one O(n) untangle pass.
//
// out_re/out_im receive n/2 + 1 bins, which is all there is: X[n-k] = conj(X[k]).
template <class Real = float>
class RealFFT {
public:
    explicit RealFFT(std::size_t n) : n_(n), half_(n / 2), fft_(n / 2),
                                      zr_(n / 2), zi_(n / 2), cs_(n / 2), sn_(n / 2)
    {
        for (std::size_t k = 0; k < half_; ++k) {
            cs_[k] = static_cast<Real>(std::cos(-TWO_PI * static_cast<double>(k) / static_cast<double>(n)));
            sn_[k] = static_cast<Real>(std::sin(-TWO_PI * static_cast<double>(k) / static_cast<double>(n)));
        }
    }

    std::size_t size() const { return n_; }

    void forward(const Real* x, Real* out_re, Real* out_im) const
    {
        // pack: z[j] = x[2j] + i*x[2j+1]
        for (std::size_t j = 0; j < half_; ++j) { zr_[j] = x[2 * j]; zi_[j] = x[2 * j + 1]; }
        fft_.transform(zr_.data(), zi_.data(), /*inverse=*/false);

        // untangle: split Z into the transforms of the even and odd subsequences, then recombine.
        //   Ze = (Z[k] + conj(Z[k'])) / 2        Zo = (Z[k] - conj(Z[k'])) / (2i)
        //   X[k] = Ze + e^{-2*pi*i*k/n} * Zo,    with k' = (n/2 - k) mod (n/2)
        for (std::size_t k = 0; k <= half_; ++k) {
            const std::size_t kk = (half_ - k) % half_;
            const std::size_t ki = (k == half_) ? 0 : k;
            const Real ar = zr_[ki], ai = zi_[ki];
            const Real br = zr_[kk], bi = -zi_[kk];              // conj(Z[k'])
            const Real er = Real(0.5) * (ar + br), ei = Real(0.5) * (ai + bi);   // Ze
            const Real orr = Real(0.5) * (ai - bi), oi = Real(-0.5) * (ar - br); // Zo = (..)/(2i)
            const Real c = (k == half_) ? Real(-1) : cs_[k];
            const Real sg = (k == half_) ? Real(0)  : sn_[k];
            out_re[k] = er + (orr * c - oi * sg);
            out_im[k] = ei + (orr * sg + oi * c);
        }
    }

private:
    std::size_t n_, half_;
    FFT<Real> fft_;
    mutable std::vector<Real> zr_, zi_;
    std::vector<Real> cs_, sn_;
};

} // namespace hawk
