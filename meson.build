# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) The NL Sounds contributors
project('themonohawk', 'cpp',
  version : '0.2.0',
  license : 'AGPL-3.0-only',
  default_options : [
    'cpp_std=c++17',
    'warning_level=3',
    'werror=false',
    'buildtype=release',                        # -O3: the DSP is real-time; -O0 xruns
    # NDEBUG in the release build, which also drops meson's _GLIBCXX_ASSERTIONS: a violated library
    # assertion aborts the process, and in a plugin the process is the host. The checks stay in every
    # other build type -- the ASan/UBSan test build is debugoptimized -- where a bug should stop the run.
    'b_ndebug=if-release',
  ])

# The Hawk — a precise, reliable guitar/bass tuner for Linux (LV2).
# src/ builds and tests the format-agnostic core in isolation; the LV2 wrapper and GUI (lv2/) link against it.

subdir('src')
subdir('lv2')   # before tools/test: the LV2 selftest runs against hawk.so
subdir('tools')
subdir('test')
