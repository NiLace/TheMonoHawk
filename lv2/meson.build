# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) The NL Sounds contributors
# The LV2 wrapper (hawk.so) and its GUI (hawk_ui.so). scripts/install.sh assembles the bundle from these, the .ttl
# files, gui/assets and gui/fonts, into ~/.lv2.

lv2_dep    = dependency('lv2')
thread_dep = dependency('threads')

hawk_lv2 = shared_module('hawk',
  'hawk_lv2.c',
  dependencies : [hawk_core_dep, lv2_dep, thread_dep, m_dep],
  name_prefix  : '',
  c_args       : ['-fvisibility=hidden'],
  install      : false)

# The GUI: a self-contained X11 + Cairo LV2 UI (no windowing framework).
cairo_dep = dependency('cairo')
x11_dep   = dependency('x11')
ft_dep    = dependency('freetype2')   # the panel's type (Oswald) loaded from the bundle: hawk_ui_fonts.h

# libm by name: a C module does not get it through libstdc++, and a host that has not loaded it would fail at dlopen.
# The module unloads (tools/ui_unload_probe.c): C makes no STB_GNU_UNIQUE symbols, and the fonts are made safe on
# purpose (hawk_ui_fonts.h).
m_dep = meson.get_compiler('c').find_library('m', required : false)
ui_deps = [lv2_dep, cairo_dep, x11_dep, ft_dep, m_dep, thread_dep]
hawk_ui = shared_module('hawk_ui',
  'hawk_ui.c',
  dependencies        : ui_deps,
  include_directories : include_directories('../src'),  # shared tunings.h (header-only)
  name_prefix         : '',
  c_args              : ['-fvisibility=hidden'],
  install             : false)
