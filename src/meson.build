# The format-agnostic core. Knows nothing about LV2, CLAP, Pugl or Cairo.
# The plugin wrappers (added in later layers) link against this same library.

hawk_core_sources = [
  'note_mapping.cpp',
  'pitch_detector.cpp',
  'fine_tuner.cpp',
]

hawk_core_lib = static_library('hawk_core',
  hawk_core_sources)

hawk_core_dep = declare_dependency(
  link_with          : hawk_core_lib,
  include_directories : include_directories('.'))
