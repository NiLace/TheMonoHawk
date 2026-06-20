// Test harness for the MPM pitch detector.
//
// Feed synthetic sine waves at exactly known frequencies, detect them, and
// report the error in cents (1 cent = 1/100 of a semitone). This is the §11
// "synthetic sweep" test: the detector's correctness is a number you can read.

#include "note_mapping.hpp"
#include "pitch_detector.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr double pi          = 3.14159265358979323846;
constexpr double sample_rate = 48000.0;

int checks_run    = 0;
int checks_failed = 0;

// Generate a pure sine of `frequency` Hz, `count` samples long.
std::vector<float> make_sine(double frequency, std::size_t count,
                             double amplitude = 0.5)
{
    std::vector<float> buffer(count);
    for (std::size_t i = 0; i < count; ++i) {
        buffer[i] = static_cast<float>(
            amplitude * std::sin(2.0 * pi * frequency *
                                 static_cast<double>(i) / sample_rate));
    }
    return buffer;
}

// Frame long enough to hold several periods of `frequency`, clamped to a
// sensible range. (The real plugin sizes this adaptively from the coarse
// estimate; here we just give the detector a fair window.)
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

// Detect a pure tone of `frequency` and check the cents error is within
// `tolerance_cents`, and that the frame is reported voiced.
void check_tone(const hawk::PitchDetector& detector,
                const std::string& label,
                double frequency,
                double tolerance_cents)
{
    ++checks_run;
    const std::size_t n      = frame_length(frequency);
    const std::vector<float> s = make_sine(frequency, n);
    const hawk::PitchEstimate e = detector.analyze(s.data(), s.size());

    const double cents = e.voiced ? cents_between(e.frequency, frequency) : 0.0;
    const bool   ok    = e.voiced && std::fabs(cents) <= tolerance_cents;
    if (!ok) ++checks_failed;

    std::printf("  [%s] %-22s f=%9.3f Hz  detected=%9.3f  err=%+7.3f cent  "
                "clarity=%.3f  voiced=%d\n",
                ok ? "PASS" : "FAIL", label.c_str(),
                frequency, e.frequency, cents, e.clarity, e.voiced ? 1 : 0);
}

} // namespace

int main()
{
    const hawk::PitchDetector detector(sample_rate);

    std::printf("== guitar standard tuning (A4 = 440) ==\n");
    check_tone(detector, "E2 low E",  82.4069, 1.0);
    check_tone(detector, "A2",       110.0000, 1.0);
    check_tone(detector, "D3",       146.8324, 1.0);
    check_tone(detector, "G3",       195.9977, 1.0);
    check_tone(detector, "B3",       246.9417, 1.0);
    check_tone(detector, "E4 high E", 329.6276, 1.0);

    std::printf("== bass, including 5-string low B ==\n");
    check_tone(detector, "B0 low B",  30.8677, 1.0);
    check_tone(detector, "E1",        41.2034, 1.0);
    check_tone(detector, "A1",        55.0000, 1.0);

    std::printf("== upper range ==\n");
    check_tone(detector, "A4",       440.0000, 1.0);
    check_tone(detector, "A5",       880.0000, 1.0);
    check_tone(detector, "E6",      1318.5102, 1.0);

    // End-to-end: detector feeds the note mapper. Take A4 sharpened by exactly
    // +15 cents, detect it, and confirm we recover "A4, +15 cents".
    std::printf("== end-to-end: detect -> note + cents ==\n");
    {
        ++checks_run;
        const double freq = hawk::note_to_frequency(69) *
                            std::pow(2.0, 15.0 / 1200.0);
        const std::size_t n = frame_length(freq);
        const std::vector<float> s = make_sine(freq, n);
        const hawk::PitchEstimate e = detector.analyze(s.data(), s.size());
        const hawk::NoteReading r = hawk::frequency_to_note(e.frequency);

        const bool ok = e.voiced && r.midi_note == 69 &&
                        std::fabs(r.cents - 15.0) <= 1.0;
        if (!ok) ++checks_failed;
        std::printf("  [%s] A4 +15c            -> %s %+6.2f cents\n",
                    ok ? "PASS" : "FAIL",
                    hawk::note_name(r.midi_note).c_str(), r.cents);
    }

    // Silence must read as unvoiced, not chase noise into a phantom pitch.
    std::printf("== gating ==\n");
    {
        ++checks_run;
        const std::vector<float> silence(4096, 0.0f);
        const hawk::PitchEstimate e = detector.analyze(silence.data(),
                                                       silence.size());
        const bool ok = !e.voiced;
        if (!ok) ++checks_failed;
        std::printf("  [%s] silence             -> voiced=%d (want 0)\n",
                    ok ? "PASS" : "FAIL", e.voiced ? 1 : 0);
    }

    std::printf("\n%d checks, %d failed\n", checks_run, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
