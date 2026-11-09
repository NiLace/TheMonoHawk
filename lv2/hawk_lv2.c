// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
// The Hawk — LV2 plugin (DSP / audio side). The faceplate GUI lives separately
// in hawk_ui.c; this file has no UI code.
//
// Passes audio through (with an optional smoothed dim/mute), and exposes the
// detected note, cents, clarity and frequency as LV2 output control ports so a
// host's generic UI can display them. The heavy detection runs on a background
// worker thread; the audio thread only copies samples into a lock-free ring
// buffer and reads back the latest result, so run() stays real-time safe.

#include "fine_tuner.h"
#include "math_constants.h"
#include "note_mapping.h"
#include "pitch_detector.h"
#include "ring_buffer.h"
#include "tunings.h"

#include <lv2/core/lv2.h>

#include <errno.h>
#include <float.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>   // the HAWK_DEBUG_CENTS traces (a lab switch: no build defines it)
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__SSE__)
#include <xmmintrin.h>
#endif

#define HAWK_URI "https://nylarea.com/lv2/themonohawk"

enum PortIndex {
    PORT_IN = 0,
    PORT_OUT,
    PORT_ACTIVE,
    PORT_DIM_DB,
    PORT_NOTE,
    PORT_CENTS,
    PORT_CLARITY,
    PORT_FREQ,
    PORT_VOICED,
    PORT_TOLERANCE,
    PORT_RAISE,
    PORT_IN_TUNE,
    PORT_LOWER,
    PORT_A4,
    PORT_TUNING,
    PORT_REF_ACTIVE,
    PORT_REF_NOTE,
    PORT_PING_ENABLE,
    PORT_LEVEL,
    PORT_SENSITIVITY,
    PORT_STROBE,
    PORT_STROBE_MODE,
    PORT_ENABLED,
};

// The analysis result, as one observation (see r_seq in Hawk).
typedef struct Reading {
    float note;
    float cents;
    float clarity;
    float freq;
    float strobe;   // radians, (-pi, pi]: see the worker
    int   voiced;
} Reading;

// Zeroed at instantiation (calloc); hawk_defaults() then sets every member whose rest value is not zero.
typedef struct Hawk {
    double sample_rate;
    // The analysis runs at sample_rate / decim, 44.1-48 kHz for the common rates (see Decimator).
    double   analysis_rate;
    unsigned decim;

    // Ports.
    const float* in        ;
    float*       out       ;
    const float* active    ;
    const float* dim_db    ;
    float*       p_note    ;
    float*       p_cents   ;
    float*       p_clarity ;
    float*       p_freq    ;
    float*       p_voiced  ;
    const float* tolerance ;
    const float* p_a4      ;
    const float* p_tuning  ;  // selected tuning index (the ping gate checks its strings)
    const float* ref_active ; // reference-tone on/off (UI toggle)
    const float* ref_note   ; // reference-tone target, MIDI note
    const float* ping_enable ; // in-tune ping on/off (Advanced Settings)
    float*       p_level    ;  // input level 0..1 (output, for the UI meter)
    const float* sensitivity ; // detection sensitivity 0..1 (input)
    float*       p_raise   ;
    float*       p_in_tune ;
    float*       p_lower   ;
    float*       p_strobe  ;  // the string's phase against the reference note (output)
    const float* strobe_mode ; // the GUI's strobe view on/off: the DSP ignores it, the host saves it
    const float* enabled     ; // lv2:enabled -- 0 = bypassed (the hawk in the GUI); unconnected = on

    // Bypass bookkeeping (audio thread). Coming back from a bypass, what the worker last published is from
    // before it: hide it until the worker publishes again (r_seq / p_seq move).
    bool     was_enabled;
    bool     stale;
    uint32_t stale_seq;

    // Smoothed dim/mute gain.
    float current_gain;

    // Input-level meter (audio thread): peak-follower with instant attack and a
    // slow release, exposed on p_level so the UI can show "is it hearing me".
    float level_env;

    // Reference-tone oscillator (audio thread): a sine mixed under the
    // passthrough so the player can tune by ear. ref_osc_gain is ramped in/out
    // so engaging/releasing the tone never clicks; the rotation below keeps the
    // sine continuous across blocks (and across pitch changes).
    float  ref_osc_gain;

    // Confirmation "ping" (audio thread): a short bell tone fired on the rising
    // edge of the in-tune state, so the player hears each string land in tune.
    // ping_env is a fast-attack/exponential-decay envelope; 0 means silent (so
    // idle passthrough stays bit-exact).
    /* Both oscillators step a complex rotation instead of calling sin() per sample: with
     * sin(), the running reference tone took run() from 13.98 to 38.81 ns/sample (+178 %).
     * z = (cos phase, sin phase); multiplying by e^{j dphase} advances it. sin(2p) comes out free
     * as 2*sin(p)*cos(p), which is what the ping's second partial needs.
     * ⚠ A rotation drifts in magnitude, so run() renormalises it once per block. */
    double ref_zr, ref_zi, ref_step_c, ref_step_s, ref_step_for;
    double png_zr, png_zi, png_step_c, png_step_s, png_step_for;
    float  ping_env;

    // Per-block constants that depend only on the sample rate, computed once in instantiate().
    float gain_coeff;      // one-pole ~10 ms: the dim and the reference tone's fades
    float ping_decay;      // ping envelope, ~0.13 s
    float level_release;   // level meter release, ~0.3 s

    // Tuning-lamp stabiliser state (see run()): -1 none, 0 raise, 1 in tune,
    // 2 lower.
    int      tune_zone;
    int      cand_zone;
    uint32_t cand_samps;
    uint32_t release_samps;
    uint32_t in_tune_samps;  // how long we've been latched "in tune"

    // Detection, run off the audio thread. The ring holds the HOST-rate stream (decimated by the worker), so at
    // 192 kHz it fills four times faster than at 48 kHz: sized for that headroom.
    RingBuffer         ring;   // 1 << 16 samples
    pthread_t          worker;
    bool               worker_started;   // a joinable `worker` exists
    _Atomic bool       running;
#if HAWK_JUDGE_SYNC
    _Atomic bool       judge_idle;   // the port judge's handshake only: see run()
#endif
    /* With the ring empty the worker waits on a condition variable with a 3 ms timeout, and
     * deactivate() notifies it, so a stop is seen immediately instead of on the next tick (a flat
     * 3 ms sleep made deactivate() wait 2.92 ms on average, 3.23 ms worst).
     *
     * ⚠ ONLY THE STOP IS SIGNALLED, deliberately. The obvious extension is to have run() notify
     * the worker whenever it pushes audio, which would remove the idle poll entirely -- but that
     * puts a mutex and a futex wake on the AUDIO THREAD, which is exactly what a real-time path
     * must not do. The 3 ms timed wait stays as the data path; it costs a wakeup every 3 ms on a
     * thread that is idle anyway. Closing fast is worth a condition variable; feeding is not. */
    pthread_mutex_t    wake_m;
    pthread_cond_t     wake_cv;   // CLOCK_MONOTONIC
    /* Where in the stream the audio was lost, not merely that it was.
     *
     * The first version of this stored a drop COUNT and had the worker reset as soon as it saw a
     * non-zero value. That is wrong, and the probe that was meant to prove it could not tell the
     * difference. The consumer runs up to a ring-full -- 1.365 s -- behind the producer, so when it
     * learns of a drop, everything it is holding is still contiguous: it would throw away good
     * audio, clear the flag, and then walk straight into the actual splice with nothing left to
     * warn it. The gap has to be named by POSITION and acted on when the read pointer passes it.
     *
     * And by more than one position: a single slot is overwritten when the ring
     * overflows twice before the worker gets there, and the first splice is then crossed unseen --
     * measured with the gap probe under ASan, where this worker is slow enough: 110 Hz published.
     * GapTracker keeps the first and last unhandled seam; see ring_buffer.h. */
    GapTracker gaps;

    /* The analysis result is ONE observation, so it has to cross the thread boundary as
     * one. It used to be five independent atomics stored one after another, which means the audio
     * thread could sample between two of them and light the display with a MIXTURE of two
     * analyses -- the new note against the old cents, say, or the new frequency while voiced is
     * still 0. Measured on the real plugin: 7 such windows in 900 blocks with the note changing.
     * Each is narrow, and none of them is a crash; what they produce is a reading that is
     * internally impossible, which on a tuner is exactly the wrong kind of wrong.
     *
     * A seqlock fits because the traffic is one writer, one reader, and the reader may retry:
     * the counter goes odd before the payload is touched and even after, so a reader that sees
     * the same even value on both sides knows nothing moved underneath it.
     *
     * ⚠ The payload fields stay atomic (relaxed) rather than being plain floats. The seqlock
     * gives CONSISTENCY, not a licence to race: reading a plain object while the writer is in it
     * is a data race whatever the counter says, and sanitisers are right to call it. Relaxed
     * atomics cost nothing here and make the code mean what it says. */
    _Atomic uint32_t r_seq;      // odd while the writer is inside
    _Atomic float    r_note;
    _Atomic float    r_cents;
    _Atomic float    r_clarity;
    _Atomic float    r_freq;
    _Atomic float    r_strobe;
    _Atomic int      r_voiced;

    Reading last_reading;     // audio thread only: what to fall back to
    _Atomic float a4_ref;   // tuning reference (audio thread -> worker), 440 at rest
    _Atomic float sens;     // detection sensitivity 0..1 (audio -> worker), 0.5 at rest
    _Atomic int   tuning_idx;   // selected tuning index (audio -> worker / ping gate)

} Hawk;

// Worker side. Only the worker thread calls this, so the counter needs no read-modify-write.
static void hawk_publish(Hawk* self, const Reading* v)
{
    const uint32_t s = atomic_load_explicit(&self->r_seq, memory_order_relaxed);
    atomic_store_explicit(&self->r_seq, s + 1, memory_order_relaxed);          // open: odd
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&self->r_note, v->note, memory_order_relaxed);
    atomic_store_explicit(&self->r_cents, v->cents, memory_order_relaxed);
    atomic_store_explicit(&self->r_clarity, v->clarity, memory_order_relaxed);
    atomic_store_explicit(&self->r_freq, v->freq, memory_order_relaxed);
    atomic_store_explicit(&self->r_strobe, v->strobe, memory_order_relaxed);
    atomic_store_explicit(&self->r_voiced, v->voiced, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&self->r_seq, s + 2, memory_order_release);          // closed: even
}

/* Audio side. `fallback` is what to use if the writer happens to be inside right now.
 *
 * ⚠ The classic seqlock reader retries until it wins. An AUDIO THREAD must not: however
 * unlikely, "spin until another thread finishes" is an unbounded wait on a path with a
 * deadline. The writer's critical section is five relaxed stores, so a couple of attempts
 * covers everything short of the worker being preempted mid-publish -- and if that does
 * happen, showing last block's reading for 5 ms is invisible on a tuner display, while a
 * missed buffer is not. */
static Reading hawk_read_result(Hawk* self, const Reading* fallback)
{
    for (int tries = 0; tries < 4; ++tries) {
        const uint32_t s0 = atomic_load_explicit(&self->r_seq, memory_order_acquire);
        if (s0 & 1u) continue;                     // writer is inside
        Reading v;
        v.note    = atomic_load_explicit(&self->r_note, memory_order_relaxed);
        v.cents   = atomic_load_explicit(&self->r_cents, memory_order_relaxed);
        v.clarity = atomic_load_explicit(&self->r_clarity, memory_order_relaxed);
        v.freq    = atomic_load_explicit(&self->r_freq, memory_order_relaxed);
        v.strobe  = atomic_load_explicit(&self->r_strobe, memory_order_relaxed);
        v.voiced  = atomic_load_explicit(&self->r_voiced, memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&self->r_seq, memory_order_relaxed) == s0) return v;
    }
    return *fallback;
}

// A control input as its TTL declares it. A host or UI can write anything into a control port, and one
// non-finite value must not reach the DSP: a NaN Dim Amount for ONE block used to turn every later
// output sample into NaN until the plugin was re-activated. NaN takes the default, +-Inf and anything
// out of range clamp to the bounds.
static float port_value(const float* p, float lo, float hi, float def)
{
    if (p == NULL) return def;
    const float v = *p;
    if (v != v) return def;
    const float m = (v < lo) ? lo : v;   // max(v, lo)
    return (hi < m) ? hi : m;            // min(.., hi)
}

// An input sample the analysis can use: a non-finite one becomes silence. One NaN or Inf reaching the
// stream high-pass or the onset envelope stays in their state for good, and the tuner went deaf until
// re-activated.
static inline float finite_or_zero(float x)
{
    return (fabsf(x) <= FLT_MAX) ? x : 0.0f;
}

// The high-pass that keeps sub-audio rumble out of the analysis. 4th-order Butterworth at
// 20 Hz as two 2nd-order sections (Butterworth Qs 0.5412 and 1.3066), each designed by the bilinear
// transform with the cutoff prewarped, run in double, direct form I.
//
// It runs on the CONTINUOUS stream, not on each window, and that is the whole point (measured,
// tools/rumble_bench.c): started afresh in every window, its start-up sits
// inside the window and biases CLEAN bass notes (6.1 c on B0 at 2nd order, 30 c at 4th); carried
// across windows it leaves clean notes at 0.00 c. Under 9 Hz
// rumble, without it, a rumble of 0.1 made D3/E4 read a different note (1196 / 1901 c) and 0.2
// muted the bass; with it, worst 0.01 c at 0.1, 2.6 c on B0 at 0.2, 6.2 c on B0 at 0.4.
typedef struct HPSection { double b0, b1, b2, a1, a2, x1, x2, y1, y2; } HPSection;
typedef struct HighPass20 { HPSection sec[2]; } HighPass20;

static void hp20_init(HighPass20* h, double sample_rate)
{
    const double q[2] = { 0.54119610, 1.30656296 };
    const double w = HAWK_TWO_PI * 20.0 / sample_rate;
    const double c = cos(w), sn = sin(w);
    for (int k = 0; k < 2; ++k) {
        const double al = sn / (2.0 * q[k]), a0 = 1.0 + al;
        h->sec[k] = (HPSection){ (1.0 + c) / 2.0 / a0, -(1.0 + c) / a0, (1.0 + c) / 2.0 / a0,
                                 -2.0 * c / a0, (1.0 - al) / a0, 0.0, 0.0, 0.0, 0.0 };
    }
}
// As if `x` had been the input forever: the output starts at 0 instead of with a step.
static void hp20_prime(HighPass20* h, double x)
{
    for (int k = 0; k < 2; ++k) { HPSection* s = &h->sec[k]; s->x1 = s->x2 = x; s->y1 = s->y2 = 0.0; x = 0.0; }
}
static inline float hp20_run(HighPass20* h, float in)
{
    double v = in;
    for (int k = 0; k < 2; ++k) {
        HPSection* s = &h->sec[k];
        const double y = s->b0 * v + s->b1 * s->x1 + s->b2 * s->x2 - s->a1 * s->y1 - s->a2 * s->y2;
        s->x2 = s->x1; s->x1 = v; s->y2 = s->y1; s->y1 = y;
        v = y;
    }
    return (float)v;
}

// The analysis input at 44.1-48 kHz whatever the host's rate. Above 50 kHz the worker used to do
// everything at full rate -- at 96 kHz a 16384 window and a 262144-point long FFT, twice the work for
// no gain: the tuner reads fundamentals up to ~1.5 kHz and partials of a few kHz. So the input goes
// through 1-4 halving stages first. Each is a half-band low-pass, a Blackman-windowed sinc of 23 taps
// (11 non-zero), keeping every other sample: designed and checked at 96 kHz, ripple 0.0013 dB up to
// 10 kHz and -77.9 dB from 38 kHz, linear phase (a constant delay moves no reading).
enum { HB_N = 23, HB_C = 11, HB_MASK = 31 };
typedef struct HalfBand {
    double   h[HB_N];
    double   x[HB_MASK + 1];
    unsigned pos;
    bool     odd;
} HalfBand;

// As if `v` had been the input forever (the output then starts at v, without a ramp).
static void halfband_prime(HalfBand* b, double v)
{
    for (int e = 0; e <= HB_MASK; ++e) b->x[e] = v;
    b->pos = 0;
    b->odd = false;
}
static void halfband_init(HalfBand* b)
{
    double sum = 0.0;
    for (int n = 0; n < HB_N; ++n) {
        const double t = 0.5 * (n - HB_C);
        const double sinc = (n == HB_C) ? 1.0 : sin(HAWK_PI * t) / (HAWK_PI * t);
        const double a = HAWK_TWO_PI * n / (HB_N - 1);
        b->h[n] = 0.5 * sinc * (0.42 - 0.5 * cos(a) + 0.08 * cos(2.0 * a));
        sum += b->h[n];
    }
    for (int n = 0; n < HB_N; ++n) b->h[n] /= sum;   // unity gain at DC
    halfband_prime(b, 0.0);
}
// One input sample; every second call returns true with an output sample.
static inline bool halfband_push(HalfBand* b, double in, double* out)
{
    const unsigned pos = b->pos;
    b->x[pos & HB_MASK] = in;
    const bool emit = b->odd;
    b->odd = !b->odd;
    if (emit) {
        double acc = b->h[HB_C] * b->x[(pos - HB_C) & HB_MASK];
        for (int k = 1; k < HB_C; k += 2)   // the taps at even offsets from the centre are zero
            acc += b->h[HB_C - k] * (b->x[(pos - HB_C + k) & HB_MASK] + b->x[(pos - HB_C - k) & HB_MASK]);
        *out = acc;
    }
    ++b->pos;
    return emit;
}

typedef struct Decimator {
    HalfBand stage[4];
    int      stages;              // log2 of the factor
} Decimator;
static void decimator_prime(Decimator* d, double v) { for (int k = 0; k < d->stages; ++k) halfband_prime(&d->stage[k], v); }
static inline bool decimator_push(Decimator* d, double in, double* out)
{
    for (int k = 0; k < d->stages; ++k)
        if (!halfband_push(&d->stage[k], in, &in)) return false;
    *out = in;
    return true;
}

// The newest `size` samples of the stream, kept circularly: appending copies only the new samples,
// and the window is put in order only when an analysis reads it. (Sliding a linear buffer moved the
// whole window on every pop: 345 KB for the 1.8 s window at 48 kHz, some 300 times a second.)
typedef struct StreamTail {
    float* buf;
    size_t size;
    size_t head;                                  // where the next sample goes: the oldest one
} StreamTail;
static bool tail_init(StreamTail* t, size_t size)
{
    t->buf = (float*)calloc(size ? size : 1, sizeof(float));
    t->size = size;
    t->head = 0;
    return t->buf != NULL;
}
static void tail_free(StreamTail* t) { free(t->buf); t->buf = NULL; }
static void tail_clear(StreamTail* t)
{
    memset(t->buf, 0, t->size * sizeof(float));
    t->head = 0;
}
static void tail_push(StreamTail* t, const float* src, size_t n)
{
    const size_t size = t->size;
    if (n > size) { src += n - size; n = size; }      // only the newest window-full matters
    const size_t first = (size - t->head < n) ? size - t->head : n;   // min(n, size - head)
    memcpy(t->buf + t->head, src, first * sizeof(float));
    memcpy(t->buf, src + first, (n - first) * sizeof(float));
    t->head = (t->head + n) % size;
}
static void tail_copy_to(const StreamTail* t, float* dst)   // oldest first, size samples
{
    const size_t size = t->size;
    memcpy(dst, t->buf + t->head, (size - t->head) * sizeof(float));
    memcpy(dst + (size - t->head), t->buf, t->head * sizeof(float));
}

// Analysis window of at least ~170 ms at the ANALYSIS rate, enough to resolve low B (~31 Hz): 8192
// samples at 44.1/48 kHz, which is where the decimator puts every common host rate. The doubling and
// its 16384 cap only matter for an analysis rate between 48.2 and 50 kHz.
static size_t analysis_window_size(double sample_rate)
{
    size_t window_size = 8192;
    while (window_size < (size_t)(sample_rate * 0.17) &&
           window_size < 16384)
        window_size <<= 1;
    return window_size;
}

static void* worker_main(void* arg)
{
    Hawk* self = (Hawk*)arg;
#if defined(__SSE__)
    /* Flush denormals to zero on this thread. Its floating-point mode is otherwise INHERITED from
     * whichever thread called activate() -- in a host, the GUI or main thread -- and nothing set it:
     * measured on digital silence, the decaying filter and envelope states went denormal after ~7 s
     * and the worker's cost went from ~240 to ~535 ns per sample, +1.4 points of a core at rest.
     * Nothing this thread computes needs magnitudes below 1e-38. (SSE is the x86-64 baseline.) */
    _mm_setcsr(_mm_getcsr() | 0x8040);   // FTZ | DAZ
#endif
    // Everything below runs at the analysis rate (the input decimated by `decim`), stream positions
    // included.
    const double rate = self->analysis_rate;
    const size_t window_size  = analysis_window_size(rate);
    const size_t analysis_hop = window_size / 4;

    PitchDetector coarse;
    hawk_pitch_detector_init(&coarse, rate, HAWK_PD_DEFAULTS);
    FineTuner fine;
    hawk_fine_tuner_init(&fine, rate);

    // The worker's own buffers, allocated when it starts. An allocation that fails leaves the worker with
    // nothing to analyse: it returns at once, and the plugin passes its audio and shows no note.
    enum { CHUNK = 2048 };
    StreamTail wtail;                                // the analysis window as it streams in
    const bool tail_ok = tail_init(&wtail, window_size);
    float* window   = (float*)calloc(window_size, sizeof(float));   // ... and in order, at each analysis
    float* chunk    = (float*)calloc(CHUNK, sizeof(float));
    float* chunk_hp = (float*)calloc(CHUNK, sizeof(float));         // the same samples, high-passed
    HighPass20 hp;
    hp20_init(&hp, rate);
    bool hp_primed = false;                          // primed on the first sample, and after a seam
    Decimator dec;
    for (int k = 0; k < 4; ++k) halfband_init(&dec.stage[k]);
    dec.stages = 0;
    for (unsigned d = self->decim; d > 1; d >>= 1) ++dec.stages;
    size_t dpos = 0;                                 // stream position at the analysis rate

    size_t total_seen     = 0;
    size_t since_analysis = 0;

    // Short median history to reject single-frame cents outliers.
    Reading cur = {0};                // what is currently published; the worker owns it
    double cents_hist[5] = {0, 0, 0, 0, 0};
    __attribute__((unused)) int hist_note = -1;   // the note the readings in cents_hist belong to
    int    hist_count = 0;
    int    hist_pos   = 0;

    /* A pluck starts SHARP (tension rises with amplitude) and the pick clicks. A window is
     * 170 ms and they overlap 75 %, so one attack reaches four analyses in a row and the median of
     * five cannot reject it: measured (tools/attack_probe.c, +20 c at onset, tau 80 ms), a re-pluck
     * of an in-tune string moved the reading 9-15.5 c, took ~450 ms back within 1 c, and blinked the
     * in-tune lamp twice. So an onset is detected on the incoming audio, and no analysis whose window
     * holds the first ATTACK_S after it feeds the reading: the needle stays where it was, and from
     * silence the first reading waits for a window clear of the attack. Stream positions, like the
     * gap tracker.
     *
     * The detector compares the energy NOW with the energy ONSET_LAG ago. Tuned by measurement, and
     * every simpler version failed one case (re-plucks at 4x and at 2x the amplitude the string still
     * had, B0..E4): against a 100 ms average with a 9 dB threshold it sat at the edge (4x peaked at
     * 7.0-7.7, most missed); against slower averages the soft 2x re-plucks were missed, because an
     * average REMEMBERS how loud the decaying string used to be and so sits above its present level;
     * and a 5 ms single-pole envelope let the power ripple of a low note (at twice its frequency)
     * cross both thresholds every 6 ms, re-firing and pushing the first reading of E2 to 475 ms. */
    const double ATTACK_S    = 0.100;             // what an attack is taken to last
    const double ONSET_LAG_S = 0.050;             // "just before": the reference is this far back
    const double ONSET_RATIO = 2.5;               // energy now / energy then, to count as an onset (4 dB)
    const double REARM_RATIO = 1.5;               // ... and below which it may fire again (hysteresis)
    const double ONSET_FLOOR = 1.0e-7;            // below this power nothing is an onset (~-70 dBFS)
    const size_t ENV_DECIM = 16;                  // the envelope history is kept every 16 samples
    // two one-poles of 10 ms in cascade: ~6 % of ripple left even at B0, far inside the hysteresis
    const double env_a = 1.0 - exp(-1.0 / (0.010 * rate));
    const size_t attack_len = (size_t)(ATTACK_S * rate);
    const size_t env_hist_n = (size_t)(ONSET_LAG_S * rate) / ENV_DECIM + 1;
    double* env_hist = (double*)calloc(env_hist_n, sizeof(double));
    size_t env_head = 0, env_tick = 0;
    double      env1 = 0.0, env = 0.0;
    bool        onset_armed = true;
    size_t      onset_end   = 0;                  // stream position where the latest attack ends
    bool        seen_onset  = false;

    /* The other end of a note. A note stopped dead (a hand on the strings, a mute) leaves windows that
     * hold part note, part silence, and analysing them answers about a waveform that was never played:
     * measured, A2 published -6.7 c and a frequency 28 c off for ~170 ms after the stop, lamp still lit.
     * A fast power envelope (2 ms) falling 20 dB under the slow one marks the stop: no plucked string
     * decays that fast (a palm mute does, and it ends the note too), and the lowest bass note's power
     * ripple dips the fast envelope by at most ~7 dB. No window holding the stop feeds the reading;
     * "no signal" is published instead, which is what the player just did. */
    const double OFFSET_RATIO = 100.0;            // slow power / fast power, to count as a stop (20 dB)
    const double fast_a = 1.0 - exp(-1.0 / (0.002 * rate));
    double      fast = 0.0;
    size_t      offset_pos  = 0;                  // stream position of the latest stop
    bool        seen_offset = false;

    if (!tail_ok || !window || !chunk || !chunk_hp || !env_hist) goto done;

    while (atomic_load_explicit(&self->running, memory_order_acquire)) {
        const size_t r0  = ring_read_pos(&self->ring);   // the worker owns the read side: exact
#if HAWK_JUDGE_SYNC
        atomic_store(&self->judge_idle, false);          // before the pop: see run()
#endif
        size_t got = ring_pop(&self->ring, chunk, CHUNK);
        if (got == 0) {
#if HAWK_JUDGE_SYNC
            atomic_store(&self->judge_idle, true);
#endif
            // up to 3 ms, or until deactivate() clears `running` and signals (see wake_cv)
            pthread_mutex_lock(&self->wake_m);
            if (atomic_load_explicit(&self->running, memory_order_acquire)) {
                struct timespec until;
                clock_gettime(CLOCK_MONOTONIC, &until);
                until.tv_nsec += 3000000L;
                if (until.tv_nsec >= 1000000000L) { until.tv_nsec -= 1000000000L; ++until.tv_sec; }
                pthread_cond_timedwait(&self->wake_cv, &self->wake_m, &until);
            }
            pthread_mutex_unlock(&self->wake_m);
            continue;
        }

        /* Has this pop carried the read pointer past a seam? Then the window built so far straddles
         * it, and analysing it would answer about a waveform that was never played. Start over, and
         * keep only the part of this pop that comes after the seam: a missing reading for a
         * fraction of a second is honest, a confident wrong note is not.
         *
         * A pop that ends exactly ON the seam is clean: its last sample is the last one before the
         * loss, and the next pop starts the restart. */
        const GapVerdict gv = gaps_consume(&self->gaps, r0, r0 + got);
        if (gv.reset) {
            tail_clear(&wtail);
            total_seen     = 0;
            since_analysis = 0;
            hist_count     = 0;
            hist_pos       = 0;
            hp_primed      = false;                  // a seam is a discontinuity: start the filter again
        }
        got -= gv.skip;
        if (got == 0) continue;
        if (!hp_primed) {
            const float x0 = finite_or_zero(chunk[gv.skip]);
            decimator_prime(&dec, x0);
            hp20_prime(&hp, x0);
            hp_primed = true;
        }
        {
            const float* src = chunk + gv.skip;
            float*       dst = chunk_hp;
            if (dec.stages == 0) {
                for (size_t j = 0; j < got; ++j) dst[j] = hp20_run(&hp, finite_or_zero(src[j]));
            } else {
                size_t nd = 0;
                for (size_t j = 0; j < got; ++j) {
                    double v;
                    if (decimator_push(&dec, finite_or_zero(src[j]), &v)) dst[nd++] = hp20_run(&hp, (float)v);
                }
                got = nd;                                // from here on, samples at the analysis rate
            }
        }
        if (got == 0) continue;
        const size_t pop_pos = dpos;                     // stream position of the pop's first sample
        dpos += got;

        /* The analysis grid is fixed in the STREAM: the pop is cut at every analysis point, so a window
         * ends exactly every analysis_hop samples however the ring was drained. Analysing "when at least
         * a hop has arrived" and then restarting the count put the analyses where the pops happened to
         * end, so how often and where they fell followed this thread's speed and the machine's load:
         * under ThreadSanitizer (a slow worker draining big pops) a string 14 c flat in a drop-tuning
         * strum showed in tune half the time, while the same code at full speed showed it flat. */
        for (size_t off = 0, seg = 0; off < got; off += seg) {
            size_t need;
            if (total_seen < window_size) {
                const size_t a = window_size - total_seen;
                const size_t b = analysis_hop - ((analysis_hop < since_analysis) ? analysis_hop : since_analysis);
                need = (a < b) ? b : a;                           // max(a, b), b = hop - min(since, hop)
            } else {
                need = analysis_hop - since_analysis;
            }
            seg = (need < got - off) ? need : got - off;          // min(got - off, need)
            const float* fresh = chunk_hp + off;
            const size_t fresh_pos = pop_pos + off;               // stream position of fresh[0]
            const size_t end_pos   = fresh_pos + seg;             // one past the window's newest sample

            // onset detection on the incoming samples
            for (size_t j = 0; j < seg; ++j) {
                const double e = (double)fresh[j] * fresh[j];
                env1 += env_a * (e - env1);
                env  += env_a * (env1 - env);
                fast += fast_a * (e - fast);
                if (env > ONSET_FLOOR && fast * OFFSET_RATIO < env) {
                    seen_offset = true;
                    offset_pos  = fresh_pos + j;
                }
                if (++env_tick < ENV_DECIM) continue;
                env_tick = 0;
                const double then = env_hist[env_head];   // the oldest entry: ONSET_LAG_S ago
                env_hist[env_head] = env;
                env_head = (env_head + 1 == env_hist_n) ? 0 : env_head + 1;
                if (onset_armed && env > ONSET_FLOOR && env > ONSET_RATIO * then) {
                    seen_onset  = true;
                    onset_armed = false;
                    onset_end   = fresh_pos + j + attack_len;
                } else if (!onset_armed && env < REARM_RATIO * then) {
                    onset_armed = true;
                }
            }

            tail_push(&wtail, fresh, seg);
            total_seen     += seg;
            since_analysis += seg;

            if (total_seen < window_size || since_analysis < analysis_hop) {
                continue;
            }
            since_analysis = 0;
            tail_copy_to(&wtail, window);

            /* Digital silence (a muted or idle track): nothing to analyse, so publish "no signal" and stop
             * here. Without this the coarse detector ran every hop on zeros at nearly a note's cost, 1.2 %
             * of a core per instance. -120 dBFS RMS is below any real input's noise floor, and the
             * high-pass tail of a note that stopped falls below it within a fraction of a second. */
            double energy = 0.0;
            for (size_t i = 0; i < window_size; ++i)
                energy += (double)window[i] * window[i];
            const double rms = sqrt(energy / (double)window_size);
            const double SILENCE_RMS = 1.0e-6;
            if (rms < SILENCE_RMS) {
                if (cur.voiced || cur.clarity != 0.0f) {
                    cur.voiced  = 0;
                    cur.clarity = 0.0f;
                    hawk_publish(self, &cur);
                }
                hist_count = 0;
                continue;
            }

            const double a4 = atomic_load_explicit(&self->a4_ref, memory_order_relaxed);


            // ---- Monophonic path --------------------------------------------
            // A window that still holds an attack does not feed the reading: keep what is
            // published, as it is, until a clean window arrives.
            if (seen_offset && end_pos - window_size < offset_pos) {   // the window holds a stop
                if (cur.voiced) {
                    cur.voiced = 0;
                    hawk_publish(self, &cur);
                }
                hist_count = 0;
                continue;
            }
            if (seen_onset && end_pos - window_size < onset_end) continue;

            const PitchEstimate c = hawk_pitch_detector_analyze(&coarse, window, window_size);
            /* The worker is the only writer, so it can keep its own copy of what is published and
             * amend it -- the unvoiced path below wants to update clarity and voiced while leaving
             * the last note/cents/freq standing, which is only expressible if the set is written as
             * a set. */
            cur.clarity = (float)(c.clarity);

            // Sensitivity remaps the clarity gate: 0.5 = default (~0.85), higher =
            // more sensitive (lower threshold → picks up weaker / noisier signals).
            float gate = 0.85f + (0.5f - atomic_load_explicit(&self->sens, memory_order_relaxed)) * 0.40f;
            if (gate < 0.50f) gate = 0.50f;
            if (gate > 0.99f) gate = 0.99f;
            if (!(c.frequency > 0.0 && c.clarity >= gate)) {
                cur.voiced = 0;
                hawk_publish(self, &cur);
                hist_count = 0;  // start fresh for the next note
                continue;
            }
            const NoteReading nr = hawk_frequency_to_note(c.frequency, a4);
            const double target = hawk_note_to_frequency(nr.midi_note, a4);
            const FineEstimate f = hawk_fine_tuner_refine(&fine, window, window_size,
                                                          target);
            /* The fine stage's own confidence vetoes the window. Below MIN_COHERENCE the
             * fundamental at the chosen note did not turn steadily -- in the windows measured that is the
             * coarse stage having picked the WRONG note, and falling back to its cents would publish that
             * wrong note with confidence. So: no reading for this window.
             *
             * Where the threshold sits, measured (tools/coherence_bench.c): legitimate windows
             * >= 0.994 (clean notes 1.0000; a peg turned at 200 c/s 0.9994; vibrato +-30 c 0.9969; a note
             * in white noise at 0 dB SNR 0.994); the two wrong-note windows found (9 Hz rumble 0.1 on D3
             * and E4, 1196 and 1901 c off) 0.900 and 0.956. 0.975 leaves ~0.02 on either side. */
            const double MIN_COHERENCE = 0.975;
            if (f.coherence < MIN_COHERENCE) {
                cur.voiced = 0;
                hawk_publish(self, &cur);
                hist_count = 0;
                continue;
            }
            double cents = f.valid ? f.cents     : nr.cents;
            const double freq  = f.valid ? f.frequency : c.frequency;
            int note = nr.midi_note;
#if !HAWK_OLD_NOTE_SNAP
            /* The fine stage measures up to 60 c from the note the coarse stage chose: past 50 c the string is
             * nearer the neighbour, so the note is re-read from the fine frequency -- never "E +52 c" for a string
             * that is "F -48 c". The strobe's phase is against the coarse note, so it is kept as it was for this
             * window. And the median below only mixes readings of ONE note: across a change of note it would
             * publish the old note's cents (+49) under the new one for two analyses.
             * HAWK_OLD_NOTE_SNAP=1 restores both, for tools/boundary_probe.c's negative arm only. */
            bool resnapped = false;
            if (f.valid && fabs(cents) > 50.0) {
                const NoteReading fr = hawk_frequency_to_note(f.frequency, a4);
                note = fr.midi_note;
                cents = fr.cents;
                resnapped = true;
            }
            if (note != hist_note) { hist_count = 0; hist_note = note; }
#else
            const bool resnapped = false;
#endif

            // Median of the last few cents readings. The median reads the FIRST n_med slots, so a fresh history
            // (after a silence, a stop, or a change of note) must also start writing at slot 0 -- else its first
            // readings take the old note's cents from the slots it has not overwritten yet.
#if !HAWK_OLD_NOTE_SNAP
            if (hist_count == 0) hist_pos = 0;
#endif
            cents_hist[hist_pos] = cents;
            hist_pos = (hist_pos + 1) % 5;
            if (hist_count < 5) ++hist_count;
            const int n_med = (5 < hist_count) ? 5 : hist_count;  // always in [1, 5] here
            double sorted[5];
            for (int k = 0; k < n_med; ++k) sorted[k] = cents_hist[k];
            for (int i = 1; i < n_med; ++i)              // insertion sort: at most five values
                for (int j = i; j > 0 && sorted[j] < sorted[j - 1]; --j) {
                    const double t = sorted[j]; sorted[j] = sorted[j - 1]; sorted[j - 1] = t;
                }
            const double cents_median = sorted[n_med / 2];

            /* The strobe: the string's phase against the reference note, in STREAM time. The fine stage's
             * phase is against a reference that starts at 0 on the window's first sample, so from one
             * window to the next it jumps by the string's own phase advance; taking off the reference's
             * phase at that first sample (2 pi f0 t, t = its stream position) leaves phi(t) - 2 pi f0 t,
             * which turns at 2 pi (f - f0) rad/s: still in tune, forward when sharp, back when flat. The
             * fine stage's two moving averages add a constant offset, which a strobe does not see.
             * Kept as it was when the window gives no fine reading. */
            if (f.valid && !resnapped) {
                const double turns = target * (double)(end_pos - window_size) / rate;
                double ph = f.phase - HAWK_TWO_PI * (turns - floor(turns));
                ph = remainder(ph, HAWK_TWO_PI);
                cur.strobe = (float)(ph);
            }
            cur.note   = (float)(note);
            cur.cents  = (float)(cents_median);
            cur.freq   = (float)(freq);
            cur.voiced = 1;
            hawk_publish(self, &cur);
        }
    }
done:
    hawk_pitch_detector_free(&coarse);
    hawk_fine_tuner_free(&fine);
    tail_free(&wtail);
    free(window); free(chunk); free(chunk_hp); free(env_hist);
    return NULL;
}

// Every member whose value at rest is not zero; the rest is zeroed by calloc.
static void hawk_defaults(Hawk* self)
{
    self->sample_rate   = 48000.0;
    self->analysis_rate = 48000.0;
    self->decim         = 1;
    self->was_enabled   = true;
    self->current_gain  = 1.0f;
    self->ref_zr = 1.0; self->ref_step_c = 1.0; self->ref_step_for = -1.0;
    self->png_zr = 1.0; self->png_step_c = 1.0; self->png_step_for = -1.0;
    self->tune_zone = -1;
    self->cand_zone = -1;
    atomic_init(&self->running, false);
    atomic_init(&self->r_seq, 0);
    atomic_init(&self->r_note, 0.0f); atomic_init(&self->r_cents, 0.0f); atomic_init(&self->r_clarity, 0.0f);
    atomic_init(&self->r_freq, 0.0f); atomic_init(&self->r_strobe, 0.0f); atomic_init(&self->r_voiced, 0);
    atomic_init(&self->a4_ref, 440.0f);
    atomic_init(&self->sens, 0.5f);
    atomic_init(&self->tuning_idx, 0);
#if HAWK_JUDGE_SYNC
    atomic_init(&self->judge_idle, false);
#endif
    gaps_clear(&self->gaps);
}

static void hawk_free(Hawk* self)
{
    if (!self) return;
    ring_free(&self->ring);
    pthread_cond_destroy(&self->wake_cv);
    pthread_mutex_destroy(&self->wake_m);
    free(self);
}

static LV2_Handle instantiate(const LV2_Descriptor* descriptor, double rate, const char* bundle_path,
                              const LV2_Feature* const* features)
{
    (void)descriptor; (void)bundle_path; (void)features;
    // A rate no host runs at is refused, as the LV2 contract allows (NULL), instead of sizing the
    // buffers from it: a negative rate made an impossible allocation out of instantiate() and aborted
    // the host, and 0 gave a handle with empty filters.
    if (!(rate >= 8000.0 && rate <= 768000.0)) return NULL;
    {
        Hawk* self = (Hawk*)calloc(1, sizeof *self);
        if (!self) return NULL;
        hawk_defaults(self);
        pthread_condattr_t ca;
        pthread_condattr_init(&ca);
        pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);   // the worker's 3 ms wait must not follow the wall clock
        const bool sync_ok = pthread_mutex_init(&self->wake_m, NULL) == 0 && pthread_cond_init(&self->wake_cv, &ca) == 0;
        pthread_condattr_destroy(&ca);
        if (!sync_ok || !ring_init(&self->ring, 1u << 16)) {   // out of memory: fail the instantiation, not the host
            ring_free(&self->ring);
            free(self);
            return NULL;
        }
        self->sample_rate = rate;
        while (rate / self->decim > 50000.0) self->decim *= 2;   // 88.2/96 kHz: 2, 176.4/192: 4, ...
        self->analysis_rate = rate / self->decim;
        const float srf = (float)(rate);
        self->gain_coeff    = 1.0f - expf(-1.0f / (0.01f * srf));
        self->ping_decay    = expf(-1.0f / (0.13f * srf));
        self->level_release = expf(-1.0f / (0.3f * srf));
        return (LV2_Handle)self;
    }
}

// Stop and join the worker, if one runs. Shared by deactivate(), cleanup() and a repeated activate().
static void stop_worker(Hawk* self)
{
    atomic_store(&self->running, false);
    /* taking the lock before signalling guarantees the worker cannot miss the flag between its
     * check of `running` and going to sleep */
    pthread_mutex_lock(&self->wake_m);
    pthread_cond_broadcast(&self->wake_cv);
    pthread_mutex_unlock(&self->wake_m);
    if (self->worker_started) {
        pthread_join(self->worker, NULL);
        self->worker_started = false;
    }
}

static void connect_port(LV2_Handle handle, uint32_t port, void* data)
{
    Hawk* self = (Hawk*)handle;
    switch (port) {
        case PORT_IN:      self->in        = (const float*)data; break;
        case PORT_OUT:     self->out       = (float*)data;       break;
        case PORT_ACTIVE:  self->active    = (const float*)data; break;
        case PORT_DIM_DB:  self->dim_db    = (const float*)data; break;
        case PORT_NOTE:    self->p_note    = (float*)data;       break;
        case PORT_CENTS:   self->p_cents   = (float*)data;       break;
        case PORT_CLARITY: self->p_clarity = (float*)data;       break;
        case PORT_FREQ:    self->p_freq    = (float*)data;       break;
        case PORT_VOICED:    self->p_voiced  = (float*)data;       break;
        case PORT_TOLERANCE: self->tolerance = (const float*)data; break;
        case PORT_RAISE:     self->p_raise   = (float*)data;       break;
        case PORT_IN_TUNE:   self->p_in_tune = (float*)data;       break;
        case PORT_LOWER:     self->p_lower   = (float*)data;       break;
        case PORT_A4:        self->p_a4      = (const float*)data; break;
        case PORT_TUNING:    self->p_tuning  = (const float*)data; break;
        case PORT_REF_ACTIVE: self->ref_active = (const float*)data; break;
        case PORT_REF_NOTE:   self->ref_note   = (const float*)data; break;
        case PORT_PING_ENABLE: self->ping_enable = (const float*)data; break;
        case PORT_LEVEL:       self->p_level     = (float*)data;       break;
        case PORT_SENSITIVITY: self->sensitivity = (const float*)data; break;
        case PORT_STROBE:      self->p_strobe      = (float*)data;     break;
        case PORT_STROBE_MODE: self->strobe_mode   = (const float*)data; break;
        case PORT_ENABLED:     self->enabled       = (const float*)data; break;
        default: break;
    }
}

static void activate(LV2_Handle handle)
{
    Hawk* self = (Hawk*)handle;
    // activate() twice without deactivate() breaks the host contract, but assigning a new thread
    // over a running one would take the host down with it (a thread handle silently lost, then joined twice).
    stop_worker(self);

    // EVERYTHING the audio path or the display can carry over has to go back to rest here.
    //
    // Measured before this existed: after deactivate() + activate() + one block of DIGITAL SILENCE,
    // 4 of 9 control outputs still held their old values -- note 57, freq 220, voiced 1,
    // in_tune 1. The host had stopped the plugin, nothing was going in, and the tuner was still
    // reporting a settled, in-tune A3. Instantiate-time defaults do not cover this: activate can be
    // called many times on the same instance.
    //
    // Safe to touch without atomics or locks: the worker has been joined above, and the thread is
    // started at the END of this function, so nothing else is running yet.
    self->current_gain = 1.0f;

    {
        const Reading zero = {0};
        hawk_publish(self, &zero);   // no worker running yet, so this cannot race
        self->last_reading = zero;
    }

    self->level_env = 0.0f;
    self->ping_env  = 0.0f;
    self->png_zr = 1.0; self->png_zi = 0.0;
    self->ref_osc_gain = 0.0f;

    self->tune_zone     = -1;
    self->cand_zone     = -1;
    self->cand_samps    = 0;
    self->release_samps = 0;
    self->in_tune_samps = 0;

    // and the audio the worker has not consumed yet, which is audio from before the host stopped us
    ring_clear(&self->ring);
    // ring_clear() rewinds both stream positions to 0, so a gap recorded against the OLD numbering would
    // now name a point the reader is already past -- and fire on the first pop after restart
    gaps_clear(&self->gaps);

    atomic_store(&self->running, true);
#if HAWK_JUDGE_SYNC
    atomic_store(&self->judge_idle, false);
#endif
#if !HAWK_THREAD_UNGUARDED
    // Creating a thread can fail (the system's task limit). Without a worker nothing is analysed -- the audio still
    // passes and the tuner shows no note. (HAWK_THREAD_UNGUARDED=1 aborts there instead, as an unhandled failure
    // takes the HOST down, for tools/thread_probe.c's negative arm only.)
    if (pthread_create(&self->worker, NULL, worker_main, self) == 0) {
        self->worker_started = true;
    } else {
        atomic_store(&self->running, false);
    }
#else
    if (pthread_create(&self->worker, NULL, worker_main, self) != 0) abort();
    self->worker_started = true;
#endif
}

static void run(LV2_Handle handle, uint32_t n_samples)
{
    Hawk* self = (Hawk*)handle;
    if (self->in == NULL || self->out == NULL) {
        return;
    }

    /* The plugin's own bypass (lv2:enabled: the host's bypass and the hawk in the GUI are this one port).
     * Bypassed, the input passes untouched once the dim and the tones have faded (10 ms), nothing is analysed
     * and the tuner shows nothing. Back from it, the stream has a hole where the bypass was: a seam restarts
     * the analysis after it, and the last readings published before it stay hidden until fresh ones come. */
    const bool enabled = port_value(self->enabled, 0.0f, 1.0f, 1.0f) > 0.5f;
    if (enabled && !self->was_enabled) {
        gaps_mark(&self->gaps, ring_written(&self->ring));
        self->stale     = true;
        self->stale_seq = atomic_load_explicit(&self->r_seq, memory_order_acquire);
    }
    self->was_enabled = enabled;

    // Feed the detector the dry input before we touch the output (in case the
    // host aliases the in and out buffers).
    if (enabled) {
        const size_t pushed = ring_push(&self->ring, self->in, n_samples);
        if (pushed < n_samples) {
            /* The ring was full, so what is in it ends exactly at the discontinuity. */
            gaps_mark(&self->gaps, ring_written(&self->ring));
        }
    }
#if HAWK_JUDGE_SYNC
    /* The port judge only (tools/port_c): wake the worker and wait until it has analysed everything pushed so far,
     * so what this block reads back no longer depends on the threads' timing. Never in a product build: it puts a
     * futex wake and a spin on the audio thread. The worker clears judge_idle BEFORE each pop, so seeing the ring
     * empty and then the flag set means the last pop's analysis is finished. */
    if (atomic_load(&self->running)) {
        pthread_mutex_lock(&self->wake_m);
        pthread_cond_broadcast(&self->wake_cv);
        pthread_mutex_unlock(&self->wake_m);
        while (!(ring_read_pos(&self->ring) == ring_written(&self->ring) && atomic_load(&self->judge_idle))) sched_yield();
    }
#endif

    const float active = enabled && self->active ? *self->active : 0.0f;
    const float dim_db = port_value(self->dim_db, -60.0f, 0.0f, -12.0f);
    const float a4_now = port_value(self->p_a4, 380.0f, 500.0f, 440.0f);
    atomic_store_explicit(&self->a4_ref, a4_now, memory_order_relaxed);
    atomic_store_explicit(&self->sens, port_value(self->sensitivity, 0.0f, 1.0f, 0.5f), memory_order_relaxed);
    atomic_store_explicit(&self->tuning_idx, (int)(lroundf(port_value(self->p_tuning, 0.0f, (float)(TUNING_COUNT - 1), 0.0f))),
                          memory_order_relaxed);
    const float target_gain =
        (active > 0.5f) ? powf(10.0f, dim_db / 20.0f) : 1.0f;

    // One-pole smoothing (~10 ms) so engaging the dim never clicks.
    const float coeff = self->gain_coeff;

    // Reference tone: a sine at the selected string's pitch, faded in/out and
    // mixed under the passthrough. When it is fully off we skip the mix entirely,
    // so unused/idle passthrough stays bit-exact.
    const float REF_LEVEL = 0.20f;                 // tone amplitude when engaged
    const bool  ref_on    = enabled && self->ref_active && *self->ref_active > 0.5f;
    const float ref_tgt   = ref_on ? REF_LEVEL : 0.0f;
    const bool  mix_ref   = ref_on || self->ref_osc_gain > 0.0f;
    double ref_dphase = 0.0;
    if (mix_ref) {
        const int rn = (int)(lroundf(port_value(self->ref_note, 0.0f, 127.0f, 40.0f)));
        ref_dphase = HAWK_TWO_PI * hawk_note_to_frequency(rn, a4_now) / self->sample_rate;
    }

    // Ping oscillator constants: a soft, warm pitch (E5) with a quiet octave
    // partial for a touch of bell, and a short ~0.13 s exponential decay.
    const float  PING_LEVEL = 0.18f;
    const double PING_FREQ  = 659.26;       // E5 — warmer, less piercing than E6
    const double ping_dphase = HAWK_TWO_PI * PING_FREQ / self->sample_rate;
    /* The rotation step only changes when the frequency does -- a note change for the reference,
     * never for the ping -- so the two transcendentals are paid per BLOCK at worst, not per sample.
     * The phase itself lives in z and carries across a frequency change untouched, which is what
     * keeps the tone continuous when the target note moves. */
    if (ref_dphase != self->ref_step_for) {
        self->ref_step_c = cos(ref_dphase);
        self->ref_step_s = sin(ref_dphase);
        self->ref_step_for = ref_dphase;
    }
    if (ping_dphase != self->png_step_for) {
        self->png_step_c = cos(ping_dphase);
        self->png_step_s = sin(ping_dphase);
        self->png_step_for = ping_dphase;
    }
    /* Renormalise once per block. The drift is ~1e-16 relative per step, so over any realistic
     * block it is far below what a synthesised reference tone needs -- but left unbounded it
     * accumulates for the life of the plugin, and this costs two divisions per block. */
    {
        const double rn = 1.0 / sqrt(self->ref_zr * self->ref_zr + self->ref_zi * self->ref_zi);
        self->ref_zr *= rn; self->ref_zi *= rn;
        const double pn = 1.0 / sqrt(self->png_zr * self->png_zr + self->png_zi * self->png_zi);
        self->png_zr *= pn; self->png_zi *= pn;
    }
    const float ping_decay = self->ping_decay;

    // Level meter: peak-follower with ~0.3 s release on the dry input.
    const float level_release = self->level_release;

    for (uint32_t i = 0; i < n_samples; ++i) {
        const float ain = fabsf(finite_or_zero(self->in[i]));   // an Inf would pin the meter
        if (ain > self->level_env) self->level_env = ain;       // instant attack
        else                       self->level_env *= level_release;
        // Snap to zero, exactly as ping_env below already does. Without it this envelope is the
        // only one in the plugin that decays into the denormal range and STAYS there: measured,
        // it takes 26.2 s of silence to get there and then does not leave (still denormal after
        // 200 M samples), and a denormal multiply costs x17.3 on this machine -- about 0.35 % of
        // a core, permanently, every time the player stops, in a plugin that lives on the track.
        if (self->level_env < 1.0e-9f) self->level_env = 0.0f;
        self->current_gain += (target_gain - self->current_gain) * coeff;
        // Snap to the target once within a hair of it. The one-pole smoother only
        // *approaches* its target, so without this the gain never reaches exactly 1.0
        // and passthrough is never bit-exact after the dim/mute has been used once.
        // ⚠ The hair is 1e-4 (-80 dB), not 1e-6: near 1.0 the float step (gain - 1) * coeff
        // falls below the float spacing and the smoother STOPS ~3e-5 short, so a 1e-6
        // snap never fired -- the bypass gate found it (0 of 45568 samples exact).
        if (fabsf(self->current_gain - target_gain) < 1.0e-4f)
            self->current_gain = target_gain;

        float s = self->in[i] * self->current_gain;
        if (mix_ref) {
            self->ref_osc_gain += (ref_tgt - self->ref_osc_gain) * coeff;
            // Once a release has decayed to silence, snap to exactly 0 so the
            // next block takes the bit-exact passthrough path again.
            if (!ref_on && self->ref_osc_gain < 1.0e-5f) self->ref_osc_gain = 0.0f;
            s += (float)(self->ref_zi) * self->ref_osc_gain;
            const double nzr = self->ref_zr * self->ref_step_c - self->ref_zi * self->ref_step_s;
            const double nzi = self->ref_zr * self->ref_step_s + self->ref_zi * self->ref_step_c;
            self->ref_zr = nzr; self->ref_zi = nzi;
        }
        if (self->ping_env > 0.0f) {
            /* sin(2p) = 2 sin(p) cos(p), so the second partial is free from the same rotation */
            s += PING_LEVEL * self->ping_env *
                 (float)(self->png_zi + 0.22 * (2.0 * self->png_zi * self->png_zr));
            const double qzr = self->png_zr * self->png_step_c - self->png_zi * self->png_step_s;
            const double qzi = self->png_zr * self->png_step_s + self->png_zi * self->png_step_c;
            self->png_zr = qzr; self->png_zi = qzi;
            self->ping_env *= ping_decay;
            if (self->ping_env < 1.0e-4f) self->ping_env = 0.0f;  // snap -> bit-exact
        }
        self->out[i] = s;
    }
    if (!enabled) self->level_env = 0.0f;
    if (self->p_level) *self->p_level = (1.0f < self->level_env) ? 1.0f : self->level_env;   // min

    /* One read per block, of the whole set. The old code loaded r_cents TWICE -- once for the
     * output port and once for the lamp logic -- so the number the user saw and the number the
     * lamps were decided from could come from different analyses. */
    Reading rd = hawk_read_result(self, &self->last_reading);
    self->last_reading = rd;
    if (self->stale && atomic_load_explicit(&self->r_seq, memory_order_acquire) != self->stale_seq) self->stale = false;
#if !HAWK_BYPASS_KEEPS_READING
    // the whole reading, not just the flags: a host's generic UI or an automation lane shows these ports as they are
    // (HAWK_BYPASS_KEEPS_READING=1 restores the old, flags only, for tools/bypass_probe.c's negative arm only)
    if (!enabled || self->stale) { const Reading zero = {0}; rd = zero; }
#else
    if (!enabled || self->stale) { rd.voiced = 0; rd.clarity = 0.0f; }
#endif

    if (self->p_note)    *self->p_note    = rd.note;
    if (self->p_cents)   *self->p_cents   = rd.cents;
    if (self->p_clarity) *self->p_clarity = rd.clarity;
    if (self->p_freq)    *self->p_freq    = rd.freq;
    if (self->p_strobe)  *self->p_strobe  = rd.strobe;

    const int   voiced_now = rd.voiced;
    const float cents_now  = rd.cents;
    if (self->p_voiced) *self->p_voiced = (float)(voiced_now);

    // --- Readable tuning lamps -------------------------------------------
    // The cents value (already median-smoothed in the worker) still wobbles and
    // real strings drift, so deciding the lamps instantaneously makes them
    // chatter. The measurement stays as precise as ever; we only stabilise the
    // *displayed verdict* so a human can read it. Three mechanisms:
    //   - hysteresis: enter the in-tune band at tol, leave only past tol+margin;
    //   - asymmetric debounce: entering "in tune" feels near-instant, while any
    //     other switch must persist a little to avoid flicker;
    //   - a sticky latch: once "in tune" lights, hold it for IN_TUNE_HOLD_S even
    //     if the string drifts off, so a split-second hit stays readable.
    // Tweak IN_TUNE_HOLD_S to taste: bigger = stickier/laggier, smaller = twitchier.
    const double IN_TUNE_HOLD_S = 1.3;       // <-- the "hold long enough to read" knob
    const double ENTER_DEBOUNCE_S = 0.03;
    const double SWITCH_DEBOUNCE_S = 0.12;
    const double RELEASE_HOLD_S = 0.40;

    const float    tol      = port_value(self->tolerance, 0.5f, 10.0f, 2.0f);
    const float    hyst     = 1.5f;
    const uint32_t min_hold = (uint32_t)(IN_TUNE_HOLD_S   * self->sample_rate);
    const uint32_t enter_db = (uint32_t)(ENTER_DEBOUNCE_S * self->sample_rate);
    const uint32_t switch_db= (uint32_t)(SWITCH_DEBOUNCE_S* self->sample_rate);
    const uint32_t release  = (uint32_t)(RELEASE_HOLD_S   * self->sample_rate);

    const int zone_before = self->tune_zone;   // for the ping rising-edge test

    // Advance the in-tune hold clock whenever the lamp currently reads "in tune"
    // (wall-clock, so it releases even if the note has since gone silent).
    // Both clocks saturate at what they are compared with: left to count, a uint32 wraps after ~6 h at
    // 192 kHz (a held drone, an idle track), and the wrap would re-arm the latch / the release hold.
    if (self->tune_zone == 1 && self->in_tune_samps < min_hold) self->in_tune_samps += n_samples;

    int raw;  // -1 none, 0 raise, 1 in tune, 2 lower
    if (voiced_now) {
        self->release_samps = 0;
        const float band = (self->tune_zone == 1) ? (tol + hyst) : tol;
        if (fabsf(cents_now) <= band) raw = 1;
        else if (cents_now < 0.0f)        raw = 0;  // flat -> raise
        else                              raw = 2;  // sharp -> lower
    } else {
        if (self->release_samps < release) self->release_samps += n_samples;
        raw = (self->release_samps < release) ? self->tune_zone : -1;
    }

    // Sticky latch: while in tune and the minimum hold hasn't elapsed, ignore
    // any pull away from "in tune" and keep the lamp lit.
    const bool latched = (self->tune_zone == 1) && (self->in_tune_samps < min_hold);

    if (latched && raw != 1) {
        self->cand_zone  = -1;
        self->cand_samps = 0;
    } else if (raw == self->tune_zone) {
        self->cand_samps = 0;
    } else if (raw == self->cand_zone) {
        self->cand_samps += n_samples;
        // Entering "in tune" is quick; every other switch is more deliberate.
        const uint32_t need = (raw == 1) ? enter_db : switch_db;
        if (self->cand_samps >= need) {
            self->tune_zone  = raw;
            self->cand_samps = 0;
            if (raw == 1) self->in_tune_samps = 0;  // start the hold clock
        }
    } else {
        self->cand_zone  = raw;
        self->cand_samps = n_samples;
    }

    if (!enabled) {   // bypassed: no lamp lingers on its release hold
        self->tune_zone  = -1;
        self->cand_zone  = -1;
        self->cand_samps = 0;
    }
    if (self->p_raise)   *self->p_raise   = (self->tune_zone == 0) ? 1.0f : 0.0f;
    if (self->p_in_tune) *self->p_in_tune = (self->tune_zone == 1) ? 1.0f : 0.0f;
    if (self->p_lower)   *self->p_lower   = (self->tune_zone == 2) ? 1.0f : 0.0f;

    // Fire the confirmation ping (rendered from the next block) unless switched
    // off in Advanced Settings. It fires on the rising edge into "in tune" -- but
    // ONLY when the detected note is one of the selected tuning's open-string
    // pitches. A note that lands in tune yet isn't part of the tuning (e.g. a D2
    // in Standard, or any fretted note) must stay silent: the ping confirms a
    // tuned STRING, not just any in-tune pitch.
    const bool ping_on = enabled && (self->ping_enable ? (*self->ping_enable > 0.5f) : false);
    bool fire_ping = false;
    if (ping_on && self->tune_zone == 1 && zone_before != 1) {
        // Rising edge into "in tune" -- check the note against the tuning targets
        // (cheap, and only on this rare edge, so no per-block cost).
        const int det = (int)(lroundf(rd.note));
        int tgt[TUNING_MAX_STRINGS];   /* sized from the table, not guessed */
        const int nt = build_targets(atomic_load_explicit(&self->tuning_idx, memory_order_relaxed), tgt);
        for (int i = 0; i < nt; ++i)
            if (tgt[i] == det) { fire_ping = true; break; }
    }
    if (fire_ping) {
        self->ping_env = 1.0f;
        self->png_zr = 1.0; self->png_zi = 0.0;
    }
}

static void deactivate(LV2_Handle handle)
{
    stop_worker((Hawk*)handle);
}

static void cleanup(LV2_Handle handle)
{
    Hawk* self = (Hawk*)handle;
    stop_worker(self);
    hawk_free(self);
}

static const LV2_Descriptor descriptor = {
    HAWK_URI,
    instantiate,
    connect_port,
    activate,
    run,
    deactivate,
    cleanup,
    NULL  // extension_data
};

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
    return (index == 0) ? &descriptor : NULL;
}
