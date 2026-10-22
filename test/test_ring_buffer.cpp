// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
// Ring buffer, and specifically the thing that matters: an overflow must be REPORTED, not
// swallowed. The detector's window has to be contiguous, so a dropped span is not "a bit less
// audio" -- it is a phase splice in the middle of the frame, which the correlation reads as a
// period that is not there. A silent drop turns a dropout into a wrong answer.
//
// build: g++ -std=c++17 -O2 -I src test/test_ring_buffer.cpp -o /tmp/t && /tmp/t

#include "ring_buffer.hpp"

#include <cstdio>
#include <vector>

namespace {

int checks_run = 0;
int checks_failed = 0;

void check(bool ok, const char* what, const char* detail = "")
{
    ++checks_run;
    if (!ok) ++checks_failed;
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what,
                detail[0] ? " -- " : "", detail);
}

} // namespace

int main()
{
    std::printf("== ring buffer ==\n");
    const std::size_t CAP = 1024;

    // ---- the ordinary case: everything fits ------------------------------
    {
        hawk::RingBuffer r(CAP);
        std::vector<float> in(100, 1.0f);
        const std::size_t n = r.push(in.data(), in.size());
        check(n == in.size(), "a push that fits reports the full count");
    }

    // ---- the case that matters: it does NOT fit -----------------------
    {
        hawk::RingBuffer r(CAP);
        std::vector<float> in(CAP + 500, 1.0f);
        const std::size_t n = r.push(in.data(), in.size());
        char d[128];
        std::snprintf(d, sizeof d, "asked for %zu into a %zu ring, took %zu",
                      in.size(), CAP, n);
        check(n < in.size(), "an overflowing push reports SHORT", d);
        check(n == CAP, "and it reports exactly what it took", d);
    }

    // ---- filling it in stages, until it refuses ---------------------------
    {
        hawk::RingBuffer r(CAP);
        std::vector<float> in(256, 1.0f);
        std::size_t total = 0;
        bool short_seen = false;
        for (int k = 0; k < 8; ++k) {
            const std::size_t n = r.push(in.data(), in.size());
            total += n;
            if (n < in.size()) short_seen = true;
        }
        char d[128];
        std::snprintf(d, sizeof d, "8 pushes of 256 into a %zu ring accepted %zu", CAP, total);
        check(short_seen, "a caller that overruns the ring finds out", d);
        check(total == CAP, "and never more than the capacity goes in", d);
    }

    // ---- what the consumer gets back is what went in ----------------------
    {
        hawk::RingBuffer r(CAP);
        std::vector<float> in(300);
        for (std::size_t i = 0; i < in.size(); ++i) in[i] = static_cast<float>(i);
        r.push(in.data(), in.size());
        std::vector<float> out(300, -1.0f);
        const std::size_t got = r.pop(out.data(), out.size());
        bool same = (got == in.size());
        for (std::size_t i = 0; same && i < got; ++i) same = (out[i] == in[i]);
        check(same, "what comes out is what went in, in order");
    }

    // ---- GapTracker: WHERE the stream was cut, when the consumer is behind --------------------
    //
    // Why two gaps: under ASan the worker is slow, the ring
    // overflowed TWICE before the worker reached the first seam, and the single gap_pos slot had
    // been overwritten by the second -- so the worker crossed the first splice without resetting
    // and published 110 Hz, a pitch nobody played (the same gap probe passes on a fast worker).
    // These cases are deterministic: positions are given, no threads.
    {
        // a stand-in for the OLD logic (one slot, overwritten), to show the scenario bites it
        struct OneSlot {
            std::size_t pos = 0; bool pending = false;
            void mark(std::size_t w) { pos = w; pending = true; }
            bool reset_on(std::size_t r1) { if (pending && r1 > pos) { pending = false; return true; } return false; }
        } old;
        old.mark(1000); old.mark(1100);          // two overflows before the consumer gets there
        const bool old_saw_first = old.reset_on(1050);   // a pop that crosses the FIRST seam only
        check(!old_saw_first, "control: the one-slot logic misses the first of two seams",
              "if this fails, the scenario below no longer tests anything");

        hawk::GapTracker g;
        g.mark(1000); g.mark(1100);
        hawk::GapTracker::Verdict v = g.consume(900, 1050);    // crosses 1000, not 1100
        check(v.reset, "a pop crossing the FIRST of two seams restarts the window");
        check(v.skip == 150, "and keeps none of it: a second seam lies ahead, position unknown");
        v = g.consume(1050, 1100);                              // ends exactly ON the second seam
        check(v.reset && v.skip == 50, "a pop ending on the last seam is still inside the dirty span");
        v = g.consume(1100, 1300);                              // starts at the second seam
        check(v.reset && v.skip == 0, "the pop that starts at the last seam restarts and keeps all of it");
        v = g.consume(1300, 1500);
        check(!v.reset && v.skip == 0, "past the last seam the gap is handled: no further restarts");
    }
    {
        hawk::GapTracker g;
        g.mark(5000);
        hawk::GapTracker::Verdict v = g.consume(4000, 5000);   // ends exactly where the audio ended
        check(!v.reset && v.skip == 0, "a pop that ends ON the seam is clean and kept");
        v = g.consume(5000, 6000);
        check(v.reset && v.skip == 0, "the next one starts after the seam: restart, keep everything");
        g.mark(9000);
        v = g.consume(8000, 10000);                             // one seam in the middle of a pop
        check(v.reset && v.skip == 1000, "a seam inside a pop: restart and drop what came before it");
        v = g.consume(10000, 11000);
        check(!v.reset, "and a single seam is handled by that one pop");
    }
    {
        hawk::GapTracker g;                                    // nothing ever lost
        const hawk::GapTracker::Verdict v = g.consume(0, 2048);
        check(!v.reset && v.skip == 0, "no overflow, no restart");
    }

    std::printf("\n%d checks, %d failed\n", checks_run, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
