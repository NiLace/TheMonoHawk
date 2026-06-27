// The Hawk — LV2 plugin (DSP / audio side). The faceplate GUI lives separately
// in hawk_ui.cpp; this file has no UI code.
//
// Passes audio through (with an optional smoothed dim/mute), and exposes the
// detected note, cents, clarity and frequency as LV2 output control ports so a
// host's generic UI can display them. The heavy detection runs on a background
// worker thread; the audio thread only copies samples into a lock-free ring
// buffer and reads back the latest result, so run() stays real-time safe.

#include "fine_tuner.hpp"
#include "note_mapping.hpp"
#include "pitch_detector.hpp"
#include "ring_buffer.hpp"
#include "tunings.hpp"

#include <lv2/core/lv2.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#define HAWK_URI "https://themonohawk.audio/lv2/tuner"

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
};

struct Hawk {
    double sample_rate = 48000.0;

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
    const float* p_tuning  = nullptr;  // selected tuning index; display-only for now
    const float* ref_active = nullptr; // reference-tone on/off (UI toggle)
    const float* ref_note   = nullptr; // reference-tone target, MIDI note
    const float* ping_enable = nullptr; // in-tune ping on/off (Advanced Settings)
    float*       p_level    = nullptr;  // input level 0..1 (output, for the UI meter)
    const float* sensitivity = nullptr; // detection sensitivity 0..1 (input)
    float*       p_raise   = nullptr;
    float*       p_in_tune = nullptr;
    float*       p_lower   = nullptr;

    // Smoothed dim/mute gain.
    float current_gain = 1.0f;

    // Input-level meter (audio thread): peak-follower with instant attack and a
    // slow release, exposed on p_level so the UI can show "is it hearing me".
    float level_env = 0.0f;

    // Reference-tone oscillator (audio thread): a sine mixed under the
    // passthrough so the player can tune by ear. ref_osc_gain is ramped in/out
    // so engaging/releasing the tone never clicks; ref_phase keeps the sine
    // continuous across blocks (and across pitch changes).
    double ref_phase    = 0.0;
    float  ref_osc_gain = 0.0f;

    // Confirmation "ping" (audio thread): a short bell tone fired on the rising
    // edge of the in-tune state, so the player hears each string land in tune.
    // ping_env is a fast-attack/exponential-decay envelope; 0 means silent (so
    // idle passthrough stays bit-exact).
    double ping_phase = 0.0;
    float  ping_env   = 0.0f;

    // Tuning-lamp stabiliser state (see run()): -1 none, 0 raise, 1 in tune,
    // 2 lower.
    int      tune_zone     = -1;
    int      cand_zone     = -1;
    uint32_t cand_samps    = 0;
    uint32_t release_samps = 0;
    uint32_t in_tune_samps = 0;  // how long we've been latched "in tune"

    // Detection, run off the audio thread. Ring sized for headroom at high
    // sample rates (a 16384 window at 192 kHz fills 4x faster than at 48 kHz).
    hawk::RingBuffer   ring{1u << 16};
    std::thread        worker;
    std::atomic<bool>  running{false};

    std::atomic<float> r_note{0.0f};
    std::atomic<float> r_cents{0.0f};
    std::atomic<float> r_clarity{0.0f};
    std::atomic<float> r_freq{0.0f};
    std::atomic<int>   r_voiced{0};
    std::atomic<float> a4_ref{440.0f};  // tuning reference (audio thread -> worker)
    std::atomic<float> sens{0.5f};      // detection sensitivity 0..1 (audio -> worker)
    std::atomic<int>   tuning_idx{0};   // selected tuning index (audio -> worker / ping gate)

    void worker_loop();
};

void Hawk::worker_loop()
{
    // Analysis window ~170 ms, scaled with the sample rate so it always spans
    // enough time to resolve low B (~31 Hz) at any rate. A fixed 8192 samples is
    // 171 ms at 48 kHz but only 43 ms at 192 kHz — too short for low bass, which
    // then silently stops being detected. Capped at 16384 so the O(N^2) detector
    // stays affordable at very high rates (8192 ≤48 kHz, 16384 at 96/192 kHz).
    std::size_t window_size = 8192;
    while (window_size < static_cast<std::size_t>(sample_rate * 0.17) &&
           window_size < 16384)
        window_size <<= 1;
    const std::size_t analysis_hop = window_size / 4;

    const hawk::PitchDetector coarse(sample_rate);
    const hawk::FineTuner     fine(sample_rate);

    std::vector<float> window(window_size, 0.0f);
    std::vector<float> chunk(2048);

    std::size_t total_seen     = 0;
    std::size_t since_analysis = 0;

    // Short median history to reject single-frame cents outliers.
    double cents_hist[5] = {0, 0, 0, 0, 0};
    int    hist_count = 0;
    int    hist_pos   = 0;

    while (running.load(std::memory_order_acquire)) {
        const std::size_t got = ring.pop(chunk.data(), chunk.size());
        if (got == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
            continue;
        }

        if (got < window_size) {
            std::memmove(window.data(), window.data() + got,
                         (window_size - got) * sizeof(float));
            std::memcpy(window.data() + (window_size - got), chunk.data(),
                        got * sizeof(float));
        } else {
            std::memcpy(window.data(), chunk.data() + (got - window_size),
                        window_size * sizeof(float));
        }
        total_seen     += got;
        since_analysis += got;

        if (total_seen < window_size || since_analysis < analysis_hop) {
            continue;
        }
        since_analysis = 0;

        const hawk::PitchEstimate c = coarse.analyze(window.data(),
                                                     window_size);
        const double a4 = a4_ref.load(std::memory_order_relaxed);
        r_clarity.store(static_cast<float>(c.clarity));


        // ---- Monophonic path --------------------------------------------
        // Sensitivity remaps the clarity gate: 0.5 = default (~0.85), higher =
        // more sensitive (lower threshold → picks up weaker / noisier signals).
        float gate = 0.85f + (0.5f - sens.load(std::memory_order_relaxed)) * 0.40f;
        if (gate < 0.50f) gate = 0.50f;
        if (gate > 0.99f) gate = 0.99f;
        if (!(c.frequency > 0.0 && c.clarity >= gate)) {
            r_voiced.store(0);
            hist_count = 0;  // start fresh for the next note
            continue;
        }
        const hawk::NoteReading nr = hawk::frequency_to_note(c.frequency, a4);
        const double target = hawk::note_to_frequency(nr.midi_note, a4);
        const hawk::FineEstimate f = fine.refine(window.data(), window_size,
                                                 target);
        const double cents = f.valid ? f.cents     : nr.cents;
        const double freq  = f.valid ? f.frequency : c.frequency;

        // Median of the last few cents readings.
        cents_hist[hist_pos] = cents;
        hist_pos = (hist_pos + 1) % 5;
        if (hist_count < 5) ++hist_count;
        const int n = std::min(hist_count, 5);  // always in [1, 5] here
        double sorted[5];
        for (int k = 0; k < n; ++k) sorted[k] = cents_hist[k];
        // GCC 15 at -O3 emits a bogus -Warray-bounds from inside std::sort's
        // inlined body (it loses track that n <= 5 across the heap-loaded count).
        // The access is provably in bounds; suppress just this known false
        // positive rather than contorting the code around a compiler bug.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
        std::sort(sorted, sorted + n);
#pragma GCC diagnostic pop
        const double cents_median = sorted[n / 2];

        r_note.store(static_cast<float>(nr.midi_note));
        r_cents.store(static_cast<float>(cents_median));
        r_freq.store(static_cast<float>(freq));
        r_voiced.store(1);
    }
}

LV2_Handle instantiate(const LV2_Descriptor*, double rate, const char*,
                       const LV2_Feature* const*)
{
    Hawk* self = new Hawk();
    self->sample_rate = rate;
    return static_cast<LV2_Handle>(self);
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
        default: break;
    }
}

void activate(LV2_Handle handle)
{
    Hawk* self = static_cast<Hawk*>(handle);
    self->current_gain = 1.0f;
    self->running.store(true);
    self->worker = std::thread(&Hawk::worker_loop, self);
}

void run(LV2_Handle handle, uint32_t n_samples)
{
    Hawk* self = static_cast<Hawk*>(handle);
    if (self->in == nullptr || self->out == nullptr) {
        return;
    }

    // Feed the detector the dry input before we touch the output (in case the
    // host aliases the in and out buffers).
    self->ring.push(self->in, n_samples);

    const float active = self->active ? *self->active : 0.0f;
    const float dim_db = self->dim_db ? *self->dim_db : 0.0f;
    if (self->p_a4) self->a4_ref.store(*self->p_a4, std::memory_order_relaxed);
    if (self->sensitivity)
        self->sens.store(*self->sensitivity, std::memory_order_relaxed);
    if (self->p_tuning)
        self->tuning_idx.store(static_cast<int>(std::lround(*self->p_tuning)),
                               std::memory_order_relaxed);
    const float target_gain =
        (active > 0.5f) ? std::pow(10.0f, dim_db / 20.0f) : 1.0f;

    // One-pole smoothing (~10 ms) so engaging the dim never clicks.
    const float coeff =
        1.0f - std::exp(-1.0f / (0.01f * static_cast<float>(self->sample_rate)));

    // Reference tone: a sine at the selected string's pitch, faded in/out and
    // mixed under the passthrough. When it is fully off we skip the mix entirely,
    // so unused/idle passthrough stays bit-exact.
    constexpr float REF_LEVEL = 0.20f;             // tone amplitude when engaged
    constexpr double TWO_PI   = 6.283185307179586;
    const bool  ref_on    = self->ref_active && *self->ref_active > 0.5f;
    const float ref_tgt   = ref_on ? REF_LEVEL : 0.0f;
    const bool  mix_ref   = ref_on || self->ref_osc_gain > 0.0f;
    double ref_dphase = 0.0;
    if (mix_ref) {
        int rn = self->ref_note ? static_cast<int>(std::lround(*self->ref_note)) : 40;
        if (rn < 0)   rn = 0;
        if (rn > 127) rn = 127;
        const double a4 = self->p_a4 ? static_cast<double>(*self->p_a4) : 440.0;
        ref_dphase = TWO_PI * hawk::note_to_frequency(rn, a4) / self->sample_rate;
    }

    // Ping oscillator constants: a soft, warm pitch (E5) with a quiet octave
    // partial for a touch of bell, and a short ~0.13 s exponential decay.
    constexpr float  PING_LEVEL = 0.18f;
    constexpr double PING_FREQ  = 659.26;   // E5 — warmer, less piercing than E6
    const double ping_dphase = TWO_PI * PING_FREQ / self->sample_rate;
    const float  ping_decay  =
        std::exp(-1.0f / (0.13f * static_cast<float>(self->sample_rate)));

    // Level meter: peak-follower with ~0.3 s release on the dry input.
    const float level_release =
        std::exp(-1.0f / (0.3f * static_cast<float>(self->sample_rate)));

    for (uint32_t i = 0; i < n_samples; ++i) {
        const float ain = std::fabs(self->in[i]);
        if (ain > self->level_env) self->level_env = ain;       // instant attack
        else                       self->level_env *= level_release;
        self->current_gain += (target_gain - self->current_gain) * coeff;
        // Snap to the target once within a hair of it. The one-pole smoother only
        // *approaches* its target, so without this the gain settles at e.g.
        // 0.9999998 instead of exactly 1.0 — meaning passthrough would never be
        // truly bit-exact after the dim/mute has been used once. Multiplying by
        // an exact 1.0f is identity, so this restores bit-perfect passthrough.
        if (std::fabs(self->current_gain - target_gain) < 1.0e-6f)
            self->current_gain = target_gain;

        float s = self->in[i] * self->current_gain;
        if (mix_ref) {
            self->ref_osc_gain += (ref_tgt - self->ref_osc_gain) * coeff;
            // Once a release has decayed to silence, snap to exactly 0 so the
            // next block takes the bit-exact passthrough path again.
            if (!ref_on && self->ref_osc_gain < 1.0e-5f) self->ref_osc_gain = 0.0f;
            s += static_cast<float>(std::sin(self->ref_phase)) * self->ref_osc_gain;
            self->ref_phase += ref_dphase;
            if (self->ref_phase >= TWO_PI) self->ref_phase -= TWO_PI;
        }
        if (self->ping_env > 0.0f) {
            const double p = self->ping_phase;
            s += PING_LEVEL * self->ping_env *
                 static_cast<float>(std::sin(p) + 0.22 * std::sin(2.0 * p));
            self->ping_phase += ping_dphase;
            if (self->ping_phase >= TWO_PI) self->ping_phase -= TWO_PI;
            self->ping_env *= ping_decay;
            if (self->ping_env < 1.0e-4f) self->ping_env = 0.0f;  // snap -> bit-exact
        }
        self->out[i] = s;
    }
    if (self->p_level) *self->p_level = std::min(self->level_env, 1.0f);

    if (self->p_note)    *self->p_note    = self->r_note.load();
    if (self->p_cents)   *self->p_cents   = self->r_cents.load();
    if (self->p_clarity) *self->p_clarity = self->r_clarity.load();
    if (self->p_freq)    *self->p_freq    = self->r_freq.load();

    const int   voiced_now = self->r_voiced.load();
    const float cents_now  = self->r_cents.load();
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

    const float    tol      = self->tolerance ? *self->tolerance : 2.0f;
    const float    hyst     = 1.5f;
    const uint32_t min_hold = static_cast<uint32_t>(IN_TUNE_HOLD_S   * self->sample_rate);
    const uint32_t enter_db = static_cast<uint32_t>(ENTER_DEBOUNCE_S * self->sample_rate);
    const uint32_t switch_db= static_cast<uint32_t>(SWITCH_DEBOUNCE_S* self->sample_rate);
    const uint32_t release  = static_cast<uint32_t>(RELEASE_HOLD_S   * self->sample_rate);

    const int zone_before = self->tune_zone;   // for the ping rising-edge test

    // Advance the in-tune hold clock whenever the lamp currently reads "in tune"
    // (wall-clock, so it releases even if the note has since gone silent).
    if (self->tune_zone == 1) self->in_tune_samps += n_samples;

    int raw;  // -1 none, 0 raise, 1 in tune, 2 lower
    if (voiced_now) {
        self->release_samps = 0;
        const float band = (self->tune_zone == 1) ? (tol + hyst) : tol;
        if (std::fabs(cents_now) <= band) raw = 1;
        else if (cents_now < 0.0f)        raw = 0;  // flat -> raise
        else                              raw = 2;  // sharp -> lower
    } else {
        self->release_samps += n_samples;
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

    if (self->p_raise)   *self->p_raise   = (self->tune_zone == 0) ? 1.0f : 0.0f;
    if (self->p_in_tune) *self->p_in_tune = (self->tune_zone == 1) ? 1.0f : 0.0f;
    if (self->p_lower)   *self->p_lower   = (self->tune_zone == 2) ? 1.0f : 0.0f;

    // Fire the confirmation ping (rendered from the next block) unless switched
    // off in Advanced Settings. It fires on the rising edge into "in tune" -- but
    // ONLY when the detected note is one of the selected tuning's open-string
    // pitches. A note that lands in tune yet isn't part of the tuning (e.g. a D2
    // in Standard, or any fretted note) must stay silent: the ping confirms a
    // tuned STRING, not just any in-tune pitch.
    const bool ping_on = self->ping_enable ? (*self->ping_enable > 0.5f) : false;
    bool fire_ping = false;
    if (ping_on && self->tune_zone == 1 && zone_before != 1) {
        // Rising edge into "in tune" -- check the note against the tuning targets
        // (cheap, and only on this rare edge, so no per-block cost).
        const int det = static_cast<int>(std::lround(self->r_note.load()));
        int tgt[8];
        const int nt = build_targets(self->tuning_idx.load(std::memory_order_relaxed), tgt);
        for (int i = 0; i < nt; ++i)
            if (tgt[i] == det) { fire_ping = true; break; }
    }
    if (fire_ping) {
        self->ping_env   = 1.0f;
        self->ping_phase = 0.0;
    }
}

void deactivate(LV2_Handle handle)
{
    Hawk* self = static_cast<Hawk*>(handle);
    self->running.store(false);
    if (self->worker.joinable()) {
        self->worker.join();
    }
}

void cleanup(LV2_Handle handle)
{
    Hawk* self = static_cast<Hawk*>(handle);
    if (self->running.load()) {
        self->running.store(false);
        if (self->worker.joinable()) {
            self->worker.join();
        }
    }
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
