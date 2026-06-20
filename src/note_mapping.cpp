#include "note_mapping.hpp"

#include <array>
#include <cmath>

namespace hawk {

namespace {

// Twelve-tone equal temperament: a semitone is the 12th root of 2.
constexpr double semitones_per_octave = 12.0;

// MIDI note 69 is A4 (the reference pitch). All mapping is relative to it.
constexpr int a4_midi_note = 69;

} // namespace

double note_to_frequency(int midi_note, double a4_hz)
{
    const double semitones_from_a4 =
        static_cast<double>(midi_note - a4_midi_note);
    return a4_hz * std::pow(2.0, semitones_from_a4 / semitones_per_octave);
}

NoteReading frequency_to_note(double frequency_hz, double a4_hz)
{
    NoteReading reading;
    reading.frequency = frequency_hz;

    // A non-positive frequency has no musical meaning; report it as invalid
    // rather than feeding it into log2() and returning nonsense.
    if (frequency_hz <= 0.0 || a4_hz <= 0.0) {
        return reading;
    }

    // Real-valued MIDI position: how many semitones above A4 this frequency is,
    // offset so that A4 lands on its MIDI number.
    const double midi_real =
        a4_midi_note +
        semitones_per_octave * std::log2(frequency_hz / a4_hz);

    const int nearest = static_cast<int>(std::lround(midi_real));

    reading.midi_note = nearest;
    reading.cents     = (midi_real - nearest) * 100.0;
    reading.valid     = true;
    return reading;
}

std::string note_name(int midi_note)
{
    static const std::array<const char*, 12> names = {
        "C", "C#", "D", "D#", "E", "F",
        "F#", "G", "G#", "A", "A#", "B"
    };

    // Floor-divide toward negative infinity so that octave numbering stays
    // correct for any note. In scientific pitch notation MIDI 0 is C-1.
    const int note_index = ((midi_note % 12) + 12) % 12;
    const int octave     = (midi_note / 12) - 1 -
                           ((midi_note % 12 < 0) ? 1 : 0);

    return std::string(names[note_index]) + std::to_string(octave);
}

} // namespace hawk
