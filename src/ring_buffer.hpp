/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) The NL Sounds contributors */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hawk {

// Single-producer / single-consumer lock-free float ring buffer.
//
// The audio thread (producer) pushes samples; the detection worker (consumer)
// pops them. Neither blocks nor allocates after construction, so the audio
// thread stays real-time safe. Capacity must be a power of two.
class RingBuffer {
public:
    explicit RingBuffer(std::size_t capacity_pow2)
        : buffer_(capacity_pow2, 0.0f)
        , mask_(capacity_pow2 - 1)
    {
    }

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
    std::size_t push(const float* data, std::size_t count)
    {
        const std::size_t w     = write_.load(std::memory_order_relaxed);
        const std::size_t r     = read_.load(std::memory_order_acquire);
        const std::size_t space = capacity() - (w - r);
        const std::size_t n     = (count < space) ? count : space;
        for (std::size_t i = 0; i < n; ++i) {
            buffer_[(w + i) & mask_] = data[i];
        }
        write_.store(w + n, std::memory_order_release);
        return n;
    }

    // Drops everything currently buffered. NOT lock-free-safe: it may only be called while neither
    // side is running, which in this plugin means from activate() -- deactivate() has already
    // joined the worker by then. Exists so a re-activated plugin does not start by analysing audio
    // from before the host stopped it.
    void clear()
    {
        read_.store(0, std::memory_order_relaxed);
        write_.store(0, std::memory_order_relaxed);
    }

    // Consumer side. Reads up to `count` samples into `out`; returns how many
    // were actually available and copied.
    std::size_t pop(float* out, std::size_t count)
    {
        const std::size_t r     = read_.load(std::memory_order_relaxed);
        const std::size_t w     = write_.load(std::memory_order_acquire);
        const std::size_t avail = w - r;
        const std::size_t n     = (count < avail) ? count : avail;
        for (std::size_t i = 0; i < n; ++i) {
            out[i] = buffer_[(r + i) & mask_];
        }
        read_.store(r + n, std::memory_order_release);
        return n;
    }

    std::size_t capacity() const { return mask_ + 1; }

    // Monotonic stream positions -- they count every sample ever pushed / popped and are never
    // wrapped, so they name a point in the STREAM rather than a slot in the buffer. The gap tracker
    // needs that: knowing a drop happened is useless without knowing WHERE, because the consumer is up
    // to a ring-full behind the producer and the audio it is holding when it hears the news is
    // still perfectly contiguous. The discontinuity is at written() as of the failed push.
    std::size_t written()  const { return write_.load(std::memory_order_acquire); }
    std::size_t read_pos() const { return read_.load(std::memory_order_acquire); }

private:
    std::vector<float>       buffer_;
    std::size_t              mask_;
    std::atomic<std::size_t> write_{0};
    std::atomic<std::size_t> read_{0};
};

// Where the stream was cut, for a consumer that may be far behind the producer.
//
// A seam at stream position W means samples < W are from before the loss and samples >= W from
// after it. The producer marks one at written() after every short push; the consumer asks, for
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
class GapTracker {
public:
    struct Verdict {
        bool        reset = false;   // the window built so far straddles a seam: start it over
        std::size_t skip  = 0;       // leading samples of this pop that are from before the seam
    };

    // Producer side: the stream is cut at `pos` (the producer's written() after the short push).
    void mark(std::size_t pos)
    {
        const uint32_t s = seq_.load(std::memory_order_relaxed);
        if (s == ack_.load(std::memory_order_acquire))   // previous burst handled: a new one starts
            first_.store(pos, std::memory_order_relaxed);
        last_.store(pos, std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_release);
    }

    // Consumer side, once per pop that covered [r0, r1).
    Verdict consume(std::size_t r0, std::size_t r1)
    {
        Verdict v;
        const uint32_t s = seq_.load(std::memory_order_acquire);
        if (s == seen_) return v;                          // nothing unhandled
        const std::size_t first = first_.load(std::memory_order_relaxed);
        const std::size_t last  = last_.load(std::memory_order_relaxed);
        if (r1 <= first) return v;                         // not there yet: what we hold is clean
        v.reset = true;
        if (r1 > last) {                                   // past the last seam
            v.skip = (last > r0) ? last - r0 : 0;
            seen_ = s;
            ack_.store(s, std::memory_order_release);
        } else {
            v.skip = r1 - r0;                              // inside the dirty span: keep nothing
        }
        return v;
    }

    // Only while neither side runs (activate()).
    void clear()
    {
        seq_.store(0, std::memory_order_relaxed);
        ack_.store(0, std::memory_order_relaxed);
        seen_ = 0;
        first_.store(0, std::memory_order_relaxed);
        last_.store(0, std::memory_order_relaxed);
    }

private:
    std::atomic<uint32_t>    seq_{0};     // producer: bursts marked
    std::atomic<uint32_t>    ack_{0};     // consumer: last one handled
    uint32_t                 seen_ = 0;   // consumer-private copy of ack_
    std::atomic<std::size_t> first_{0};
    std::atomic<std::size_t> last_{0};
};

} // namespace hawk
