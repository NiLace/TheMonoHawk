# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) The NL Sounds contributors
# The LV2 wrapper (hawk.so) and its GUI (hawk_ui.so). scripts/install.sh assembles the bundle from these, the .ttl
# files, gui/assets and gui/fonts, into ~/.lv2.

lv2_dep    = dependency('lv2')
thread_dep = dependency('threads')

hawk_lv2 = shared_module('hawk',
  'hawk_lv2.cpp',
  dependencies : [hawk_core_dep, lv2_dep, thread_dep],
  name_prefix  : '',
  cpp_args     : ['-fvisibility=hidden'],
  install      : false)

# The GUI: a self-contained X11 + Cairo LV2 UI (no windowing framework).
cairo_dep = dependency('cairo')
x11_dep   = dependency('x11')
ft_dep    = dependency('freetype2')   # the panel's type (Oswald) loaded from the bundle: hawk_ui_fonts.h

# -fno-gnu-unique: without it the headers' inline/static locals become STB_GNU_UNIQUE symbols and the loader never
# unloads the module -- safe by accident; the fonts are made safe on purpose (hawk_ui_fonts.h).
hawk_ui = shared_module('hawk_ui',
  'hawk_ui.cpp',
  dependencies        : [lv2_dep, cairo_dep, x11_dep, ft_dep],
  include_directories : include_directories('../src'),  # shared tunings.hpp (header-only)
  name_prefix         : '',
  cpp_args            : ['-fvisibility=hidden', '-fvisibility-inlines-hidden', '-fno-gnu-unique'],
  install             : false)
