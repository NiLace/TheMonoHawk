// Offline diagnostic: run the real detector across a recorded signal and
// dissect it, so we can see why the cents reading wanders / reads sharp.
//
// Input: raw 32-bit float mono at 48 kHz on argv[1].

#include "fine_tuner.hpp"
#include "note_mapping.hpp"
#include "pitch_detector.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
constexpr double pi = 3.14159265358979323846;
constexpr double sr = 48000.0;

// Magnitude of the DFT at one frequency over a window (Goertzel-style).
double mag_at(const float* x, std::size_t n, double f)
{
    double re = 0.0, im = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double a = 2.0 * pi * f * static_cast<double>(i) / sr;
        re += x[i] * std::cos(a);
        im -= x[i] * std::sin(a);
    }
    return std::sqrt(re * re + im * im) * 2.0 / static_cast<double>(n);
}

// Find the spectral peak near `f_guess` (+/- 4%), returning its frequency.
double refine_peak(const float* x, std::size_t n, double f_guess, double* amp)
{
    double best_f = f_guess, best_m = -1.0;
    for (int k = -80; k <= 80; ++k) {
        const double f = f_guess * (1.0 + 0.0005 * k);
        const double m = mag_at(x, n, f);
        if (m > best_m) { best_m = m; best_f = f; }
    }
    if (amp) *amp = best_m;
    return best_f;
}
} // namespace

int main(int argc, char** argv)
{
    const char* path = (argc > 1) ? argv[1] : "/tmp/D.f32";
    FILE* fp = std::fopen(path, "rb");
    if (!fp) { std::printf("cannot open %s\n", path); return 1; }
    std::vector<float> x;
    float buf[4096];
    std::size_t got;
    while ((got = std::fread(buf, sizeof(float), 4096, fp)) > 0)
        x.insert(x.end(), buf, buf + got);
    std::fclose(fp);
    std::printf("loaded %zu samples (%.2f s)\n", x.size(), x.size() / sr);

    const hawk::PitchDetector coarse(sr);
    const hawk::FineTuner     fine(sr);
    const std::size_t WIN = 8192, HOP = 2048;

    // Frame-by-frame sweep: quantify bias and jitter of coarse vs fine cents.
    std::printf("\n  t(s)   note  coarseHz  coarse_c  fine_c  clarity\n");
    double sum_fine = 0, sum_fine2 = 0, sum_coarse = 0;
    int    voiced = 0, printed = 0;
    std::size_t best_off = 0; double best_rms = 0;
    for (std::size_t off = 0; off + WIN <= x.size(); off += HOP) {
        double rms = 0; for (std::size_t i = 0; i < WIN; ++i) rms += x[off+i]*x[off+i];
        rms = std::sqrt(rms / WIN);
        if (rms > best_rms) { best_rms = rms; best_off = off; }

        auto c = coarse.analyze(&x[off], WIN);
        if (!c.voiced) continue;
        auto nr = hawk::frequency_to_note(c.frequency);
        double target = hawk::note_to_frequency(nr.midi_note);
        auto f = fine.refine(&x[off], WIN, target);
        if (!f.valid) continue;
        ++voiced;
        sum_fine += f.cents; sum_fine2 += f.cents * f.cents;
        sum_coarse += nr.cents;
        if ((voiced % 12) == 1 && printed < 24) {  // sample ~every 0.5 s
            std::printf("  %5.2f  %-4s  %8.3f  %+7.2f  %+6.2f  %.3f\n",
                        off / sr, hawk::note_name(nr.midi_note).c_str(),
                        c.frequency, nr.cents, f.cents, c.clarity);
            ++printed;
        }
    }
    if (voiced) {
        double mean = sum_fine / voiced;
        double var  = sum_fine2 / voiced - mean * mean;
        std::printf("\nover %d voiced frames:\n", voiced);
        std::printf("  fine   cents: mean %+.2f, stddev %.2f\n", mean,
                    std::sqrt(var > 0 ? var : 0));
        std::printf("  coarse cents: mean %+.2f\n", sum_coarse / voiced);
    }

    // Harmonic dissection on the strongest window.
    std::printf("\n=== harmonic structure at t=%.2f s (strongest) ===\n",
                best_off / sr);
    auto c = coarse.analyze(&x[best_off], WIN);
    auto nr = hawk::frequency_to_note(c.frequency);
    double f0 = hawk::note_to_frequency(nr.midi_note);
    std::printf("detected note %s, ET f0 = %.3f Hz\n",
                hawk::note_name(nr.midi_note).c_str(), f0);
    std::printf("  n   freq(Hz)   amp     amp/A1   f_n/n(Hz)  stretch(cents)\n");
    double a1 = 0, f1n = 0;
    for (int n = 1; n <= 9; ++n) {
        double amp = 0;
        double fn = refine_peak(&x[best_off], WIN, f0 * n, &amp);
        if (n == 1) { a1 = amp > 0 ? amp : 1e-9; f1n = fn; }
        double implied = fn / n;
        double stretch = 1200.0 * std::log2(implied / f1n);
        std::printf("  %d  %9.3f  %.4f  %6.2f   %9.3f  %+7.2f\n",
                    n, fn, amp, amp / a1, implied, stretch);
    }
    return 0;
}
