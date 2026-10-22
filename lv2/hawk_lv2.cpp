// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
// The Hawk — LV2 plugin (DSP / audio side). The faceplate GUI lives separately
// in hawk_ui.cpp; this file has no UI code.
//
// Passes audio through (with an optional smoothed dim/mute), and exposes the
// detected note, cents, clarity and frequency as LV2 output control ports so a
// host's generic UI can display them. The heavy detection runs on a background
// worker thread; the audio thread only copies samples into a lock-free ring
// buffer and reads back the latest result, so run() stays real-time safe.

#include "fine_tuner.hpp"
#include "math_constants.hpp"
#include "note_mapping.hpp"
#include "pitch_detector.hpp"
#include "ring_buffer.hpp"
#include "tunings.hpp"

#include <lv2/core/lv2.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <chrono>
#include <cmath>
#include <cstdio>   // the HAWK_DEBUG_CENTS traces (a lab switch: no build defines it)
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <system_error>
#include <thread>
#include <vector>
#if defined(__SSE__)
#include <xmmintrin.h>
#endif

#define HAWK_URI "https://nylarea.com/lv2/themonohawk"

namespace {

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

struct Hawk {
    double sample_rate = 48000.0;
    // The analysis runs at sample_rate / decim, 44.1-48 kHz for the common rates (see Decimator).
    double   analysis_rate = 48000.0;
    unsigned decim         = 1;

    // Ports.
    const float* in        = nullptr;
    float*       out       = nullptr;
    const float* active    = nullptr;
    const float* dim_db    = nullptr;
    float*       p_note    = nullptr;
    float*       p_cents   = nullptr;
    float*       p_clarity = nullptr;
    float*       p_freq    = nullptr;
    float*       p_voiced  = nullptr;
    const float* tolerance = nullptr;
    const float* p_a4      = nullptr;
    const float* p_tuning  = nullptr;  // selected tuning index (the ping gate checks its strings)
    const float* ref_active = nullptr; // reference-tone on/off (UI toggle)
    const float* ref_note   = nullptr; // reference-tone target, MIDI note
    const float* ping_enable = nullptr; // in-tune ping on/off (Advanced Settings)
    float*       p_level    = nullptr;  // input level 0..1 (output, for the UI meter)
    const float* sensitivity = nullptr; // detection sensitivity 0..1 (input)
    float*       p_raise   = nullptr;
    float*       p_in_tune = nullptr;
    float*       p_lower   = nullptr;
    float*       p_strobe  = nullptr;  // the string's phase against the reference note (output)
    const float* strobe_mode = nullptr; // the GUI's strobe view on/off: the DSP ignores it, the host saves it
    const float* enabled     = nullptr; // lv2:enabled -- 0 = bypassed (the hawk in the GUI); unconnected = on

    // Bypass bookkeeping (audio thread). Coming back from a bypass, what the worker last published is from
    // before it: hide it until the worker publishes again (r_seq / p_seq move).
    bool     was_enabled = true;
    bool     stale       = false;
    uint32_t stale_seq   = 0;

    // Smoothed dim/mute gain.
    float current_gain = 1.0f;

    // Input-level meter (audio thread): peak-follower with instant attack and a
    // slow release, exposed on p_level so the UI can show "is it hearing me".
    float level_env = 0.0f;

    // Reference-tone oscillator (audio thread): a sine mixed under the
    // passthrough so the player can tune by ear. ref_osc_gain is ramped in/out
    // so engaging/releasing the tone never clicks; the rotation below keeps the
    // sine continuous across blocks (and across pitch changes).
    float  ref_osc_gain = 0.0f;

    // Confirmation "ping" (audio thread): a short bell tone fired on the rising
    // edge of the in-tune state, so the player hears each string land in tune.
    // ping_env is a fast-attack/exponential-decay envelope; 0 means silent (so
    // idle passthrough stays bit-exact).
    /* Both oscillators step a complex rotation instead of calling std::sin per sample: with
     * std::sin, the running reference tone took run() from 13.98 to 38.81 ns/sample (+178 %).
     * z = (cos phase, sin phase); multiplying by e^{j dphase} advances it. sin(2p) comes out free
     * as 2*sin(p)*cos(p), which is what the ping's second partial needs.
     * ⚠ A rotation drifts in magnitude, so run() renormalises it once per block. */
    double ref_zr = 1.0, ref_zi = 0.0, ref_step_c = 1.0, ref_step_s = 0.0, ref_step_for = -1.0;
    double png_zr = 1.0, png_zi = 0.0, png_step_c = 1.0, png_step_s = 0.0, png_step_for = -1.0;
    float  ping_env   = 0.0f;

    // Per-block constants that depend only on the sample rate, computed once in instantiate().
    float gain_coeff    = 0.0f;   // one-pole ~10 ms: the dim and the reference tone's fades
    float ping_decay    = 0.0f;   // ping envelope, ~0.13 s
    float level_release = 0.0f;   // level meter release, ~0.3 s

    // Tuning-lamp stabiliser state (see run()): -1 none, 0 raise, 1 in tune,
    // 2 lower.
    int      tune_zone     = -1;
    int      cand_zone     = -1;
    uint32_t cand_samps    = 0;
    uint32_t release_samps = 0;
    uint32_t in_tune_samps = 0;  // how long we've been latched "in tune"

    // Detection, run off the audio thread. The ring holds the HOST-rate stream (decimated by the worker), so at
    // 192 kHz it fills four times faster than at 48 kHz: sized for that headroom.
    hawk::RingBuffer   ring{1u << 16};
    std::thread        worker;
    std::atomic<bool>  running{false};
    /* With the ring empty the worker waits on a condition variable with a 3 ms timeout, and
     * deactivate() notifies it, so a stop is seen immediately instead of on the next tick (a flat
     * 3 ms sleep made deactivate() wait 2.92 ms on average, 3.23 ms worst).
     *
     * ⚠ ONLY THE STOP IS SIGNALLED, deliberately. The obvious extension is to have run() notify
     * the worker whenever it pushes audio, which would remove the idle poll entirely -- but that
     * puts a mutex and a futex wake on the AUDIO THREAD, which is exactly what a real-time path
     * must not do. The 3 ms timed wait stays as the data path; it costs a wakeup every 3 ms on a
     * thread that is idle anyway. Closing fast is worth a condition variable; feeding is not. */
    std::mutex              wake_m;
    std::condition_variable wake_cv;
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
     * hawk::GapTracker keeps the first and last unhandled seam; see ring_buffer.hpp. */
    hawk::GapTracker gaps;

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
    struct Reading {
        float note    = 0.0f;
        float cents   = 0.0f;
        float clarity = 0.0f;
        float freq    = 0.0f;
        float strobe  = 0.0f;   // radians, (-pi, pi]: see the worker
        int   voiced  = 0;
    };
    std::atomic<uint32_t> r_seq{0};      // odd while the writer is inside
    std::atomic<float>    r_note{0.0f};
    std::atomic<float>    r_cents{0.0f};
    std::atomic<float>    r_clarity{0.0f};
    std::atomic<float>    r_freq{0.0f};
    std::atomic<float>    r_strobe{0.0f};
    std::atomic<int>      r_voiced{0};

    // Worker side. Only the worker thread calls this, so the counter needs no read-modify-write.
    void publish(const Reading& v)
    {
        const uint32_t s = r_seq.load(std::memory_order_relaxed);
        r_seq.store(s + 1, std::memory_order_relaxed);          // open: odd
        std::atomic_thread_fence(std::memory_order_release);
        r_note   .store(v.note,    std::memory_order_relaxed);
        r_cents  .store(v.cents,   std::memory_order_relaxed);
        r_clarity.store(v.clarity, std::memory_order_relaxed);
        r_freq   .store(v.freq,    std::memory_order_relaxed);
        r_strobe .store(v.strobe,  std::memory_order_relaxed);
        r_voiced .store(v.voiced,  std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        r_seq.store(s + 2, std::memory_order_release);          // closed: even
    }

    /* Audio side. `fallback` is what to use if the writer happens to be inside right now.
     *
     * ⚠ The classic seqlock reader retries until it wins. An AUDIO THREAD must not: however
     * unlikely, "spin until another thread finishes" is an unbounded wait on a path with a
     * deadline. The writer's critical section is five relaxed stores, so a couple of attempts
     * covers everything short of the worker being preempted mid-publish -- and if that does
     * happen, showing last block's reading for 5 ms is invisible on a tuner display, while a
     * missed buffer is not. */
    Reading read_result(const Reading& fallback) const
    {
        for (int tries = 0; tries < 4; ++tries) {
            const uint32_t s0 = r_seq.load(std::memory_order_acquire);
            if (s0 & 1u) continue;                     // writer is inside
            Reading v;
            v.note    = r_note   .load(std::memory_order_relaxed);
            v.cents   = r_cents  .load(std::memory_order_relaxed);
            v.clarity = r_clarity.load(std::memory_order_relaxed);
            v.freq    = r_freq   .load(std::memory_order_relaxed);
            v.strobe  = r_strobe .load(std::memory_order_relaxed);
            v.voiced  = r_voiced .load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (r_seq.load(std::memory_order_relaxed) == s0) return v;
        }
        return fallback;
    }
    Reading last_reading{};   // audio thread only: what to fall back to
    std::atomic<float> a4_ref{440.0f};  // tuning reference (audio thread -> worker)
    std::atomic<float> sens{0.5f};      // detection sensitivity 0..1 (audio -> worker)
    std::atomic<int>   tuning_idx{0};   // selected tuning index (audio -> worker / ping gate)

    void worker_loop();
};

// A control input as its TTL declares it. A host or UI can write anything into a control port, and one
// non-finite value must not reach the DSP: a NaN Dim Amount for ONE block used to turn every later
// output sample into NaN until the plugin was re-activated. NaN takes the default, +-Inf and anything
// out of range clamp to the bounds.
static float port_value(const float* p, float lo, float hi, float def)
{
    if (p == nullptr) return def;
    const float v = *p;
    if (v != v) return def;
    return std::min(std::max(v, lo), hi);
}

// An input sample the analysis can use: a non-finite one becomes silence. One NaN or Inf reaching the
// stream high-pass or the onset envelope stays in their state for good, and the tuner went deaf until
// re-activated.
static inline float finite_or_zero(float x)
{
    return (std::fabs(x) <= std::numeric_limits<float>::max()) ? x : 0.0f;
}

// The high-pass that keeps sub-audio rumble out of the analysis. 4th-order Butterworth at
// 20 Hz as two 2nd-order sections (Butterworth Qs 0.5412 and 1.3066), each designed by the bilinear
// transform with the cutoff prewarped, run in double, direct form I.
//
// It runs on the CONTINUOUS stream, not on each window, and that is the whole point (measured,
// tools/rumble_bench.cpp): started afresh in every window, its start-up sits
// inside the window and biases CLEAN bass notes (6.1 c on B0 at 2nd order, 30 c at 4th); carried
// across windows it leaves clean notes at 0.00 c. Under 9 Hz
// rumble, without it, a rumble of 0.1 made D3/E4 read a different note (1196 / 1901 c) and 0.2
// muted the bass; with it, worst 0.01 c at 0.1, 2.6 c on B0 at 0.2, 6.2 c on B0 at 0.4.
struct HighPass20 {
    struct Section { double b0, b1, b2, a1, a2, x1, x2, y1, y2; };
    Section sec[2];
    explicit HighPass20(double sample_rate)
    {
        const double q[2] = { 0.54119610, 1.30656296 };
        const double w = hawk::TWO_PI * 20.0 / sample_rate;
        const double c = std::cos(w), sn = std::sin(w);
        for (int k = 0; k < 2; ++k) {
            const double al = sn / (2.0 * q[k]), a0 = 1.0 + al;
            sec[k] = { (1.0 + c) / 2.0 / a0, -(1.0 + c) / a0, (1.0 + c) / 2.0 / a0,
                       -2.0 * c / a0, (1.0 - al) / a0, 0.0, 0.0, 0.0, 0.0 };
        }
    }
    // As if `x` had been the input forever: the output starts at 0 instead of with a step.
    void prime(double x)
    {
        for (Section& s : sec) { s.x1 = s.x2 = x; s.y1 = s.y2 = 0.0; x = 0.0; }
    }
    float run(float in)
    {
        double v = in;
        for (Section& s : sec) {
            const double y = s.b0 * v + s.b1 * s.x1 + s.b2 * s.x2 - s.a1 * s.y1 - s.a2 * s.y2;
            s.x2 = s.x1; s.x1 = v; s.y2 = s.y1; s.y1 = y;
            v = y;
        }
        return static_cast<float>(v);
    }
};

// The analysis input at 44.1-48 kHz whatever the host's rate. Above 50 kHz the worker used to do
// everything at full rate -- at 96 kHz a 16384 window and a 262144-point long FFT, twice the work for
// no gain: the tuner reads fundamentals up to ~1.5 kHz and partials of a few kHz. So the input goes
// through 1-4 halving stages first. Each is a half-band low-pass, a Blackman-windowed sinc of 23 taps
// (11 non-zero), keeping every other sample: designed and checked at 96 kHz, ripple 0.0013 dB up to
// 10 kHz and -77.9 dB from 38 kHz, linear phase (a constant delay moves no reading).
struct HalfBand {
    static constexpr int N = 23, C = 11, MASK = 31;
    double h[N];
    double x[MASK + 1];
    unsigned pos = 0;
    bool     odd = false;
    HalfBand()
    {
        double sum = 0.0;
        for (int n = 0; n < N; ++n) {
            const double t = 0.5 * (n - C);
            const double sinc = (n == C) ? 1.0 : std::sin(hawk::PI * t) / (hawk::PI * t);
            const double a = hawk::TWO_PI * n / (N - 1);
            h[n] = 0.5 * sinc * (0.42 - 0.5 * std::cos(a) + 0.08 * std::cos(2.0 * a));
            sum += h[n];
        }
        for (double& v : h) v /= sum;   // unity gain at DC
        prime(0.0);
    }
    // As if `v` had been the input forever (the output then starts at v, without a ramp).
    void prime(double v)
    {
        for (double& e : x) e = v;
        pos = 0;
        odd = false;
    }
    // One input sample; every second call returns true with an output sample.
    bool push(double in, double& out)
    {
        x[pos & MASK] = in;
        const bool emit = odd;
        odd = !odd;
        if (emit) {
            double acc = h[C] * x[(pos - C) & MASK];
            for (int k = 1; k < C; k += 2)   // the taps at even offsets from the centre are zero
                acc += h[C - k] * (x[(pos - C + k) & MASK] + x[(pos - C - k) & MASK]);
            out = acc;
        }
        ++pos;
        return emit;
    }
};

struct Decimator {
    HalfBand stage[4];
    int      stages = 0;              // log2 of the factor
    void prime(double v) { for (int k = 0; k < stages; ++k) stage[k].prime(v); }
    bool push(double in, double& out)
    {
        for (int k = 0; k < stages; ++k)
            if (!stage[k].push(in, in)) return false;
        out = in;
        return true;
    }
};

// The newest `size` samples of the stream, kept circularly: appending copies only the new samples,
// and the window is put in order only when an analysis reads it. (Sliding a linear buffer moved the
// whole window on every pop: 345 KB for the 1.8 s window at 48 kHz, some 300 times a second.)
struct StreamTail {
    std::vector<float> buf;
    std::size_t        head = 0;                  // where the next sample goes: the oldest one
    explicit StreamTail(std::size_t size) : buf(size, 0.0f) {}
    void clear()
    {
        std::fill(buf.begin(), buf.end(), 0.0f);
        head = 0;
    }
    void push(const float* src, std::size_t n)
    {
        const std::size_t size = buf.size();
        if (n > size) { src += n - size; n = size; }      // only the newest window-full matters
        const std::size_t first = std::min(n, size - head);
        std::memcpy(buf.data() + head, src, first * sizeof(float));
        std::memcpy(buf.data(), src + first, (n - first) * sizeof(float));
        head = (head + n) % size;
    }
    void copy_to(float* dst) const                        // oldest first, buf.size() samples
    {
        const std::size_t size = buf.size();
        std::memcpy(dst, buf.data() + head, (size - head) * sizeof(float));
        std::memcpy(dst + (size - head), buf.data(), head * sizeof(float));
    }
};

// Analysis window of at least ~170 ms at the ANALYSIS rate, enough to resolve low B (~31 Hz): 8192
// samples at 44.1/48 kHz, which is where the decimator puts every common host rate. The doubling and
// its 16384 cap only matter for an analysis rate between 48.2 and 50 kHz.
static std::size_t analysis_window_size(double sample_rate)
{
    std::size_t window_size = 8192;
    while (window_size < static_cast<std::size_t>(sample_rate * 0.17) &&
           window_size < 16384)
        window_size <<= 1;
    return window_size;
}

void Hawk::worker_loop()
{
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
    const double rate = analysis_rate;
    const std::size_t window_size  = analysis_window_size(rate);
    const std::size_t analysis_hop = window_size / 4;

    const hawk::PitchDetector coarse(rate);
    const hawk::FineTuner     fine(rate);

    StreamTail         wtail(window_size);           // the analysis window as it streams in
    std::vector<float> window(window_size, 0.0f);    // ... and in order, at each analysis
    std::vector<float> chunk(2048);
    std::vector<float> chunk_hp(chunk.size());       // the same samples, high-passed
    HighPass20 hp(rate);
    bool hp_primed = false;                          // primed on the first sample, and after a seam
    Decimator dec;
    for (unsigned d = decim; d > 1; d >>= 1) ++dec.stages;
    std::size_t dpos = 0;                            // stream position at the analysis rate

    std::size_t total_seen     = 0;
    std::size_t since_analysis = 0;

    // Short median history to reject single-frame cents outliers.
    Reading cur{};                    // what is currently published; the worker owns it
    double cents_hist[5] = {0, 0, 0, 0, 0};
    [[maybe_unused]] int hist_note = -1;     // the note the readings in cents_hist belong to
    int    hist_count = 0;
    int    hist_pos   = 0;

    /* A pluck starts SHARP (tension rises with amplitude) and the pick clicks. A window is
     * 170 ms and they overlap 75 %, so one attack reaches four analyses in a row and the median of
     * five cannot reject it: measured (tools/attack_probe.cpp, +20 c at onset, tau 80 ms), a re-pluck
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
    constexpr double ATTACK_S    = 0.100;         // what an attack is taken to last
    constexpr double ONSET_LAG_S = 0.050;         // "just before": the reference is this far back
    constexpr double ONSET_RATIO = 2.5;           // energy now / energy then, to count as an onset (4 dB)
    constexpr double REARM_RATIO = 1.5;           // ... and below which it may fire again (hysteresis)
    constexpr double ONSET_FLOOR = 1.0e-7;        // below this power nothing is an onset (~-70 dBFS)
    constexpr std::size_t ENV_DECIM = 16;         // the envelope history is kept every 16 samples
    // two one-poles of 10 ms in cascade: ~6 % of ripple left even at B0, far inside the hysteresis
    const double env_a = 1.0 - std::exp(-1.0 / (0.010 * rate));
    const std::size_t attack_len = static_cast<std::size_t>(ATTACK_S * rate);
    std::vector<double> env_hist(static_cast<std::size_t>(ONSET_LAG_S * rate) / ENV_DECIM + 1, 0.0);
    std::size_t env_head = 0, env_tick = 0;
    double      env1 = 0.0, env = 0.0;
    bool        onset_armed = true;
    std::size_t onset_end   = 0;                  // stream position where the latest attack ends
    bool        seen_onset  = false;

    /* The other end of a note. A note stopped dead (a hand on the strings, a mute) leaves windows that
     * hold part note, part silence, and analysing them answers about a waveform that was never played:
     * measured, A2 published -6.7 c and a frequency 28 c off for ~170 ms after the stop, lamp still lit.
     * A fast power envelope (2 ms) falling 20 dB under the slow one marks the stop: no plucked string
     * decays that fast (a palm mute does, and it ends the note too), and the lowest bass note's power
     * ripple dips the fast envelope by at most ~7 dB. No window holding the stop feeds the reading;
     * "no signal" is published instead, which is what the player just did. */
    constexpr double OFFSET_RATIO = 100.0;        // slow power / fast power, to count as a stop (20 dB)
    const double fast_a = 1.0 - std::exp(-1.0 / (0.002 * rate));
    double      fast = 0.0;
    std::size_t offset_pos  = 0;                  // stream position of the latest stop
    bool        seen_offset = false;

    while (running.load(std::memory_order_acquire)) {
        const std::size_t r0  = ring.read_pos();      // the worker owns the read side: exact
        std::size_t got = ring.pop(chunk.data(), chunk.size());
        if (got == 0) {
            std::unique_lock<std::mutex> lk(wake_m);
            wake_cv.wait_for(lk, std::chrono::milliseconds(3),
                             [this] { return !running.load(std::memory_order_acquire); });
            continue;
        }

        /* Has this pop carried the read pointer past a seam? Then the window built so far straddles
         * it, and analysing it would answer about a waveform that was never played. Start over, and
         * keep only the part of this pop that comes after the seam: a missing reading for a
         * fraction of a second is honest, a confident wrong note is not.
         *
         * A pop that ends exactly ON the seam is clean: its last sample is the last one before the
         * loss, and the next pop starts the restart. */
        const hawk::GapTracker::Verdict gv = gaps.consume(r0, r0 + got);
        if (gv.reset) {
            wtail.clear();
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
            dec.prime(x0);
            hp.prime(x0);
            hp_primed = true;
        }
        {   // raw pointers on purpose: with _GLIBCXX_ASSERTIONS (every non-release build) the per-sample
            // checked operator[] cost +0.46 points of a core here (measured, paired; +0.02 without the
            // assertions). Same arithmetic, bounds already checked above.
            const float* src = chunk.data() + gv.skip;
            float*       dst = chunk_hp.data();
            if (dec.stages == 0) {
                for (std::size_t j = 0; j < got; ++j) dst[j] = hp.run(finite_or_zero(src[j]));
            } else {
                std::size_t nd = 0;
                for (std::size_t j = 0; j < got; ++j) {
                    double v;
                    if (dec.push(finite_or_zero(src[j]), v)) dst[nd++] = hp.run(static_cast<float>(v));
                }
                got = nd;                                // from here on, samples at the analysis rate
            }
        }
        if (got == 0) continue;
        const std::size_t pop_pos = dpos;                // stream position of the pop's first sample
        dpos += got;

        /* The analysis grid is fixed in the STREAM: the pop is cut at every analysis point, so a window
         * ends exactly every analysis_hop samples however the ring was drained. Analysing "when at least
         * a hop has arrived" and then restarting the count put the analyses where the pops happened to
         * end, so how often and where they fell followed this thread's speed and the machine's load:
         * under ThreadSanitizer (a slow worker draining big pops) a string 14 c flat in a drop-tuning
         * strum showed in tune half the time, while the same code at full speed showed it flat. */
        for (std::size_t off = 0, seg = 0; off < got; off += seg) {
            const std::size_t need = (total_seen < window_size)
                ? std::max(window_size - total_seen, analysis_hop - std::min(since_analysis, analysis_hop))
                : analysis_hop - since_analysis;
            seg = std::min(got - off, need);
            const float* fresh = chunk_hp.data() + off;
            const std::size_t fresh_pos = pop_pos + off;        // stream position of fresh[0]
            const std::size_t end_pos   = fresh_pos + seg;        // one past the window's newest sample

            // onset detection on the incoming samples
            for (std::size_t j = 0; j < seg; ++j) {
                const double e = static_cast<double>(fresh[j]) * fresh[j];
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
                env_head = (env_head + 1 == env_hist.size()) ? 0 : env_head + 1;
                if (onset_armed && env > ONSET_FLOOR && env > ONSET_RATIO * then) {
                    seen_onset  = true;
                    onset_armed = false;
                    onset_end   = fresh_pos + j + attack_len;
                } else if (!onset_armed && env < REARM_RATIO * then) {
                    onset_armed = true;
                }
            }

            wtail.push(fresh, seg);
            total_seen     += seg;
            since_analysis += seg;

            if (total_seen < window_size || since_analysis < analysis_hop) {
                continue;
            }
            since_analysis = 0;
            wtail.copy_to(window.data());

            /* Digital silence (a muted or idle track): nothing to analyse, so publish "no signal" and stop
             * here. Without this the coarse detector ran every hop on zeros at nearly a note's cost, 1.2 %
             * of a core per instance. -120 dBFS RMS is below any real input's noise floor, and the
             * high-pass tail of a note that stopped falls below it within a fraction of a second. */
            double energy = 0.0;
            for (std::size_t i = 0; i < window_size; ++i)
                energy += static_cast<double>(window[i]) * window[i];
            const double rms = std::sqrt(energy / static_cast<double>(window_size));
            constexpr double SILENCE_RMS = 1.0e-6;
            if (rms < SILENCE_RMS) {
                if (cur.voiced || cur.clarity != 0.0f) {
                    cur.voiced  = 0;
                    cur.clarity = 0.0f;
                    publish(cur);
                }
                hist_count = 0;
                continue;
            }

            const double a4 = a4_ref.load(std::memory_order_relaxed);


            // ---- Monophonic path --------------------------------------------
            // A window that still holds an attack does not feed the reading: keep what is
            // published, as it is, until a clean window arrives.
            if (seen_offset && end_pos - window_size < offset_pos) {   // the window holds a stop
                if (cur.voiced) {
                    cur.voiced = 0;
                    publish(cur);
                }
                hist_count = 0;
                continue;
            }
            if (seen_onset && end_pos - window_size < onset_end) continue;

            const hawk::PitchEstimate c = coarse.analyze(window.data(), window_size);
            /* The worker is the only writer, so it can keep its own copy of what is published and
             * amend it -- the unvoiced path below wants to update clarity and voiced while leaving
             * the last note/cents/freq standing, which is only expressible if the set is written as
             * a set. */
            cur.clarity = static_cast<float>(c.clarity);

            // Sensitivity remaps the clarity gate: 0.5 = default (~0.85), higher =
            // more sensitive (lower threshold → picks up weaker / noisier signals).
            float gate = 0.85f + (0.5f - sens.load(std::memory_order_relaxed)) * 0.40f;
            if (gate < 0.50f) gate = 0.50f;
            if (gate > 0.99f) gate = 0.99f;
            if (!(c.frequency > 0.0 && c.clarity >= gate)) {
                cur.voiced = 0;
                publish(cur);
                hist_count = 0;  // start fresh for the next note
                continue;
            }
            const hawk::NoteReading nr = hawk::frequency_to_note(c.frequency, a4);
            const double target = hawk::note_to_frequency(nr.midi_note, a4);
            const hawk::FineEstimate f = fine.refine(window.data(), window_size,
                                                     target);
            /* The fine stage's own confidence vetoes the window. Below MIN_COHERENCE the
             * fundamental at the chosen note did not turn steadily -- in the windows measured that is the
             * coarse stage having picked the WRONG note, and falling back to its cents would publish that
             * wrong note with confidence. So: no reading for this window.
             *
             * Where the threshold sits, measured (tools/coherence_bench.cpp): legitimate windows
             * >= 0.994 (clean notes 1.0000; a peg turned at 200 c/s 0.9994; vibrato +-30 c 0.9969; a note
             * in white noise at 0 dB SNR 0.994); the two wrong-note windows found (9 Hz rumble 0.1 on D3
             * and E4, 1196 and 1901 c off) 0.900 and 0.956. 0.975 leaves ~0.02 on either side. */
            constexpr double MIN_COHERENCE = 0.975;
            if (f.coherence < MIN_COHERENCE) {
                cur.voiced = 0;
                publish(cur);
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
             * HAWK_OLD_NOTE_SNAP=1 restores both, for tools/boundary_probe.cpp's negative arm only. */
            bool resnapped = false;
            if (f.valid && std::fabs(cents) > 50.0) {
                const hawk::NoteReading fr = hawk::frequency_to_note(f.frequency, a4);
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
            const int n_med = std::min(hist_count, 5);  // always in [1, 5] here
            double sorted[5];
            for (int k = 0; k < n_med; ++k) sorted[k] = cents_hist[k];
            // GCC 15 at -O3 emits a bogus -Warray-bounds from inside std::sort's
            // inlined body (it loses track that n_med <= 5 across the heap-loaded count).
            // The access is provably in bounds; suppress just this known false
            // positive rather than contorting the code around a compiler bug.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
            std::sort(sorted, sorted + n_med);
#pragma GCC diagnostic pop
            const double cents_median = sorted[n_med / 2];

            /* The strobe: the string's phase against the reference note, in STREAM time. The fine stage's
             * phase is against a reference that starts at 0 on the window's first sample, so from one
             * window to the next it jumps by the string's own phase advance; taking off the reference's
             * phase at that first sample (2 pi f0 t, t = its stream position) leaves phi(t) - 2 pi f0 t,
             * which turns at 2 pi (f - f0) rad/s: still in tune, forward when sharp, back when flat. The
             * fine stage's two moving averages add a constant offset, which a strobe does not see.
             * Kept as it was when the window gives no fine reading. */
            if (f.valid && !resnapped) {
                const double turns = target * static_cast<double>(end_pos - window_size) / rate;
                double ph = f.phase - hawk::TWO_PI * (turns - std::floor(turns));
                ph = std::remainder(ph, hawk::TWO_PI);
                cur.strobe = static_cast<float>(ph);
            }
            cur.note   = static_cast<float>(note);
            cur.cents  = static_cast<float>(cents_median);
            cur.freq   = static_cast<float>(freq);
            cur.voiced = 1;
            publish(cur);
        }
    }
}

LV2_Handle instantiate(const LV2_Descriptor*, double rate, const char*,
                       const LV2_Feature* const*)
{
    // A rate no host runs at is refused, as the LV2 contract allows (NULL), instead of sizing the
    // buffers from it: a negative rate threw std::length_error out of instantiate() and aborted
    // the host, and 0 gave a handle with empty filters.
    if (!(rate >= 8000.0 && rate <= 768000.0)) return nullptr;
    try {
        Hawk* self = new Hawk();
        self->sample_rate = rate;
        while (rate / self->decim > 50000.0) self->decim *= 2;   // 88.2/96 kHz: 2, 176.4/192: 4, ...
        self->analysis_rate = rate / self->decim;
        const float srf = static_cast<float>(rate);
        self->gain_coeff    = 1.0f - std::exp(-1.0f / (0.01f * srf));
        self->ping_decay    = std::exp(-1.0f / (0.13f * srf));
        self->level_release = std::exp(-1.0f / (0.3f * srf));
        return static_cast<LV2_Handle>(self);
    } catch (const std::exception&) {   // out of memory: fail the instantiation, not the host
        return nullptr;
    }
}

// Stop and join the worker, if one runs. Shared by deactivate(), cleanup() and a repeated activate().
void stop_worker(Hawk* self)
{
    self->running.store(false);
    /* taking the lock before notifying guarantees the worker cannot miss the flag between its
     * predicate check and going to sleep */
    { std::lock_guard<std::mutex> g(self->wake_m); }
    self->wake_cv.notify_all();
    if (self->worker.joinable()) {
        self->worker.join();
    }
}

void connect_port(LV2_Handle handle, uint32_t port, void* data)
{
    Hawk* self = static_cast<Hawk*>(handle);
    switch (port) {
        case PORT_IN:      self->in        = static_cast<const float*>(data); break;
        case PORT_OUT:     self->out       = static_cast<float*>(data);       break;
        case PORT_ACTIVE:  self->active    = static_cast<const float*>(data); break;
        case PORT_DIM_DB:  self->dim_db    = static_cast<const float*>(data); break;
        case PORT_NOTE:    self->p_note    = static_cast<float*>(data);       break;
        case PORT_CENTS:   self->p_cents   = static_cast<float*>(data);       break;
        case PORT_CLARITY: self->p_clarity = static_cast<float*>(data);       break;
        case PORT_FREQ:    self->p_freq    = static_cast<float*>(data);       break;
        case PORT_VOICED:    self->p_voiced  = static_cast<float*>(data);       break;
        case PORT_TOLERANCE: self->tolerance = static_cast<const float*>(data); break;
        case PORT_RAISE:     self->p_raise   = static_cast<float*>(data);       break;
        case PORT_IN_TUNE:   self->p_in_tune = static_cast<float*>(data);       break;
        case PORT_LOWER:     self->p_lower   = static_cast<float*>(data);       break;
        case PORT_A4:        self->p_a4      = static_cast<const float*>(data); break;
        case PORT_TUNING:    self->p_tuning  = static_cast<const float*>(data); break;
        case PORT_REF_ACTIVE: self->ref_active = static_cast<const float*>(data); break;
        case PORT_REF_NOTE:   self->ref_note   = static_cast<const float*>(data); break;
        case PORT_PING_ENABLE: self->ping_enable = static_cast<const float*>(data); break;
        case PORT_LEVEL:       self->p_level     = static_cast<float*>(data);       break;
        case PORT_SENSITIVITY: self->sensitivity = static_cast<const float*>(data); break;
        case PORT_STROBE:      self->p_strobe      = static_cast<float*>(data);     break;
        case PORT_STROBE_MODE: self->strobe_mode   = static_cast<const float*>(data); break;
        case PORT_ENABLED:     self->enabled       = static_cast<const float*>(data); break;
        default: break;
    }
}

void activate(LV2_Handle handle)
{
    Hawk* self = static_cast<Hawk*>(handle);
    // activate() twice without deactivate() breaks the host contract, but assigning a new thread
    // over a running one calls std::terminate and takes the host down with it.
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

    self->publish(Hawk::Reading{});   // no worker running yet, so this cannot race
    self->last_reading = Hawk::Reading{};

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
    self->ring.clear();
    // clear() rewinds both stream positions to 0, so a gap recorded against the OLD numbering would
    // now name a point the reader is already past -- and fire on the first pop after restart
    self->gaps.clear();

    self->running.store(true);
#if !HAWK_THREAD_UNGUARDED
    // Creating a thread can fail (the system's task limit): std::thread then throws, and an exception out of
    // activate() terminates the HOST. Without a worker nothing is analysed -- the audio still passes and the tuner
    // shows no note. (HAWK_THREAD_UNGUARDED=1 drops the guard, for tools/thread_probe.cpp's negative arm only.)
    try {
        self->worker = std::thread(&Hawk::worker_loop, self);
    } catch (const std::system_error&) {
        self->running.store(false);
    }
#else
    self->worker = std::thread(&Hawk::worker_loop, self);
#endif
}

void run(LV2_Handle handle, uint32_t n_samples)
{
    Hawk* self = static_cast<Hawk*>(handle);
    if (self->in == nullptr || self->out == nullptr) {
        return;
    }

    /* The plugin's own bypass (lv2:enabled: the host's bypass and the hawk in the GUI are this one port).
     * Bypassed, the input passes untouched once the dim and the tones have faded (10 ms), nothing is analysed
     * and the tuner shows nothing. Back from it, the stream has a hole where the bypass was: a seam restarts
     * the analysis after it, and the last readings published before it stay hidden until fresh ones come. */
    const bool enabled = port_value(self->enabled, 0.0f, 1.0f, 1.0f) > 0.5f;
    if (enabled && !self->was_enabled) {
        self->gaps.mark(self->ring.written());
        self->stale     = true;
        self->stale_seq = self->r_seq.load(std::memory_order_acquire);
    }
    self->was_enabled = enabled;

    // Feed the detector the dry input before we touch the output (in case the
    // host aliases the in and out buffers).
    if (enabled) {
        const std::size_t pushed = self->ring.push(self->in, n_samples);
        if (pushed < n_samples) {
            /* The ring was full, so what is in it ends exactly at the discontinuity. */
            self->gaps.mark(self->ring.written());
        }
    }

    const float active = enabled && self->active ? *self->active : 0.0f;
    const float dim_db = port_value(self->dim_db, -60.0f, 0.0f, -12.0f);
    const float a4_now = port_value(self->p_a4, 380.0f, 500.0f, 440.0f);
    self->a4_ref.store(a4_now, std::memory_order_relaxed);
    self->sens.store(port_value(self->sensitivity, 0.0f, 1.0f, 0.5f), std::memory_order_relaxed);
    self->tuning_idx.store(static_cast<int>(std::lround(port_value(self->p_tuning, 0.0f, static_cast<float>(TUNING_COUNT - 1), 0.0f))),
                           std::memory_order_relaxed);
    const float target_gain =
        (active > 0.5f) ? std::pow(10.0f, dim_db / 20.0f) : 1.0f;

    // One-pole smoothing (~10 ms) so engaging the dim never clicks.
    const float coeff = self->gain_coeff;

    // Reference tone: a sine at the selected string's pitch, faded in/out and
    // mixed under the passthrough. When it is fully off we skip the mix entirely,
    // so unused/idle passthrough stays bit-exact.
    constexpr float REF_LEVEL = 0.20f;             // tone amplitude when engaged
    using hawk::TWO_PI;
    const bool  ref_on    = enabled && self->ref_active && *self->ref_active > 0.5f;
    const float ref_tgt   = ref_on ? REF_LEVEL : 0.0f;
    const bool  mix_ref   = ref_on || self->ref_osc_gain > 0.0f;
    double ref_dphase = 0.0;
    if (mix_ref) {
        const int rn = static_cast<int>(std::lround(port_value(self->ref_note, 0.0f, 127.0f, 40.0f)));
        ref_dphase = TWO_PI * hawk::note_to_frequency(rn, a4_now) / self->sample_rate;
    }

    // Ping oscillator constants: a soft, warm pitch (E5) with a quiet octave
    // partial for a touch of bell, and a short ~0.13 s exponential decay.
    constexpr float  PING_LEVEL = 0.18f;
    constexpr double PING_FREQ  = 659.26;   // E5 — warmer, less piercing than E6
    const double ping_dphase = TWO_PI * PING_FREQ / self->sample_rate;
    /* The rotation step only changes when the frequency does -- a note change for the reference,
     * never for the ping -- so the two transcendentals are paid per BLOCK at worst, not per sample.
     * The phase itself lives in z and carries across a frequency change untouched, which is what
     * keeps the tone continuous when the target note moves. */
    if (ref_dphase != self->ref_step_for) {
        self->ref_step_c = std::cos(ref_dphase);
        self->ref_step_s = std::sin(ref_dphase);
        self->ref_step_for = ref_dphase;
    }
    if (ping_dphase != self->png_step_for) {
        self->png_step_c = std::cos(ping_dphase);
        self->png_step_s = std::sin(ping_dphase);
        self->png_step_for = ping_dphase;
    }
    /* Renormalise once per block. The drift is ~1e-16 relative per step, so over any realistic
     * block it is far below what a synthesised reference tone needs -- but left unbounded it
     * accumulates for the life of the plugin, and this costs two divisions per block. */
    {
        const double rn = 1.0 / std::sqrt(self->ref_zr * self->ref_zr + self->ref_zi * self->ref_zi);
        self->ref_zr *= rn; self->ref_zi *= rn;
        const double pn = 1.0 / std::sqrt(self->png_zr * self->png_zr + self->png_zi * self->png_zi);
        self->png_zr *= pn; self->png_zi *= pn;
    }
    const float ping_decay = self->ping_decay;

    // Level meter: peak-follower with ~0.3 s release on the dry input.
    const float level_release = self->level_release;

    for (uint32_t i = 0; i < n_samples; ++i) {
        const float ain = std::fabs(finite_or_zero(self->in[i]));   // an Inf would pin the meter
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
        if (std::fabs(self->current_gain - target_gain) < 1.0e-4f)
            self->current_gain = target_gain;

        float s = self->in[i] * self->current_gain;
        if (mix_ref) {
            self->ref_osc_gain += (ref_tgt - self->ref_osc_gain) * coeff;
            // Once a release has decayed to silence, snap to exactly 0 so the
            // next block takes the bit-exact passthrough path again.
            if (!ref_on && self->ref_osc_gain < 1.0e-5f) self->ref_osc_gain = 0.0f;
            s += static_cast<float>(self->ref_zi) * self->ref_osc_gain;
            const double nzr = self->ref_zr * self->ref_step_c - self->ref_zi * self->ref_step_s;
            const double nzi = self->ref_zr * self->ref_step_s + self->ref_zi * self->ref_step_c;
            self->ref_zr = nzr; self->ref_zi = nzi;
        }
        if (self->ping_env > 0.0f) {
            /* sin(2p) = 2 sin(p) cos(p), so the second partial is free from the same rotation */
            s += PING_LEVEL * self->ping_env *
                 static_cast<float>(self->png_zi + 0.22 * (2.0 * self->png_zi * self->png_zr));
            const double qzr = self->png_zr * self->png_step_c - self->png_zi * self->png_step_s;
            const double qzi = self->png_zr * self->png_step_s + self->png_zi * self->png_step_c;
            self->png_zr = qzr; self->png_zi = qzi;
            self->ping_env *= ping_decay;
            if (self->ping_env < 1.0e-4f) self->ping_env = 0.0f;  // snap -> bit-exact
        }
        self->out[i] = s;
    }
    if (!enabled) self->level_env = 0.0f;
    if (self->p_level) *self->p_level = std::min(self->level_env, 1.0f);

    /* One read per block, of the whole set. The old code loaded r_cents TWICE -- once for the
     * output port and once for the lamp logic -- so the number the user saw and the number the
     * lamps were decided from could come from different analyses. */
    Hawk::Reading rd = self->read_result(self->last_reading);
    self->last_reading = rd;
    if (self->stale && self->r_seq.load(std::memory_order_acquire) != self->stale_seq) self->stale = false;
#if !HAWK_BYPASS_KEEPS_READING
    // the whole reading, not just the flags: a host's generic UI or an automation lane shows these ports as they are
    // (HAWK_BYPASS_KEEPS_READING=1 restores the old, flags only, for tools/bypass_probe.cpp's negative arm only)
    if (!enabled || self->stale) rd = Hawk::Reading{};
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
    if (self->p_voiced) *self->p_voiced = static_cast<float>(voiced_now);

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
    constexpr double IN_TUNE_HOLD_S = 1.3;   // <-- the "hold long enough to read" knob
    constexpr double ENTER_DEBOUNCE_S = 0.03;
    constexpr double SWITCH_DEBOUNCE_S = 0.12;
    constexpr double RELEASE_HOLD_S = 0.40;

    const float    tol      = port_value(self->tolerance, 0.5f, 10.0f, 2.0f);
    const float    hyst     = 1.5f;
    const uint32_t min_hold = static_cast<uint32_t>(IN_TUNE_HOLD_S   * self->sample_rate);
    const uint32_t enter_db = static_cast<uint32_t>(ENTER_DEBOUNCE_S * self->sample_rate);
    const uint32_t switch_db= static_cast<uint32_t>(SWITCH_DEBOUNCE_S* self->sample_rate);
    const uint32_t release  = static_cast<uint32_t>(RELEASE_HOLD_S   * self->sample_rate);

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
        if (std::fabs(cents_now) <= band) raw = 1;
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
        const int det = static_cast<int>(std::lround(rd.note));
        int tgt[TUNING_MAX_STRINGS];   /* sized from the table, not guessed */
        const int nt = build_targets(self->tuning_idx.load(std::memory_order_relaxed), tgt);
        for (int i = 0; i < nt; ++i)
            if (tgt[i] == det) { fire_ping = true; break; }
    }
    if (fire_ping) {
        self->ping_env = 1.0f;
        self->png_zr = 1.0; self->png_zi = 0.0;
    }
}

void deactivate(LV2_Handle handle)
{
    stop_worker(static_cast<Hawk*>(handle));
}

void cleanup(LV2_Handle handle)
{
    Hawk* self = static_cast<Hawk*>(handle);
    stop_worker(self);
    delete self;
}

const LV2_Descriptor descriptor = {
    HAWK_URI,
    instantiate,
    connect_port,
    activate,
    run,
    deactivate,
    cleanup,
    nullptr  // extension_data
};

} // namespace

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
    return (index == 0) ? &descriptor : nullptr;
}
