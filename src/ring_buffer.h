/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) The NL Sounds contributors */
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

// Single-producer / single-consumer lock-free float ring buffer.
//
// The audio thread (producer) pushes samples; the detection worker (consumer)
// pops them. Neither blocks nor allocates after construction, so the audio
// thread stays real-time safe. Capacity must be a power of two.
typedef struct RingBuffer {
    float*                buffer_;
    size_t                mask_;
    _Atomic size_t        write_;
    _Atomic size_t        read_;
} RingBuffer;

// capacity_pow2 samples, zeroed; false if the allocation failed
static inline bool ring_init(RingBuffer* r, size_t capacity_pow2)
{
    r->buffer_ = (float*)calloc(capacity_pow2, sizeof(float));
    r->mask_   = capacity_pow2 - 1;
    atomic_init(&r->write_, 0);
    atomic_init(&r->read_, 0);
    return r->buffer_ != NULL;
}
static inline void ring_free(RingBuffer* r) { free(r->buffer_); r->buffer_ = NULL; }

static inline size_t ring_capacity(const RingBuffer* r) { return r->mask_ + 1; }

// Producer side. Writes what fits and RETURNS HOW MANY, so the caller can tell that samples
// were lost.
//
// ⚠ The old comment here said the detector "simply works from whatever it receives", and that
// is the one thing it cannot do: the analysis window has to be CONTIGUOUS. A gap splices two
// pieces of audio that never touched, which is a phase discontinuity in the middle of the
// frame -- the correlation reads it as a period that is not there. Dropping silently turns a
// dropout into a wrong ANSWER instead of a missing one.
//
// In practice the ring holds 1.365 s at 48 kHz and the worker drains it in 2048-sample bites,
// so overflowing needs the worker not to run for over a second. Rare, not impossible -- and
// the cost of noticing is one comparison.
static inline size_t ring_push(RingBuffer* r, const float* data, size_t count)
{
    const size_t w     = atomic_load_explicit(&r->write_, memory_order_relaxed);
    const size_t rd    = atomic_load_explicit(&r->read_, memory_order_acquire);
    const size_t space = ring_capacity(r) - (w - rd);
    const size_t n     = (count < space) ? count : space;
    for (size_t i = 0; i < n; ++i) {
        r->buffer_[(w + i) & r->mask_] = data[i];
    }
    atomic_store_explicit(&r->write_, w + n, memory_order_release);
    return n;
}

// Drops everything currently buffered. NOT lock-free-safe: it may only be called while neither
// side is running, which in this plugin means from activate() -- deactivate() has already
// joined the worker by then. Exists so a re-activated plugin does not start by analysing audio
// from before the host stopped it.
static inline void ring_clear(RingBuffer* r)
{
    atomic_store_explicit(&r->read_, 0, memory_order_relaxed);
    atomic_store_explicit(&r->write_, 0, memory_order_relaxed);
}

// Consumer side. Reads up to `count` samples into `out`; returns how many
// were actually available and copied.
static inline size_t ring_pop(RingBuffer* r, float* out, size_t count)
{
    const size_t rd    = atomic_load_explicit(&r->read_, memory_order_relaxed);
    const size_t w     = atomic_load_explicit(&r->write_, memory_order_acquire);
    const size_t avail = w - rd;
    const size_t n     = (count < avail) ? count : avail;
    for (size_t i = 0; i < n; ++i) {
        out[i] = r->buffer_[(rd + i) & r->mask_];
    }
    atomic_store_explicit(&r->read_, rd + n, memory_order_release);
    return n;
}

// Monotonic stream positions -- they count every sample ever pushed / popped and are never
// wrapped, so they name a point in the STREAM rather than a slot in the buffer. The gap tracker
// needs that: knowing a drop happened is useless without knowing WHERE, because the consumer is up
// to a ring-full behind the producer and the audio it is holding when it hears the news is
// still perfectly contiguous. The discontinuity is at ring_written() as of the failed push.
static inline size_t ring_written(RingBuffer* r)  { return atomic_load_explicit(&r->write_, memory_order_acquire); }
static inline size_t ring_read_pos(RingBuffer* r) { return atomic_load_explicit(&r->read_, memory_order_acquire); }

// Where the stream was cut, for a consumer that may be far behind the producer.
//
// A seam at stream position W means samples < W are from before the loss and samples >= W from
// after it. The producer marks one at ring_written() after every short push; the consumer asks, for
// each pop covering [r0, r1), whether to restart its window and how many leading samples of the
// pop to drop.
//
// ⚠ ONE slot is not enough: if the ring
// overflows twice before the consumer reaches the first seam, a single overwritten position names
// only the second, and the consumer walks across the first splice without noticing. So this keeps
// the FIRST and the LAST unhandled seam. Between them there may be others whose positions are
// lost, so everything from the first to the last is dropped; only the audio after the last seam
// is trusted. A wrong guess here costs a missing reading for a moment, never a wrong one.
//
// Lock-free, single producer / single consumer, no allocation. The producer starts a new burst
// (rewrites `first`) only once the consumer has acknowledged the previous one; if the two race,
// the consumer sees a stale `first` it has already passed and restarts once more than needed.
typedef struct GapVerdict {
    bool   reset;   // the window built so far straddles a seam: start it over
    size_t skip;    // leading samples of this pop that are from before the seam
} GapVerdict;

typedef struct GapTracker {
    _Atomic uint32_t seq_;     // producer: bursts marked
    _Atomic uint32_t ack_;     // consumer: last one handled
    uint32_t         seen_;    // consumer-private copy of ack_
    _Atomic size_t   first_;
    _Atomic size_t   last_;
} GapTracker;

// Only while neither side runs (activate()), and once before first use.
static inline void gaps_clear(GapTracker* g)
{
    atomic_store_explicit(&g->seq_, 0, memory_order_relaxed);
    atomic_store_explicit(&g->ack_, 0, memory_order_relaxed);
    g->seen_ = 0;
    atomic_store_explicit(&g->first_, 0, memory_order_relaxed);
    atomic_store_explicit(&g->last_, 0, memory_order_relaxed);
}

// Producer side: the stream is cut at `pos` (the producer's ring_written() after the short push).
static inline void gaps_mark(GapTracker* g, size_t pos)
{
    const uint32_t s = atomic_load_explicit(&g->seq_, memory_order_relaxed);
    if (s == atomic_load_explicit(&g->ack_, memory_order_acquire))   // previous burst handled: a new one starts
        atomic_store_explicit(&g->first_, pos, memory_order_relaxed);
    atomic_store_explicit(&g->last_, pos, memory_order_relaxed);
    atomic_store_explicit(&g->seq_, s + 1, memory_order_release);
}

// Consumer side, once per pop that covered [r0, r1).
static inline GapVerdict gaps_consume(GapTracker* g, size_t r0, size_t r1)
{
    GapVerdict v = { false, 0 };
    const uint32_t s = atomic_load_explicit(&g->seq_, memory_order_acquire);
    if (s == g->seen_) return v;                       // nothing unhandled
    const size_t first = atomic_load_explicit(&g->first_, memory_order_relaxed);
    const size_t last  = atomic_load_explicit(&g->last_, memory_order_relaxed);
    if (r1 <= first) return v;                         // not there yet: what we hold is clean
    v.reset = true;
    if (r1 > last) {                                   // past the last seam
        v.skip = (last > r0) ? last - r0 : 0;
        g->seen_ = s;
        atomic_store_explicit(&g->ack_, s, memory_order_release);
    } else {
        v.skip = r1 - r0;                              // inside the dirty span: keep nothing
    }
    return v;
}
