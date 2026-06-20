// Reliability stress tests for the MPM pitch detector.
//
// Pure sines (test_pitch_detector.cpp) prove precision on the easy case. Real
// strings are not sines: they have a stack of harmonics, sometimes a weak or
// missing fundamental, an attack/decay envelope, and noise. This is where
// octave errors and jitter live. Every signal here is deterministic, so the
// pass/fail numbers are reproducible.

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

double cents_between(double detected, double reference)
{
    return 1200.0 * std::log2(detected / reference);
}

double rms_of_harmonics(const std::vector<double>& amps)
{
    double sum_sq = 0.0;
    for (double a : amps) sum_sq += a * a;
    return std::sqrt(sum_sq / 2.0);  // RMS of a sum of incoherent sinusoids
}

// Build a realistic tone: a stack of harmonics of f0, with optional string
// inharmonicity (stretched partials), an optional exponential decay envelope,
// and optional white noise at a target SNR. Deterministic for a given seed.
std::vector<float> make_tone(double f0,
                             std::size_t count,
                             const std::vector<double>& harmonic_amps,
                             double inharmonicity_b   = 0.0,
                             double decay_tau_seconds = 0.0,
                             double snr_db            = 1e9,
                             std::uint32_t seed       = 1234u)
{
    std::vector<float> buffer(count, 0.0f);
    std::mt19937 rng(seed);
    std::normal_distribution<double> gaussian(0.0, 1.0);

    const double sig_rms   = rms_of_harmonics(harmonic_amps);
    const double noise_amp = (snr_db < 200.0)
        ? sig_rms / std::pow(10.0, snr_db / 20.0)
        : 0.0;

    for (std::size_t i = 0; i < count; ++i) {
        const double t = static_cast<double>(i) / sample_rate;
        double sample = 0.0;
        for (std::size_t h = 0; h < harmonic_amps.size(); ++h) {
            const int    n  = static_cast<int>(h) + 1;
            double       fn = n * f0;
            if (inharmonicity_b > 0.0) {
                fn = n * f0 * std::sqrt(1.0 + inharmonicity_b * n * n);
            }
            sample += harmonic_amps[h] * std::sin(2.0 * pi * fn * t);
        }
        if (decay_tau_seconds > 0.0) {
            sample *= std::exp(-t / decay_tau_seconds);
        }
        if (noise_amp > 0.0) {
            sample += noise_amp * gaussian(rng);
        }
        buffer[i] = static_cast<float>(0.3 * sample);
    }
    return buffer;
}

// Check that a voiced signal lands on the *correct note* (no octave error) and
// within `tol_cents` of f0.
void check_voiced(const hawk::PitchDetector& detector,
                  const std::string& label,
                  double f0,
                  const std::vector<float>& samples,
                  double tol_cents)
{
    ++checks_run;
    const hawk::PitchEstimate e = detector.analyze(samples.data(),
                                                   samples.size());
    const int expected_note = hawk::frequency_to_note(f0).midi_note;
    const hawk::NoteReading got = hawk::frequency_to_note(e.frequency);
    const double cents = e.voiced ? cents_between(e.frequency, f0) : 0.0;

    const bool ok = e.voiced &&
                    got.midi_note == expected_note &&
                    std::fabs(cents) <= tol_cents;
    if (!ok) ++checks_failed;

    std::printf("  [%s] %-26s f0=%9.3f -> %-4s %+7.2fc  "
                "(detected %9.3f Hz, clarity %.3f, voiced %d)\n",
                ok ? "PASS" : "FAIL", label.c_str(), f0,
                e.voiced ? hawk::note_name(got.midi_note).c_str() : "----",
                cents, e.frequency, e.clarity, e.voiced ? 1 : 0);
}

// Check octave-correctness only. The coarse MPM stage's contract is to identify
// the right note; exact cents under heavy inharmonicity is the fine stage's job
// (design-spec.md §3.2), which does not exist yet. Cents are printed for
// information but not asserted here.
void check_note_only(const hawk::PitchDetector& detector,
                     const std::string& label,
                     double f0,
                     const std::vector<float>& samples)
{
    ++checks_run;
    const hawk::PitchEstimate e = detector.analyze(samples.data(),
                                                   samples.size());
    const int expected_note = hawk::frequency_to_note(f0).midi_note;
    const hawk::NoteReading got = hawk::frequency_to_note(e.frequency);
    const double cents = e.voiced ? cents_between(e.frequency, f0) : 0.0;

    const bool ok = e.voiced && got.midi_note == expected_note;
    if (!ok) ++checks_failed;

    std::printf("  [%s] %-26s f0=%9.3f -> %-4s  (note octave checked; "
                "cents %+.2f informational, awaits fine stage)\n",
                ok ? "PASS" : "FAIL", label.c_str(), f0,
                e.voiced ? hawk::note_name(got.midi_note).c_str() : "----",
                cents);
}

void check_unvoiced(const hawk::PitchDetector& detector,
                    const std::string& label,
                    const std::vector<float>& samples)
{
    ++checks_run;
    const hawk::PitchEstimate e = detector.analyze(samples.data(),
                                                   samples.size());
    const bool ok = !e.voiced;
    if (!ok) ++checks_failed;
    std::printf("  [%s] %-26s -> voiced %d (want 0, clarity %.3f)\n",
                ok ? "PASS" : "FAIL", label.c_str(),
                e.voiced ? 1 : 0, e.clarity);
}

} // namespace

int main()
{
    const hawk::PitchDetector detector(sample_rate);

    // A plucked-string-like harmonic stack and some pathological variants.
    const std::vector<double> guitar   = {1.0, 0.6, 0.45, 0.3, 0.22, 0.16,
                                          0.12, 0.09};
    const std::vector<double> bass     = {1.0, 0.85, 0.7, 0.55, 0.45, 0.35,
                                          0.28, 0.22, 0.18};
    const std::vector<double> missing  = {0.0, 0.9, 0.7, 0.5, 0.35, 0.25};
    const std::vector<double> octtrap  = {0.3, 1.0, 0.5, 0.35, 0.25};

    std::printf("== full harmonic stack (realistic timbre) ==\n");
    for (auto [name, f0] : std::vector<std::pair<std::string,double>>{
            {"E2 low E", 82.4069}, {"A2", 110.0}, {"D3", 146.8324},
            {"G3", 195.9977}, {"B3", 246.9417}, {"E4 high E", 329.6276}}) {
        check_voiced(detector, name, f0,
                     make_tone(f0, frame_length(f0), guitar), 1.0);
    }

    std::printf("== missing fundamental (octave-error trap) ==\n");
    check_voiced(detector, "E2 no fundamental", 82.4069,
                 make_tone(82.4069, frame_length(82.4069), missing), 1.0);
    check_voiced(detector, "A1 no fundamental", 55.0,
                 make_tone(55.0, frame_length(55.0), missing), 1.0);
    check_voiced(detector, "B0 no fundamental", 30.8677,
                 make_tone(30.8677, frame_length(30.8677), missing), 1.0);

    std::printf("== 2nd harmonic louder than 1st (octave-up trap) ==\n");
    check_voiced(detector, "A2 octave trap", 110.0,
                 make_tone(110.0, frame_length(110.0), octtrap), 1.0);
    check_voiced(detector, "E2 octave trap", 82.4069,
                 make_tone(82.4069, frame_length(82.4069), octtrap), 1.0);

    std::printf("== decaying note ==\n");
    check_voiced(detector, "A2 decay tau=0.25s", 110.0,
                 make_tone(110.0, frame_length(110.0), guitar, 0.0, 0.25), 1.5);

    std::printf("== string inharmonicity (stretched partials) ==\n");
    // Realistic wound-bass inharmonicity: the coarse stage must stay tight.
    check_voiced(detector, "E1 bass realistic B=6e-5", 41.2034,
                 make_tone(41.2034, frame_length(41.2034), bass, 6e-5), 3.0);
    // Piano-grade extreme inharmonicity: octave must be right; exact cents is
    // the fine stage's job, so we only assert the note here.
    check_note_only(detector, "E1 extreme B=4e-4", 41.2034,
                    make_tone(41.2034, frame_length(41.2034), bass, 4e-4));

    std::printf("== noise robustness ==\n");
    check_voiced(detector, "A2 + noise 20 dB", 110.0,
                 make_tone(110.0, frame_length(110.0), guitar, 0.0, 0.0, 20.0),
                 5.0);
    check_voiced(detector, "E2 + noise 12 dB", 82.4069,
                 make_tone(82.4069, frame_length(82.4069), guitar, 0.0, 0.0,
                           12.0),
                 15.0);

    std::printf("== gating: noise only must read unvoiced ==\n");
    {
        std::mt19937 rng(99u);
        std::normal_distribution<double> g(0.0, 1.0);
        std::vector<float> noise(8192);
        for (auto& s : noise) s = static_cast<float>(0.1 * g(rng));
        check_unvoiced(detector, "white noise only", noise);
    }

    std::printf("\n%d checks, %d failed\n", checks_run, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
