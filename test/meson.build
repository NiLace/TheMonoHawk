# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) The NL Sounds contributors
# Tests for the core. Each test executable prints a table of numbers and exits
# non-zero on any failure, so `meson test` reports honest pass/fail.

test_note_mapping = executable('test_note_mapping',
  'test_note_mapping.cpp',
  dependencies : hawk_core_dep)

test('note mapping', test_note_mapping)

test_ring_buffer = executable('test_ring_buffer',
  'test_ring_buffer.cpp',
  dependencies : hawk_core_dep)

test('ring buffer', test_ring_buffer)

test_pitch_detector = executable('test_pitch_detector',
  'test_pitch_detector.cpp',
  dependencies : hawk_core_dep)

test('pitch detector', test_pitch_detector)

test_pitch_reliability = executable('test_pitch_reliability',
  'test_pitch_reliability.cpp',
  dependencies : hawk_core_dep)

test('pitch reliability', test_pitch_reliability)

test_fine_tuner = executable('test_fine_tuner',
  'test_fine_tuner.cpp',
  dependencies : hawk_core_dep)

test('fine tuner', test_fine_tuner)

# The port map is written by hand in three places (hawk.ttl, the PortIndex enum, every probe's P_*);
# this checks they agree and that every probe connects every port.
test('port map: ttl, enum and probes agree', find_program('python3'),
  args    : ['-I', meson.project_source_root() / 'tools' / 'check_ports.py', meson.project_source_root()])

# Integration test: the LV2 plugin loaded as a host would, end to end. The
# executable (tools/) dlopens hawk.so (lv2/), so both must exist first; passing
# the target as an arg makes meson substitute its path and build it beforehand.
# Longer timeout: it feeds ~3.5 s of audio in real-time-ish blocks.
test('lv2 selftest', hawk_lv2_selftest,
  args    : [hawk_lv2, '110'],
  depends : hawk_lv2,
  timeout : 60)

# An overflowing ring must not produce a confident wrong note. Feeds ~4.5 s of audio in
# real-time-ish blocks, so it needs the longer timeout too.
test('lv2 ring gap', hawk_gap_probe,
  args    : [hawk_lv2],
  depends : hawk_lv2,
  timeout : 60)

# No reading may mix two analyses. ~12 s of audio at 32-sample blocks.
test('lv2 result tearing', hawk_tear_probe,
  args    : [hawk_lv2],
  depends : hawk_lv2,
  timeout : 90)

test('lv2 steady-state allocations', hawk_alloc_lv2_probe,
  args    : [hawk_lv2],
  depends : hawk_lv2,
  timeout : 60)

# A re-pluck must not move the reading nor blink the lamp. A2 with a hard re-pluck (4x the
# amplitude still ringing), E2 with a soft one (2x), the case a simpler detector missed. ~15 s each.
test('lv2 attack, A2 hard re-pluck', hawk_attack_probe,
  args    : [hawk_lv2, '110', '0.7'],
  depends : hawk_lv2,
  timeout : 60)

test('lv2 attack, E2 soft re-pluck', hawk_attack_probe,
  args    : [hawk_lv2, '82.407', '0.35'],
  depends : hawk_lv2,
  timeout : 60)

# No confident wrong note under rumble (D3: 79 wrong-note blocks in 12 s before the
# coherence veto, 0 after; E4's are the stream high-pass's).
test('lv2 no wrong note, D3 under rumble', hawk_wrongnote_probe,
  args    : [hawk_lv2, '146.83', '0.1'],
  depends : hawk_lv2,
  timeout : 60)

# With the stream high-pass, rumble neither changes the note (E4 had 23-86 wrong-note
# blocks in 12 s) nor mutes the bass (B0 under rumble 0.4: 0 voiced blocks before, ~all after).
test('lv2 no wrong note, E4 under rumble', hawk_wrongnote_probe,
  args    : [hawk_lv2, '329.63', '0.1'],
  depends : hawk_lv2,
  timeout : 60)

test('lv2 reads B0 under heavy rumble', hawk_wrongnote_probe,
  args    : [hawk_lv2, '30.868', '0.4', '0.8'],
  depends : hawk_lv2,
  timeout : 60)

# What a misbehaving host, UI or signal chain can do (tools/hostile_probe.cpp): one NaN or Inf input
# sample used to deafen the tuner until re-activate (and an Inf pinned the level meter); one NaN or Inf
# Dim Amount poisoned the output for good; a Dim Amount above 0 dB amplified; activate() twice and a
# negative sample rate aborted the host (std::terminate).
foreach c : ['input-nan', 'input-inf', 'dim-nan', 'dim-inf', 'dim-over', 'activate-twice', 'rate']
  test('lv2 hostile: ' + c, hawk_hostile_probe,
    args    : [hawk_lv2, c],
    depends : hawk_lv2,
    timeout : 60)
endforeach

# At rest on a track (digital silence after a note) the worker used to cost 2.8 % of a core: the coarse
# detector ran every hop on zeros, and its states went denormal. With the silence skip and FTZ/DAZ,
# 0.4 % (i7-8700K). CPU time from schedstat, so other load does not inflate it much; the ceiling leaves
# 2.5x headroom and sits at a third of the old figure.
test('lv2 cost at rest', hawk_cpu_lv2_probe,
  args    : [hawk_lv2, 'silence', '26', '48000', '1.0'],
  depends : hawk_lv2,
  timeout : 60)

# A note stopped dead (a hand on the strings): the windows that straddle the stop used to publish
# up to 6.7 c of drift and a frequency 28 c off for ~170 ms, lamp still lit (A2).
foreach f : ['82.41', '110', '196', '329.63']
  test('lv2 note stopped dead, ' + f + ' Hz', hawk_cutoff_probe,
    args    : [hawk_lv2, f],
    depends : hawk_lv2,
    timeout : 60)
endforeach

# The chromatic reading at other host rates: above 50 kHz the analysis runs on the input decimated to
# 44.1/48 kHz (88.2/96 kHz by 2, 176.4/192 kHz by 4). Seven notes, +10 and -7 c, each read to 0.3 c.
foreach r : ['44100', '88200', '192000']
  test('lv2 reads at ' + r + ' Hz', hawk_rate_probe,
    args    : [hawk_lv2, r],
    depends : hawk_lv2,
    timeout : 90)
endforeach

# The high-notes sweep: E2 to F#6 every three semitones, the
# 2nd partial at -6 dB, each read to 0.3 c. It guards against cents lost in the highs, which it
# does not find: a regression net.
foreach r : ['48000', '96000']
  test('lv2 high-notes sweep at ' + r + ' Hz', hawk_rate_probe,
    args    : [hawk_lv2, r, 'sweep'],
    depends : hawk_lv2,
    timeout : 120)
endforeach

# The strobe phase (a still pattern in tune, turning with the offset): A2 at 0, +2 and -3 c.
foreach r : ['48000', '96000']
  test('lv2 strobe phase at ' + r + ' Hz', hawk_strobe_probe,
    args    : [hawk_lv2, r],
    depends : hawk_lv2,
    timeout : 60)
endforeach

# The plugin's own bypass (lv2:enabled; the hawk in the GUI): bypassed the output IS the input, bit for bit, the
# tuner shows nothing, and coming back shows no reading from before. It also caught the dim's smoother stopping
# ~3e-5 short of 1.0 (0 of 45568 samples exact).
test('lv2 bypass', hawk_bypass_probe,
  args    : [hawk_lv2],
  depends : hawk_lv2,
  timeout : 60)

test('lv2 no note from noise', hawk_noise_probe,
  args    : [hawk_lv2],
  depends : hawk_lv2,
  timeout : 120)



# The GUI module unloads, and the host can keep drawing text after (tools/ui_unload_probe.cpp); the negative arm -- the font release function inside the module -- must crash. Skipped without an X display.
test('ui unloads, host draws after', hawk_ui_unload_probe,
  args    : [hawk_ui, meson.project_build_root() / 'tools'],
  depends : hawk_ui,
  timeout : 60)
test('ui unload probe sees a release in the module (negative arm)', hawk_ui_unload_probe,
  args        : [hawk_ui_font_callback, meson.project_build_root() / 'tools'],
  depends     : hawk_ui_font_callback,
  should_fail : true,
  timeout     : 60)

# DETECT TUNING: open strings played one after another while the previous ones ring (no silence between them) name
# their tuning; the old rules (capture only after a silence, ties to the fewest strings) must fail.
test('ui detect tuning', hawk_ui_detect_probe, timeout : 60)
test('ui detect tuning, old rules (negative arm)', hawk_ui_detect_probe_old, should_fail : true, timeout : 60)

# The GUI's host boundary: an X error on its connection does not end the host (and the negative arm without the
# handler does); what the host sends through port_event -- ref_note, NaN, infinities -- is shown right or ignored.
test('ui survives X errors', hawk_ui_x_probe,
  args : [hawk_ui, meson.project_build_root() / 'tools'], depends : hawk_ui, timeout : 60)
test('ui without an X handler dies (negative arm)', hawk_ui_x_probe,
  args : [hawk_ui_no_x_handler, meson.project_build_root() / 'tools'], depends : hawk_ui_no_x_handler,
  should_fail : true, timeout : 60)
test('ui reads ref_note and ignores garbage', hawk_ui_ports_probe, timeout : 60)
test('ui ignoring ref_note (negative arm)', hawk_ui_ports_probe_ignore_ref, should_fail : true, timeout : 60)
test('ui trusting the ports (negative arm)', hawk_ui_ports_probe_trust, should_fail : true, timeout : 60)

# The engine's audit fixes: a string near a note boundary reads as the nearer note (no "E +52 c", no old note's cents
# under the new note); bypassed, the whole reading is zero; activate() survives a refused thread. Each with the .so
# that restores the defect as its negative arm.
test('lv2 note boundary', hawk_boundary_probe, args : [hawk_lv2], depends : hawk_lv2, timeout : 60)
test('lv2 note boundary, old snap (negative arm)', hawk_boundary_probe,
  args : [hawk_variants['HAWK_OLD_NOTE_SNAP']], should_fail : true, timeout : 60)
test('lv2 bypass, reading kept (negative arm)', hawk_bypass_probe,
  args : [hawk_variants['HAWK_BYPASS_KEEPS_READING']], should_fail : true, timeout : 60)
test('lv2 activate without a thread', hawk_thread_probe, args : [hawk_lv2], depends : hawk_lv2, timeout : 60)
test('lv2 activate without a thread, unguarded (negative arm)', hawk_thread_probe,
  args : [hawk_variants['HAWK_THREAD_UNGUARDED']], should_fail : true, timeout : 60)

# Every GUI control, clicked where it is drawn, writes its port or opens its overlay; a click right after a resize
# lands (the negative arm, with the last frame's layout, misses).
test('ui clicks', hawk_ui_click_probe, args : [meson.project_build_root() / 'tools'], timeout : 60)
test('ui clicks on a stale layout (negative arm)', hawk_ui_click_probe_stale,
  args : [meson.project_build_root() / 'tools'], should_fail : true, timeout : 60)
