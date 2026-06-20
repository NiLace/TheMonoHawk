#pragma once

#include <string>

namespace hawk {

// Result of mapping a detected frequency onto the equal-tempered scale.
struct NoteReading {
    int    midi_note = 0;    // MIDI note number (69 = A4)
    double cents     = 0.0;  // deviation from that note, in range (-50, +50]
    double frequency = 0.0;  // the input frequency (Hz), echoed back
    bool   valid     = false;// false when the input frequency is non-positive
};

// Exact frequency (Hz) of a MIDI note for a given A4 reference pitch.
//   note_to_frequency(69, 440.0) == 440.0
double note_to_frequency(int midi_note, double a4_hz = 440.0);

// Map a frequency (Hz) to the nearest equal-tempered note, given the A4
// reference pitch. Returns the note number and the cents deviation from it.
NoteReading frequency_to_note(double frequency_hz, double a4_hz = 440.0);

// Scientific-pitch name of a MIDI note, e.g. 69 -> "A4", 23 -> "B0", 61 -> "C#4".
std::string note_name(int midi_note);

} // namespace hawk
