#pragma once

#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace hawk {

// Minimal in-place radix-2 Cooley-Tukey FFT. Size must be a power of two. The
// bit-reversal permutation and twiddle factors are precomputed at construction,
// so each transform() is allocation- and trig-free. Own code, no dependency, so
// the project stays Public-Domain-publishable.
//
// transform(re, im, inverse=false) is the forward DFT; inverse=true is the
// conjugate transform WITHOUT the 1/n scaling (the caller divides by size() when
// it wants a true inverse). Used to turn the detector's O(N^2) autocorrelation
// into O(N log N) via r = IFFT(|FFT(x)|^2).
//
// Templated on the working scalar. The hot paths use FFT<float>: single
// precision halves the FLOP/bandwidth cost (and gains more on NEON), and for a
// tuner it is far more than enough -- 24-bit mantissa (~7 digits) versus the
// ~1e-5 relative accuracy a cent needs, with peak locations refined by parabolic
// interpolation on top. Twiddles are computed in double then cast, so the table
// itself carries no single-precision rounding.
template <class Real = float>
class FFT {
public:
    explicit FFT(std::size_t n)
        : n_(n), rev_(n), cos_(n / 2), sin_(n / 2)
    {
        int logn = 0;
        while ((static_cast<std::size_t>(1) << logn) < n) ++logn;
        for (std::size_t i = 0; i < n; ++i) {
            std::size_t r = 0;
            for (int b = 0; b < logn; ++b)
                if (i & (static_cast<std::size_t>(1) << b))
                    r |= static_cast<std::size_t>(1) << (logn - 1 - b);
            rev_[i] = r;
        }
        constexpr double TWO_PI = 6.283185307179586;
        for (std::size_t k = 0; k < n / 2; ++k) {
            cos_[k] = static_cast<Real>(std::cos(-TWO_PI * static_cast<double>(k) / static_cast<double>(n)));
            sin_[k] = static_cast<Real>(std::sin(-TWO_PI * static_cast<double>(k) / static_cast<double>(n)));
        }
    }

    std::size_t size() const { return n_; }

    void transform(Real* re, Real* im, bool inverse) const
    {
        for (std::size_t i = 0; i < n_; ++i) {
            const std::size_t r = rev_[i];
            if (i < r) { std::swap(re[i], re[r]); std::swap(im[i], im[r]); }
        }
        for (std::size_t len = 2; len <= n_; len <<= 1) {
            const std::size_t half = len >> 1;
            const std::size_t step = n_ / len;
            for (std::size_t i = 0; i < n_; i += len) {
                std::size_t k = 0;
                for (std::size_t j = i; j < i + half; ++j) {
                    const Real c = cos_[k];
                    const Real s = inverse ? -sin_[k] : sin_[k];
                    const Real vr = re[j + half] * c - im[j + half] * s;
                    const Real vi = re[j + half] * s + im[j + half] * c;
                    const Real ur = re[j],  ui = im[j];
                    re[j] = ur + vr;  im[j] = ui + vi;
                    re[j + half] = ur - vr;  im[j + half] = ui - vi;
                    k += step;
                }
            }
        }
    }

private:
    std::size_t n_;
    std::vector<std::size_t> rev_;
    std::vector<Real> cos_, sin_;
};

} // namespace hawk
