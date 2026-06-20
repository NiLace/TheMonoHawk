# Tests for the core. Each test executable prints a table of numbers and exits
# non-zero on any failure, so `meson test` reports honest pass/fail.

test_note_mapping = executable('test_note_mapping',
  'test_note_mapping.cpp',
  dependencies : hawk_core_dep)

test('note mapping', test_note_mapping)

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

# Integration test: the LV2 plugin loaded as a host would, end to end. The
# executable (tools/) dlopens hawk.so (lv2/), so both must exist first; passing
# the target as an arg makes meson substitute its path and build it beforehand.
# Longer timeout: it feeds ~3.5 s of audio in real-time-ish blocks.
test('lv2 selftest', hawk_lv2_selftest,
  args    : [hawk_lv2, '110'],
  depends : hawk_lv2,
  timeout : 60)
