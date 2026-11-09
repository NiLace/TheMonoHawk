// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
// Ring buffer, and specifically the thing that matters: an overflow must be REPORTED, not
// swallowed. The detector's window has to be contiguous, so a dropped span is not "a bit less
// audio" -- it is a phase splice in the middle of the frame, which the correlation reads as a
// period that is not there. A silent drop turns a dropout into a wrong answer.
//
// build: gcc -std=gnu11 -O2 -I src test/test_ring_buffer.c -o /tmp/t && /tmp/t

#include "ring_buffer.h"

#include <stdbool.h>
#include <stdio.h>

static int checks_run = 0;
static int checks_failed = 0;

static void check(bool ok, const char* what, const char* detail)
{
    ++checks_run;
    if (!ok) ++checks_failed;
    printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what,
           detail[0] ? " -- " : "", detail);
}

// a stand-in for the OLD logic (one slot, overwritten), to show the scenario bites it
typedef struct OneSlot { size_t pos; bool pending; } OneSlot;
static void one_slot_mark(OneSlot* o, size_t w) { o->pos = w; o->pending = true; }
static bool one_slot_reset_on(OneSlot* o, size_t r1)
{
    if (o->pending && r1 > o->pos) { o->pending = false; return true; }
    return false;
}

static void fill(float* a, size_t n, float v) { for (size_t i = 0; i < n; ++i) a[i] = v; }

int main(void)
{
    printf("== ring buffer ==\n");
    enum { CAP = 1024 };

    // ---- the ordinary case: everything fits ------------------------------
    {
        RingBuffer r;
        ring_init(&r, CAP);
        float in[100];
        fill(in, 100, 1.0f);
        const size_t n = ring_push(&r, in, 100);
        check(n == 100, "a push that fits reports the full count", "");
        ring_free(&r);
    }

    // ---- the case that matters: it does NOT fit -----------------------
    {
        RingBuffer r;
        ring_init(&r, CAP);
        static float in[CAP + 500];
        const size_t in_size = CAP + 500;
        fill(in, in_size, 1.0f);
        const size_t n = ring_push(&r, in, in_size);
        char d[128];
        snprintf(d, sizeof d, "asked for %zu into a %zu ring, took %zu",
                 in_size, (size_t)CAP, n);
        check(n < in_size, "an overflowing push reports SHORT", d);
        check(n == CAP, "and it reports exactly what it took", d);
        ring_free(&r);
    }

    // ---- filling it in stages, until it refuses ---------------------------
    {
        RingBuffer r;
        ring_init(&r, CAP);
        float in[256];
        fill(in, 256, 1.0f);
        size_t total = 0;
        bool short_seen = false;
        for (int k = 0; k < 8; ++k) {
            const size_t n = ring_push(&r, in, 256);
            total += n;
            if (n < 256) short_seen = true;
        }
        char d[128];
        snprintf(d, sizeof d, "8 pushes of 256 into a %zu ring accepted %zu", (size_t)CAP, total);
        check(short_seen, "a caller that overruns the ring finds out", d);
        check(total == CAP, "and never more than the capacity goes in", d);
        ring_free(&r);
    }

    // ---- what the consumer gets back is what went in ----------------------
    {
        RingBuffer r;
        ring_init(&r, CAP);
        float in[300];
        for (size_t i = 0; i < 300; ++i) in[i] = (float)i;
        ring_push(&r, in, 300);
        float out[300];
        fill(out, 300, -1.0f);
        const size_t got = ring_pop(&r, out, 300);
        bool same = (got == 300);
        for (size_t i = 0; same && i < got; ++i) same = (out[i] == in[i]);
        check(same, "what comes out is what went in, in order", "");
        ring_free(&r);
    }

    // ---- GapTracker: WHERE the stream was cut, when the consumer is behind --------------------
    //
    // Why two gaps: under ASan the worker is slow, the ring
    // overflowed TWICE before the worker reached the first seam, and the single gap_pos slot had
    // been overwritten by the second -- so the worker crossed the first splice without resetting
    // and published 110 Hz, a pitch nobody played (the same gap probe passes on a fast worker).
    // These cases are deterministic: positions are given, no threads.
    {
        OneSlot old = { 0, false };
        one_slot_mark(&old, 1000); one_slot_mark(&old, 1100);   // two overflows before the consumer gets there
        const bool old_saw_first = one_slot_reset_on(&old, 1050);   // a pop that crosses the FIRST seam only
        check(!old_saw_first, "control: the one-slot logic misses the first of two seams",
              "if this fails, the scenario below no longer tests anything");

        GapTracker g;
        gaps_clear(&g);
        gaps_mark(&g, 1000); gaps_mark(&g, 1100);
        GapVerdict v = gaps_consume(&g, 900, 1050);             // crosses 1000, not 1100
        check(v.reset, "a pop crossing the FIRST of two seams restarts the window", "");
        check(v.skip == 150, "and keeps none of it: a second seam lies ahead, position unknown", "");
        v = gaps_consume(&g, 1050, 1100);                       // ends exactly ON the second seam
        check(v.reset && v.skip == 50, "a pop ending on the last seam is still inside the dirty span", "");
        v = gaps_consume(&g, 1100, 1300);                       // starts at the second seam
        check(v.reset && v.skip == 0, "the pop that starts at the last seam restarts and keeps all of it", "");
        v = gaps_consume(&g, 1300, 1500);
        check(!v.reset && v.skip == 0, "past the last seam the gap is handled: no further restarts", "");
    }
    {
        GapTracker g;
        gaps_clear(&g);
        gaps_mark(&g, 5000);
        GapVerdict v = gaps_consume(&g, 4000, 5000);            // ends exactly where the audio ended
        check(!v.reset && v.skip == 0, "a pop that ends ON the seam is clean and kept", "");
        v = gaps_consume(&g, 5000, 6000);
        check(v.reset && v.skip == 0, "the next one starts after the seam: restart, keep everything", "");
        gaps_mark(&g, 9000);
        v = gaps_consume(&g, 8000, 10000);                      // one seam in the middle of a pop
        check(v.reset && v.skip == 1000, "a seam inside a pop: restart and drop what came before it", "");
        v = gaps_consume(&g, 10000, 11000);
        check(!v.reset, "and a single seam is handled by that one pop", "");
    }
    {
        GapTracker g;                                           // nothing ever lost
        gaps_clear(&g);
        const GapVerdict v = gaps_consume(&g, 0, 2048);
        check(!v.reset && v.skip == 0, "no overflow, no restart", "");
    }

    printf("\n%d checks, %d failed\n", checks_run, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
