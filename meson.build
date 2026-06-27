project('themonohawk', 'cpp',
  version : '0.1.0',
  license : 'GPL-3.0-or-later',
  default_options : [
    'cpp_std=c++17',
    'warning_level=3',
    'werror=false',
    'buildtype=release',                        # -O3: the DSP is real-time; -O0 xruns
  ])

# The Hawk — a precise, reliable guitar/bass tuner for Linux (LV2 first).
# Layer 1 builds and tests the format-agnostic core in isolation; the LV2 and
# CLAP wrappers come later and link against the same core.

subdir('src')
subdir('lv2')   # before tools/test: the LV2 selftest runs against hawk.so
subdir('tools')
subdir('test')
