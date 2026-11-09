// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) The NL Sounds contributors
// test_rng.c -- tools/hawk_rng.h draws the same numbers as the C++ standard library's std::mt19937 and
// std::normal_distribution<double> (libstdc++): the probes' signals depend on it. Reference values taken from
// libstdc++: the 10000th output of mt19937 with its default seed (5489, the value the standard itself names), and,
// from seed 5489, the sum of the first 100000 normal(0, 1) draws and the engine's next output after them.
#include "../tools/hawk_rng.h"

#include <stdio.h>

int main(void)
{
    int fails = 0;
    HawkMT g;
    hawk_mt_seed(&g, 5489u);
    uint32_t v = 0;
    for (int i = 0; i < 10000; ++i) v = hawk_mt_next(&g);
    printf("mt19937 10000th output: %u (expected 4123659995)\n", v);
    fails += (v != 4123659995u);

    hawk_mt_seed(&g, 5489u);
    HawkNormal n = hawk_normal(0.0, 1.0);
    double s = 0.0;
    for (int i = 0; i < 100000; ++i) s += hawk_normal_draw(&n, &g);
    const uint32_t next = hawk_mt_next(&g);
    printf("sum of 100000 normal draws: %.17g (expected -87.335677675933624), next output %u (expected 1564927710)\n",
           s, next);
    fails += (s != -87.335677675933624) + (next != 1564927710u);

    // negative control: a different seed must give a different sum
    hawk_mt_seed(&g, 5490u);
    n = hawk_normal(0.0, 1.0);
    double s2 = 0.0;
    for (int i = 0; i < 100000; ++i) s2 += hawk_normal_draw(&n, &g);
    fails += (s2 == s);

    printf(fails ? "FAIL\n" : "PASS\n");
    return fails != 0;
}
