# Command-line tuner: the detection core wired to a terminal. Not the plugin;
# a way to point an instrument at the core today.

hawk_cli = executable('hawk_cli',
  'hawk_cli.cpp',
  dependencies : hawk_core_dep,
  install      : false)

# Headless host test for the LV2 plugin: dlopens hawk.so and exercises the whole
# plugin path. Registered as a test in test/meson.build. Needs the lv2 headers
# (for lv2_descriptor) and libdl (dlopen); on modern glibc dl is folded into
# libc, so it is optional here.
dl_dep = meson.get_compiler('cpp').find_library('dl', required : false)

hawk_lv2_selftest = executable('hawk_lv2_selftest',
  'hawk_lv2_selftest.cpp',
  dependencies : [lv2_dep, dl_dep],
  install      : false)
