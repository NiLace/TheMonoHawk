# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) The NL Sounds contributors
project('themonohawk', 'c',
  version : '0.2.2',
  license : 'AGPL-3.0-only',
  default_options : [
    # gnu11, not c11: ISO C mode turns floating-point contraction off by default, gnu11 keeps the compiler's
    # default, and the arithmetic of every reading was verified with it.
    'c_std=gnu11',
    'warning_level=3',
    'werror=false',
    'buildtype=release',                        # -O3: the DSP is real-time; -O0 xruns
    # NDEBUG in the release build: a violated assertion aborts the process, and in a plugin the process is
    # the host. The checks stay in every
    # other build type -- the ASan/UBSan test build is debugoptimized -- where a bug should stop the run.
    'b_ndebug=if-release',
  ])

# The Hawk — a precise, reliable guitar/bass tuner for Linux (LV2).
# src/ builds and tests the format-agnostic core in isolation; the LV2 wrapper and GUI (lv2/) link against it.

subdir('src')
subdir('lv2')   # before tools/test: the LV2 selftest runs against hawk.so
subdir('tools')
subdir('test')
