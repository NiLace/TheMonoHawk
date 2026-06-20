# Polyphonic per-string tuning — how it works

This is a write-up of the method used to detect and tune every string of a
strummed guitar or bass at once. The working code is **not** published, because
polyphonic instrument tuning is covered by active patents (see the README). The
*method*, however, is just signal processing and physics — most of it already
published — so it's written down here freely. Build a product from it at your own
legal risk; this document is for understanding, not a licence to ship.

## The problem

Tuning one note is well understood: find the fundamental, measure how far it is
from the target pitch. A strummed chord is harder, because the strings share
overtones. On a guitar in standard tuning the low E's 4th harmonic (≈330 Hz)
lands right on top of the high E's fundamental, A's 3rd harmonic lands on the
same E, and so on. A string can be completely *buried* under a louder string's
partial, so you can't just look for a peak at its nominal frequency.

The foundation here is the spectral-irregularity idea of Zhou & Reiss [1]:
instead of trusting raw spectral peaks, reason about which peaks *belong* to which
note by their harmonic structure, and treat collisions explicitly.

## Step 1 — a collision map, derived from the tuning

For a given tuning, every open string is classified by how (if at all) it
collides with a *lower* string's harmonics. With equal-tempered intervals you can
work this out with integer math, before any audio arrives:

- **Clean** — no lower string's harmonic sits on this string's fundamental. Read
  it directly.
- **Trapped (n ≥ 3)** — a lower string's 3rd, 4th, … harmonic lands on this
  string's fundamental (e.g. standard B3 under E2's 3rd; E4 under E2's 4th *and*
  A2's 3rd). Recoverable — see Step 3.
- **Octave-trapped (n = 2)** — the fundamental sits on a lower string's 2nd
  harmonic, one octave up (common in drop and open tunings). Spectrally
  unresolvable — see Step 4.
- **Unison (n = 1)** — two strings at the same pitch (e.g. Ostrich tuning). Not
  separable from a single channel; left undetected rather than guessed.

The map is rebuilt whenever the tuning changes, so the method generalises beyond
standard tuning.

## Step 2 — clean strings: cents from agreeing harmonics

For a string whose fundamental is in the clear, the offset is read off the
spectral peak. The catch on low strings: the fundamental sits in a broad, crowded
region and a single-peak read can drift. The fix is that **a string's cents
offset is identical at every harmonic** — so read it at *every* harmonic that is
clear of every other ringing string's partials, undo each one's small inharmonic
stretch, and take the largest mutually-agreeing cluster. Prefer the unbiased
fundamental when it sits inside that cluster; fall back to the harmonic consensus
only when the fundamental itself is the outlier (a weak low-string fundamental
that railed). If nothing agrees, report *unconfident* rather than a confident
lie.

## Step 3 — trapped strings: inharmonicity as a wedge

A trapped string's fundamental shares a frequency with a lower string's harmonic,
so a short window sees one blurred peak. But **real strings are inharmonic**: a
stiff string's nth partial sits not at `n·f0` but at roughly

```
f_n ≈ n·f0·sqrt(1 + B·n²)
```

where `B` (~1e-4…3e-4 for guitar) is the inharmonicity coefficient. So the lower
string's *high* colliding partial is stretched **sharp**, while the trapped
string's own *fundamental* (n = 1) is barely stretched and sits near nominal.
They are at slightly different frequencies — and with a long, high-resolution
window (~1.8 s) they separate into two peaks.

The procedure:
1. Measure the lower (contaminating) string's `B` and `f0` from two of its own
   clean partials.
2. Predict exactly where its colliding partial lands: `n·f0·sqrt(1 + B·n²)`.
3. In the trapped string's region, take the nearby peak that is **not** that
   predicted contaminant. Its frequency gives the cents directly.

The separation scales as roughly `866·B·(n² − 1)` cents, so the double-octave
(n = 4) is easy, the twelfth (n = 3) is hard but doable (~1–2 cents apart, near
the resolution floor), and the octave (n = 2) is hopeless this way — which is why
it needs a different weapon.

## Step 4 — octave-trapped strings: beat in the time domain

For a direct octave (n = 2), the upper string's fundamental and the lower
string's 2nd partial are only ~0.06 Hz apart when in tune — no practical window
resolves that. So drop the spectrum and use time: the two near-identical
frequencies **beat**, and the beat rate equals `|f_upper − f_lower2nd|`, which
*grows with the upper string's detuning*. That is exactly when a tuner needs to
see something.

1. Fit the lower string's inharmonicity from its **1st and 3rd** partials (not
   the 2nd — that's the trap) and predict its 2nd partial.
2. Heterodyne the long window down at that predicted partial; complex low-pass to
   isolate the beat envelope.
3. The envelope's modulation rate (a small FFT of the decimated envelope) gives
   the detuning magnitude; the spectral centroid around the partial gives the
   sign (sharp vs flat).

This recovers an octave-trapped string to a few cents on real strings, even in a
full strum. Its honest limit: a small deadzone near perfect tune, where the beat
is too slow to clock within the window — but an unmeasurable beat *means* in
tune, so the failure mode is benign.

## Mode gating and honest limits

Trapped/octave reads are only trustworthy inside a real strum (the contaminating
strings must be ringing), so they're gated to "enough strings present"; a single
note falls back to the ordinary monophonic detector. Known soft spots: the
double-trapped high E (buried under two lower strings at once) is the hardest
case; the octave deadzone above; and dense low-bass chords, where cents are
reported honestly-or-not-at-all rather than confidently wrong.

## References

[1] R. Zhou, J. D. Reiss, M. Mattavelli, and G. Zoia, "A Computationally
Efficient Method for Polyphonic Pitch Estimation," *EURASIP Journal on Advances
in Signal Processing*, vol. 2009, Article ID 729494. doi:10.1155/2009/729494

The inharmonicity model is standard stiff-string physics (Fletcher & Rossing,
*The Physics of Musical Instruments*); the two-peak inharmonic separation and the
time-domain octave-beat resolver are this project's own application of it.
