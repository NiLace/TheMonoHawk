# Changelog

## 0.2.0 — 2026-10-23

The first release since 0.1, and a large one: a new interface, a strobe view, and a detector that holds up when
real playing gets messy — re-plucks, handling rumble, notes stopped dead, dropouts, bad input. Same 50 tunings.

> **Upgrading from 0.1:** the plugin's URI changed and three ports were added, so a host sees 0.2 as a different
> plugin. In a session made with 0.1, **remove the old plugin and add the new one**. Install the new bundle over
> the old folder (`~/.lv2/themonohawk.lv2/`) and restart the host.

### New

- **A new interface: a rack unit.** A photographed front panel with LED screens: the note screen, the chevron
  meter with its pointer, a continuous amber input-level line, the string row as a backlit checklist (white while a
  string sounds, amber once it is tuned, the whole row and the hawk glowing when the last one lands), and four
  buttons — MUTE, TONE, STROBE, SETTINGS. The A4 reference and the tuning list open as smoked-glass windows over
  the panel, and so do Settings and Detect Tuning.
- **Strobe view** (STROBE): the rim of a turning disc seen through the meter's window. Its stripes slide right when
  you are sharp and left when you are flat, faster the further off you are, and stand still in tune; it keeps
  turning through a re-pluck instead of stopping.
- **Bypass from the panel**: the hawk and the name under the buttons switch the plugin's bypass, which is the same
  switch as the host's own (the plugin now has an `enabled` port). While bypassed the hawk goes dark and the name
  turns white.
- **The hawk lights up**: amber while the plugin runs, brighter amber when a string is in tune.
- With TONE on, the cents slot shows the reference note being played.
- The editor follows the host's interface scale (`ui:scaleFactor`).
- Two new ports for other displays: the **strobe phase** output (still in tune, rising when sharp, falling when
  flat) and the **strobe view** switch.

### Better readings

- **A re-pluck no longer kicks the needle.** A pluck starts sharp and the pick clicks; that attack used to move the
  reading by 9–15 cents for almost half a second and blink the in-tune lamp. The analysis now skips the first
  100 ms after an attack: the needle stays where it was (within a cent), and the lamp does not blink.
- **Handling rumble no longer changes the note.** Low-frequency rumble and DC on the input (knocking the
  instrument, a cable, a DI) could make the tuner show a wrong note, or silence a low bass string altogether. The
  input now goes through a 20 Hz high-pass first, and an analysis that disagrees with itself publishes nothing
  instead of a wrong note.
- **A note stopped dead stops at once.** Muting a string used to leave about 170 ms of drifting reading with the
  in-tune lamp still lit; now the reading drops as soon as the note does.
- **Between two notes the cents could belong to the previous one**: a string swept across the E/F boundary briefly
  read "F +47 c" for a pitch 48 cents below F. A pitch more than 50 cents away from the note it was first heard as
  is now read as the nearer note, and the reading restarts with it.
- **A dropout no longer becomes a wrong note.** If the host stalled long enough for audio to be lost, the detector
  could analyse the two pieces on either side of the gap as one sound and show a pitch nobody played. It now waits
  for clean audio, including when the stall repeats before it has caught up.
- **The display no longer contradicts itself.** The note, the cents and the frequency could come from two different
  analyses for a moment, showing a combination no single reading produced.
- **Detect Tuning** kept only the first string it heard and could name a 4-string bass for a guitar tuned to Drop D.
  It now takes each new string you play — let it ring for a moment, or mute it quickly — ignores strums, and breaks
  ties in favour of the common tunings. Its window says how to use it.
- While bypassed, the note, cents, frequency and strobe outputs read zero (they kept the last reading).

### Fixed

- **One bad input sample deafened the tuner.** A single NaN or infinite sample from the host silenced the detector
  until the plugin was restarted, and pinned the level meter at full. Such samples are now treated as silence.
- **Out-of-range controls are clamped.** A Dim Amount above 0 dB used to amplify (×10 at +20 dB), and a NaN control
  value could fill the output with NaN; every control is now held to its declared range, and a NaN takes the default.
- **The host is protected.** An X error on the interface's own connection no longer ends the host; a host that
  activates the plugin twice, refuses it a thread, or asks for an unsupported sample rate gets an error instead of a
  crash; the interface ignores NaN or infinite values from the host.
- **Stopping and restarting the plugin starts it clean.** After the host deactivated and re-activated it, the last
  note, the frequency and a lit in-tune lamp could stay on screen with nothing coming in. Stopping is also immediate
  (deactivating waited for the analysis thread's idle tick).
- Builds with GCC 16.

### Faster

- **At rest it costs about a tenth of what 0.1 did**: with the plugin running and nothing coming in, 1.87 % of one
  core in 0.1, 0.22 % now (Intel i7-8700K, 48 kHz, five paired runs). The analysis thread no longer works through
  denormal numbers on silence, and it skips windows with no signal in them.
- It stopped allocating memory while it runs (130 allocations per second in 0.1, none now).

### Changed

- **The plugin's URI is now `https://nylarea.com/lv2/themonohawk`** (0.1 was `https://themonohawk.audio/lv2/tuner`),
  and its name in the host is **The Mono Hawk** (0.1 was "The Mono Hawk Tuner").
- Three ports were added — the strobe phase, the strobe view and the bypass — for 23 in all.
- Licence: **AGPL-3.0-only** (0.1 was published under GPL-3.0-or-later). See the README.
- Building needs the FreeType development headers (the panel's typeface, Oswald, ships in the bundle with its OFL
  licence).

## 0.1 — 2026-06-20

First release.
