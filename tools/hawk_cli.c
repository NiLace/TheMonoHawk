// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
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

#include "fine_tuner.h"
#include "note_mapping.h"
#include "pitch_detector.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv)
{
    double sample_rate = (argc > 1) ? atof(argv[1]) : 48000.0;
    if (sample_rate < 8000.0) sample_rate = 48000.0;
    const double a4 = 440.0;

    // Window long enough to resolve low B (~31 Hz needs ~130 ms of audio).
    // 8192 samples at 48 kHz is ~171 ms; the hop sets the refresh rate.
    enum { window_size = 8192, hop_size = 1024 };

    static float window[window_size];
    static float hop[hop_size];

    PitchDetector coarse;
    hawk_pitch_detector_init(&coarse, sample_rate, HAWK_PD_DEFAULTS);
    FineTuner fine;
    hawk_fine_tuner_init(&fine, sample_rate);

    fprintf(stderr,
        "The Hawk - CLI tuner  (%.0f Hz, A4=%.0f)  Ctrl-C to quit\n",
        sample_rate, a4);

    while (true) {
        const size_t got = fread(hop, sizeof(float), hop_size, stdin);
        if (got == 0) break;

        // Slide the window left by `got` samples and append the new ones.
        {
            const size_t k = (got < window_size) ? got : window_size;
            memmove(window, window + k, (window_size - k) * sizeof(float));
            memcpy(window + (window_size - k), hop + (got - k), k * sizeof(float));
        }

        const PitchEstimate c = hawk_pitch_detector_analyze(&coarse, window, window_size);
        if (!c.voiced) {
            printf("\r  --- listening ---%40s", "");
            fflush(stdout);
            continue;
        }

        const NoteReading nr = hawk_frequency_to_note(c.frequency, a4);
        const double target = hawk_note_to_frequency(nr.midi_note, a4);
        const FineEstimate f = hawk_fine_tuner_refine(&fine, window, window_size, target);
        const double cents = f.valid ? f.cents : nr.cents;

        // ASCII needle: 41 cells spanning -50..+50 cents, ':' marks centre,
        // '#' marks the detected pitch.
        enum { width = 41, centre = 20 };
        int pos = (int)lround((cents + 50.0) / 100.0 * (width - 1));
        if (pos < 0)          pos = 0;
        if (pos > width - 1)  pos = width - 1;

        char bar[width + 1];
        memset(bar, ' ', width);
        bar[width]  = '\0';
        bar[centre] = ':';
        bar[pos]    = '#';

        const char* state = (fabs(cents) <= 3.0) ? "IN TUNE"
                          : (cents < 0.0)        ? "flat   "
                                                 : "sharp  ";

        char name[HAWK_NOTE_NAME_LEN];
        printf("\r  %-3s %+6.1f c  [%s]  %s  clr %.2f   ",
               hawk_note_name(nr.midi_note, name), cents,
               bar, state, c.clarity);
        fflush(stdout);
    }

    printf("\n");
    hawk_fine_tuner_free(&fine);
    hawk_pitch_detector_free(&coarse);
    return 0;
}
