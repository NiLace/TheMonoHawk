// Test harness for the fine heterodyne stage, exercised through the real
// pipeline: coarse detector picks the note, the note's exact frequency becomes
// the heterodyne target, the fine stage reports sub-cent deviation.
//
// The headline case is inharmonicity: the coarse stage read the piano-grade
// E1 string +10.67 cents sharp (pulled by stretched partials); the fine stage,
// tracking only the fundamental, should recover its true ~+0.35 cent offset.

#include "fine_tuner.hpp"
#include "note_mapping.hpp"
#include "pitch_detector.hpp"

#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr double pi          = 3.14159265358979323846;
constexpr double sample_rate = 48000.0;

int checks_run    = 0;
int checks_failed = 0;

std::size_t frame_length(double frequency)
{
    double n = 8.0 * sample_rate / frequency;
    if (n < 2048.0)  n = 2048.0;
    if (n > 16384.0) n = 16384.0;
    return static_cast<std::size_t>(n);
}

std::vector<float> make_tone(double f0, std::size_t count,
                             const std::vector<double>& amps,
                             double inharmonicity_b = 0.0,
                             double snr_db = 1e9, std::uint32_t seed = 7u)
{
    std::vector<float> buffer(count, 0.0f);
    std::mt19937 rng(seed);
    std::normal_distribution<double> gaussian(0.0, 1.0);

    double sum_sq = 0.0;
    for (double a : amps) sum_sq += a * a;
    const double sig_rms   = std::sqrt(sum_sq / 2.0);
    const double noise_amp = (snr_db < 200.0)
        ? sig_rms / std::pow(10.0, snr_db / 20.0) : 0.0;

    for (std::size_t i = 0; i < count; ++i) {
        const double t = static_cast<double>(i) / sample_rate;
        double sample = 0.0;
        for (std::size_t h = 0; h < amps.size(); ++h) {
            const int n  = static_cast<int>(h) + 1;
            double    fn = n * f0;
            if (inharmonicity_b > 0.0)
                fn = n * f0 * std::sqrt(1.0 + inharmonicity_b * n * n);
            sample += amps[h] * std::sin(2.0 * pi * fn * t);
        }
        if (noise_amp > 0.0) sample += noise_amp * gaussian(rng);
        buffer[i] = static_cast<float>(0.3 * sample);
    }
    return buffer;
}

// Run coarse -> target note -> fine, and check the fine cents against an
// expected value. `description` is for the printout.
void check_fine(const hawk::PitchDetector& coarse,
                const hawk::FineTuner& fine,
                const std::string& label,
                double f0,
                const std::vector<float>& samples,
                double expected_cents,
                double tol_cents)
{
    ++checks_run;
    const hawk::PitchEstimate c = coarse.analyze(samples.data(),
                                                 samples.size());
    const int   note   = hawk::frequency_to_note(c.frequency).midi_note;
    const double target = hawk::note_to_frequency(note);
    const hawk::FineEstimate f = fine.refine(samples.data(), samples.size(),
                                             target);

    const bool ok = f.valid &&
                    std::fabs(f.cents - expected_cents) <= tol_cents;
    if (!ok) ++checks_failed;

    std::printf("  [%s] %-26s target %-4s  fine %+7.3fc "
                "(want %+6.2f +/- %.2f)  f=%9.3f Hz\n",
                ok ? "PASS" : "FAIL", label.c_str(),
                hawk::note_name(note).c_str(), f.cents,
                expected_cents, tol_cents, f.frequency);
    (void)f0;
}

} // namespace

int main()
{
    const hawk::PitchDetector coarse(sample_rate);
    const hawk::FineTuner     fine(sample_rate);

    const std::vector<double> sine    = {1.0};
    const std::vector<double> guitar  = {1.0, 0.6, 0.45, 0.3, 0.22, 0.16};
    const std::vector<double> bass    = {1.0, 0.85, 0.7, 0.55, 0.45, 0.35,
                                         0.28, 0.22, 0.18};

    // A frequency `cents` away from f0.
    auto detune = [](double f0, double cents) {
        return f0 * std::pow(2.0, cents / 1200.0);
    };

    std::printf("== pure tones, sub-cent recovery ==\n");
    check_fine(coarse, fine, "A2 in tune", 110.0,
               make_tone(110.0, frame_length(110.0), sine), 0.0, 0.10);
    check_fine(coarse, fine, "A4 +15c", 440.0,
               make_tone(detune(440.0, 15.0), frame_length(440.0), sine),
               15.0, 0.10);
    check_fine(coarse, fine, "E2 -7c", 82.4069,
               make_tone(detune(82.4069, -7.0), frame_length(82.4069), sine),
               -7.0, 0.10);

    std::printf("== harmonic-rich, detuned ==\n");
    check_fine(coarse, fine, "A2 guitar +10c", 110.0,
               make_tone(detune(110.0, 10.0), frame_length(110.0), guitar),
               10.0, 0.30);

    std::printf("== inharmonicity: the headline ==\n");
    // True fundamental partial of a B=4e-4 string sits at f0*sqrt(1+B) ~ +0.35c.
    // The coarse stage read this +10.67c; the fine stage should find ~+0.35c.
    check_fine(coarse, fine, "E1 extreme B=4e-4", 41.2034,
               make_tone(41.2034, frame_length(41.2034), bass, 4e-4),
               0.346, 0.50);
    check_fine(coarse, fine, "E1 realistic B=6e-5", 41.2034,
               make_tone(41.2034, frame_length(41.2034), bass, 6e-5),
               0.052, 0.30);

    std::printf("== noise robustness ==\n");
    check_fine(coarse, fine, "A2 guitar +5c, 20 dB", 110.0,
               make_tone(detune(110.0, 5.0), frame_length(110.0), guitar,
                         /*inharmonicity*/ 0.0, /*snr_db*/ 20.0),
               5.0, 2.0);

    std::printf("== strobe direction (sign of offset) ==\n");
    {
        ++checks_run;
        auto sharp = make_tone(detune(220.0, 8.0), frame_length(220.0), sine);
        auto flat  = make_tone(detune(220.0, -8.0), frame_length(220.0), sine);
        double tgt = hawk::note_to_frequency(
            hawk::frequency_to_note(
                coarse.analyze(sharp.data(), sharp.size()).frequency).midi_note);
        auto fs = fine.refine(sharp.data(), sharp.size(), tgt);
        auto ff = fine.refine(flat.data(),  flat.size(),  tgt);
        const bool ok = fs.valid && ff.valid &&
                        fs.cents > 0.0 && ff.cents < 0.0;
        if (!ok) ++checks_failed;
        std::printf("  [%s] sharp -> %+0.2fc (>0), flat -> %+0.2fc (<0)\n",
                    ok ? "PASS" : "FAIL", fs.cents, ff.cents);
    }

    std::printf("\n%d checks, %d failed\n", checks_run, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
