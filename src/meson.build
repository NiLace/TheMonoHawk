# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) The NL Sounds contributors
# The format-agnostic core. Knows nothing about LV2, CLAP, Pugl or Cairo.
# The plugin wrappers (added in later layers) link against this same library.

# A C module needs libm itself: the C++ build got it through libstdc++, a C .so does not, and a host that has not
# loaded libm yet would fail at dlopen.
m_dep = meson.get_compiler('c').find_library('m', required : false)

hawk_core_sources = [
  'note_mapping.c',
  'pitch_detector.c',
  'fine_tuner.c',
]

hawk_core_lib = static_library('hawk_core',
  hawk_core_sources,
  dependencies : m_dep,
  pic          : true)

hawk_core_dep = declare_dependency(
  link_with          : hawk_core_lib,
  dependencies       : m_dep,
  include_directories : include_directories('.'))

