# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) The NL Sounds contributors
# The lab and gate tools. First the command-line tuner: the detection core wired to a terminal, not the plugin.

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

# Force a ring overflow against the real .so and check that nothing is published
# from the window that straddles the gap. Registered as a test in test/meson.build.
hawk_gap_probe = executable('hawk_gap_probe',
  'gap_probe.cpp',
  dependencies : [dl_dep],
  install      : false)

# Hammer the output ports while the note changes and check that note, cents and
# freq always come from the SAME analysis. Registered as a test in test/meson.build.
hawk_tear_probe = executable('hawk_tear_probe',
  'tear_probe.cpp',
  dependencies : [dl_dep],
  install      : false)

# Zero allocations for the WHOLE plugin, worker thread included: interpose operator new and count
# every allocation the loaded .so makes while it reads a strum. Needs -rdynamic so the plugin's calls
# resolve to the probe's operator new. Registered as a test in test/meson.build.
hawk_alloc_lv2_probe = executable('hawk_alloc_lv2_probe',
  'alloc_lv2_probe.cpp',
  dependencies : [dl_dep, dependency('threads')],
  link_args    : ['-rdynamic'],
  install      : false)

# Cost of the loaded plugin in % of one core, worker thread included (schedstat). A measuring tool,
# not a test: compare two builds paired and alternating in the same batch.
hawk_cpu_lv2_probe = executable('hawk_cpu_lv2_probe',
  'cpu_lv2_probe.cpp',
  dependencies : [dl_dep, dependency('threads')],
  install      : false)

# Plucks with a sharp attack and a click, re-plucked while ringing; the reading
# must not move more than 3 c nor blink the in-tune lamp. Registered as tests in test/meson.build.
hawk_attack_probe = executable('hawk_attack_probe',
  'attack_probe.cpp',
  dependencies : [dl_dep, dependency('threads')],
  install      : false)

# No block may publish a WRONG note with confidence under 9 Hz rumble (D3, where
# the coarse stage picked another note 79 times in 12 s before the fine stage's coherence veto).
# And noise alone must never become a note, at any Sensitivity. Registered in test/meson.build.
hawk_wrongnote_probe = executable('hawk_wrongnote_probe',
  'wrongnote_probe.cpp',
  dependencies : [dl_dep, dependency('threads')],
  install      : false)

hawk_noise_probe = executable('hawk_noise_probe',
  'noise_probe.cpp',
  dependencies : [dl_dep, dependency('threads')],
  install      : false)

# What a misbehaving host, UI or signal chain can do: non-finite input and controls, activate()
# twice, an absurd sample rate. Registered as tests in test/meson.build, one per case.
hawk_hostile_probe = executable('hawk_hostile_probe',
  'hostile_probe.cpp',
  dependencies : [dl_dep, dependency('threads')],
  install      : false)

# A note stopped dead must not move the reading on its way out. Registered in test/meson.build.
hawk_cutoff_probe = executable('hawk_cutoff_probe',
  'cutoff_probe.cpp',
  dependencies : [dl_dep, dependency('threads')],
  install      : false)

# The chromatic reading at the host's rate (the analysis decimates above 50 kHz). Registered in
# test/meson.build, one test per rate.
hawk_rate_probe = executable('hawk_rate_probe',
  'rate_probe.cpp',
  dependencies : [dl_dep, dependency('threads')],
  install      : false)

# The strobe phase output turns at 2*pi*(f - f_ref) rad/s. Registered in test/meson.build.
hawk_strobe_probe = executable('hawk_strobe_probe',
  'strobe_probe.cpp',
  dependencies : [dl_dep, dependency('threads')],
  install      : false)

# Lab instruments: built on every build so they cannot rot unseen, run by hand. Each documents itself in
# its header. (hawk_diag goes with the Mono Hawk too; the rest stay in The Hawk.)
hawk_diag = executable('hawk_diag', 'hawk_diag.cpp', dependencies : hawk_core_dep, install : false)

# The GUI drawn without a display (lab instrument; see its header).
hawk_ui_snapshot = executable('hawk_ui_snapshot', 'ui_snapshot.cpp',
  include_directories : include_directories('../src'),
  dependencies : [lv2_dep, cairo_dep, x11_dep, ft_dep], install : false)

# The plugin's own bypass (lv2:enabled, the hawk in the GUI). Registered in test/meson.build.
hawk_bypass_probe = executable('hawk_bypass_probe',
  'bypass_probe.cpp',
  dependencies : [dl_dep, dependency('threads')],
  install      : false)

# The GUI's unload gate (tools/ui_unload_probe.cpp) and its NEGATIVE arm: the module with its font release function
# inside it, which must crash the probe. Registered in test/meson.build. The photograph, cut-outs and type are staged
# beside them, as in the bundle.
ui_stage = []
foreach f : ['panel.png', 'btn_mute.png', 'btn_tone.png', 'btn_strobe.png', 'btn_settings.png',
             'logo_on.png', 'logo_lit.png', 'logo_bypass.png']
  ui_stage += configure_file(input : '../gui/assets/' + f, output : f, copy : true)
endforeach
ui_stage += configure_file(input : '../gui/fonts/Oswald.ttf', output : 'Oswald.ttf', copy : true)
hawk_ui_unload_probe = executable('hawk_ui_unload_probe', 'ui_unload_probe.cpp',
  dependencies : [lv2_dep, cairo_dep, x11_dep, dl_dep], install : false)
hawk_ui_font_callback = shared_module('hawk_ui_font_callback', '../lv2/hawk_ui.cpp',
  dependencies        : [lv2_dep, cairo_dep, x11_dep, ft_dep],
  include_directories : include_directories('../src'),
  name_prefix         : '',
  cpp_args            : ['-fvisibility=hidden', '-fvisibility-inlines-hidden', '-fno-gnu-unique',
                         '-DHAWK_UI_FONT_CALLBACK_IN_MODULE=1'],
  install             : false)

# DETECT TUNING picks the tuning of open strings played one after another (tools/ui_detect_probe.cpp), and its negative
# arm with the old rules. Registered in test/meson.build.
hawk_ui_detect_probe = executable('hawk_ui_detect_probe', 'ui_detect_probe.cpp',
  include_directories : include_directories('../src'),
  dependencies : [lv2_dep, cairo_dep, x11_dep, ft_dep], install : false)
hawk_ui_detect_probe_old = executable('hawk_ui_detect_probe_old', 'ui_detect_probe.cpp',
  include_directories : include_directories('../src'),
  cpp_args : ['-DHAWK_UI_DETECT_OLD=1'],
  dependencies : [lv2_dep, cairo_dep, x11_dep, ft_dep], install : false)

# The GUI's host boundary (tools/ui_x_probe.cpp, tools/ui_ports_probe.cpp) and their negative arms. Registered in
# test/meson.build.
hawk_ui_x_probe = executable('hawk_ui_x_probe', 'ui_x_probe.cpp',
  dependencies : [lv2_dep, x11_dep, dl_dep], install : false)
hawk_ui_no_x_handler = shared_module('hawk_ui_no_x_handler', '../lv2/hawk_ui.cpp',
  dependencies        : [lv2_dep, cairo_dep, x11_dep, ft_dep],
  include_directories : include_directories('../src'),
  name_prefix         : '',
  cpp_args            : ['-fvisibility=hidden', '-fvisibility-inlines-hidden', '-fno-gnu-unique',
                         '-DHAWK_UI_NO_X_HANDLER=1'],
  install             : false)
hawk_ui_ports_probe = executable('hawk_ui_ports_probe', 'ui_ports_probe.cpp',
  include_directories : include_directories('../src'),
  dependencies : [lv2_dep, cairo_dep, x11_dep, ft_dep], install : false)
hawk_ui_ports_probe_ignore_ref = executable('hawk_ui_ports_probe_ignore_ref', 'ui_ports_probe.cpp',
  include_directories : include_directories('../src'), cpp_args : ['-DHAWK_UI_IGNORE_REF_NOTE=1'],
  dependencies : [lv2_dep, cairo_dep, x11_dep, ft_dep], install : false)
hawk_ui_ports_probe_trust = executable('hawk_ui_ports_probe_trust', 'ui_ports_probe.cpp',
  include_directories : include_directories('../src'), cpp_args : ['-DHAWK_UI_TRUST_PORTS=1'],
  dependencies : [lv2_dep, cairo_dep, x11_dep, ft_dep], install : false)

# The engine's audit fixes (tools/boundary_probe.cpp, tools/thread_probe.cpp, the bypass reading in bypass_probe.cpp)
# and the .so variants that restore each defect, for their negative arms. Registered in test/meson.build.
hawk_boundary_probe = executable('hawk_boundary_probe', 'boundary_probe.cpp',
  dependencies : [dl_dep, dependency('threads')], install : false)
hawk_thread_probe = executable('hawk_thread_probe', 'thread_probe.cpp', dependencies : [dl_dep], install : false)
hawk_variants = {}
foreach v : ['HAWK_OLD_NOTE_SNAP', 'HAWK_BYPASS_KEEPS_READING', 'HAWK_THREAD_UNGUARDED']
  hawk_variants += { v : shared_module('hawk_' + v.to_lower(), '../lv2/hawk_lv2.cpp',
    dependencies : [hawk_core_dep, lv2_dep, dependency('threads')], name_prefix : '',
    cpp_args : ['-fvisibility=hidden', '-D' + v + '=1'], install : false) }
endforeach

# Every GUI control clicked where it is drawn (tools/ui_click_probe.cpp), and its negative arm: clicks tested against
# the last frame's layout miss after a resize. Registered in test/meson.build.
hawk_ui_click_probe = executable('hawk_ui_click_probe', 'ui_click_probe.cpp',
  include_directories : include_directories('../src'),
  dependencies : [lv2_dep, cairo_dep, x11_dep, ft_dep], install : false)
hawk_ui_click_probe_stale = executable('hawk_ui_click_probe_stale', 'ui_click_probe.cpp',
  include_directories : include_directories('../src'), cpp_args : ['-DHAWK_UI_NO_LAYOUT_ON_PRESS=1'],
  dependencies : [lv2_dep, cairo_dep, x11_dep, ft_dep], install : false)
