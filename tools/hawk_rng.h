// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
// hawk_rng.h -- the test material's random numbers: the MT19937 generator (Matsumoto & Nishimura, 1998) and normal
// variates by the polar method, drawn exactly as the C++ standard library's mt19937 + normal_distribution<double>
// do on this toolchain (uniforms of 53 bits from two 32-bit draws; pairs, the second kept for the next call). The
// probes and tests seed it as they always seeded std::mt19937, so their signals are the same, sample for sample:
// test/test_rng.c checks that against reference values taken from libstdc++.
#pragma once

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct HawkMT {
    uint32_t mt[624];
    int      idx;
} HawkMT;

static inline void hawk_mt_seed(HawkMT* g, uint32_t seed)
{
    g->mt[0] = seed;
    for (int i = 1; i < 624; ++i)
        g->mt[i] = 1812433253u * (g->mt[i - 1] ^ (g->mt[i - 1] >> 30)) + (uint32_t)i;
    g->idx = 624;
}

static inline uint32_t hawk_mt_next(HawkMT* g)
{
    if (g->idx >= 624) {
        for (int i = 0; i < 624; ++i) {
            const uint32_t y = (g->mt[i] & 0x80000000u) | (g->mt[(i + 1) % 624] & 0x7fffffffu);
            g->mt[i] = g->mt[(i + 397) % 624] ^ (y >> 1) ^ ((y & 1u) ? 0x9908b0dfu : 0u);
        }
        g->idx = 0;
    }
    uint32_t y = g->mt[g->idx++];
    y ^= y >> 11;
    y ^= (y << 7) & 0x9d2c5680u;
    y ^= (y << 15) & 0xefc60000u;
    y ^= y >> 18;
    return y;
}

// A double in [0, 1) from two draws: (d1 + d2 * 2^32) / 2^64, as the standard library builds a 53-bit canonical
// value out of a 32-bit engine.
static inline double hawk_mt_canonical(HawkMT* g)
{
    double sum = 0.0, tmp = 1.0;
    sum += (double)hawk_mt_next(g) * tmp;  tmp *= 4294967296.0;
    sum += (double)hawk_mt_next(g) * tmp;  tmp *= 4294967296.0;
    const double r = sum / tmp;
    return (r >= 1.0) ? nextafter(1.0, 0.0) : r;
}

typedef struct HawkNormal {
    double mean, stddev;
    double saved;
    bool   saved_available;
} HawkNormal;

static inline HawkNormal hawk_normal(double mean, double stddev)
{
    HawkNormal n = { mean, stddev, 0.0, false };
    return n;
}

static inline double hawk_normal_draw(HawkNormal* n, HawkMT* g)
{
    double ret;
    if (n->saved_available) {
        n->saved_available = false;
        ret = n->saved;
    } else {
        double x, y, r2;
        do {
            x = 2.0 * hawk_mt_canonical(g) - 1.0;
            y = 2.0 * hawk_mt_canonical(g) - 1.0;
            r2 = x * x + y * y;
        } while (r2 > 1.0 || r2 == 0.0);
        const double mult = sqrt(-2 * log(r2) / r2);
        n->saved = x * mult;
        n->saved_available = true;
        ret = y * mult;
    }
    return ret * n->stddev + n->mean;
}
