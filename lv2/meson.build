# The LV2 wrapper. Builds hawk.so; the bundle is assembled from this plus the
# .ttl files (see scripts in the repo / README for installing to ~/.lv2).

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

hawk_ui = shared_module('hawk_ui',
  'hawk_ui.cpp',
  dependencies        : [lv2_dep, cairo_dep, x11_dep],
  include_directories : include_directories('../src'),  # shared tunings.hpp (header-only)
  name_prefix         : '',
  cpp_args            : ['-fvisibility=hidden'],
  install             : false)
