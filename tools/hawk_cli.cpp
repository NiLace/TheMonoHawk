// The Hawk — command-line tuner.
//
// NOT the LV2 plugin: it has no GUI and you cannot insert it on a track. It is
// the real detection core (pitch_detector + fine_tuner) wired to a terminal so
// you can point an instrument at it today. It reads raw 32-bit float mono audio
// from stdin and prints the note, cents, and an ASCII strobe/needle live.
//
// Example (ALSA capture of your interface):
//   arecord -f FLOAT_LE -c 1 -r 48000 -t raw | ./build/tools/hawk_cli
//
// Optional argument: sample rate (default 48000).

#include "fine_tuner.hpp"
#include "note_mapping.hpp"
#include "pitch_detector.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    double sample_rate = (argc > 1) ? std::atof(argv[1]) : 48000.0;
    if (sample_rate < 8000.0) sample_rate = 48000.0;
    const double a4 = 440.0;

    // Window long enough to resolve low B (~31 Hz needs ~130 ms of audio).
    // 8192 samples at 48 kHz is ~171 ms; the hop sets the refresh rate.
    const std::size_t window_size = 8192;
    const std::size_t hop_size    = 1024;

    std::vector<float> window(window_size, 0.0f);
    std::vector<float> hop(hop_size);

    const hawk::PitchDetector coarse(sample_rate);
    const hawk::FineTuner     fine(sample_rate);

    std::fprintf(stderr,
        "The Hawk - CLI tuner  (%.0f Hz, A4=%.0f)  Ctrl-C to quit\n",
        sample_rate, a4);

    while (true) {
        const std::size_t got = std::fread(hop.data(), sizeof(float),
                                           hop_size, stdin);
        if (got == 0) break;

        // Slide the window left by `got` samples and append the new ones.
        if (got < window_size) {
            std::memmove(window.data(), window.data() + got,
                         (window_size - got) * sizeof(float));
            std::memcpy(window.data() + (window_size - got), hop.data(),
                        got * sizeof(float));
        } else {
            std::memcpy(window.data(), hop.data() + (got - window_size),
                        window_size * sizeof(float));
        }

        const hawk::PitchEstimate c = coarse.analyze(window.data(),
                                                     window_size);
        if (!c.voiced) {
            std::printf("\r  --- listening ---%40s", "");
            std::fflush(stdout);
            continue;
        }

        const hawk::NoteReading nr = hawk::frequency_to_note(c.frequency, a4);
        const double target = hawk::note_to_frequency(nr.midi_note, a4);
        const hawk::FineEstimate f = fine.refine(window.data(), window_size,
                                                 target);
        const double cents = f.valid ? f.cents : nr.cents;

        // ASCII needle: 41 cells spanning -50..+50 cents, ':' marks centre,
        // '#' marks the detected pitch.
        const int width  = 41;
        const int centre = 20;
        int pos = static_cast<int>(
            std::lround((cents + 50.0) / 100.0 * (width - 1)));
        if (pos < 0)          pos = 0;
        if (pos > width - 1)  pos = width - 1;

        std::string bar(width, ' ');
        bar[centre] = ':';
        bar[pos]    = '#';

        const char* state = (std::fabs(cents) <= 3.0) ? "IN TUNE"
                          : (cents < 0.0)             ? "flat   "
                                                      : "sharp  ";

        std::printf("\r  %-3s %+6.1f c  [%s]  %s  clr %.2f   ",
                    hawk::note_name(nr.midi_note).c_str(), cents,
                    bar.c_str(), state, c.clarity);
        std::fflush(stdout);
    }

    std::printf("\n");
    return 0;
}
