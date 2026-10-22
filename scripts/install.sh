#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) The NL Sounds contributors
# Build and install the LV2 bundle to ~/.lv2/themonohawk.lv2/.
#
# IMPORTANT: libraries are replaced atomically (write a temp file, then rename
# over the target). Overwriting a .so *in place* while a host (Ardour) has it
# memory-mapped corrupts the running process and crashes it. rename() swaps the
# directory entry to a fresh inode and leaves the old one valid for anything
# still using it, so the host never sees a half-written library.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUNDLE="$HOME/.lv2/themonohawk.lv2"

# Preferred path: the meson build tree. But meson/ninja are not on every machine this repo gets
# cloned to, and the fallback below is a two-line compile -- there is no reason for "no build
# system installed" to mean "cannot install the plugin". The flags are meson's release build
# (buildtype=release with b_ndebug=if-release: -O3 -DNDEBUG, no _GLIBCXX_ASSERTIONS). They must stay
# the same flags, or the fallback installs a different binary from the one every gate and measurement
# runs on. Not byte-identical (meson links the core as a static library), so a figure still belongs to
# the meson build.
OUT="$ROOT/build/lv2"
if command -v ninja >/dev/null 2>&1 && [ -d "$ROOT/build" ]; then
    # Only a release build without sanitizers is installed: a sanitizer build's .so links libasan, and a host that
    # loads it without the runtime preloaded EXITS -- the DAW dies, not the plugin.
    if command -v meson >/dev/null 2>&1; then
        cfg="$(meson introspect "$ROOT/build" --buildoptions 2>/dev/null | python3 -c '
import json, sys
o = {x["name"]: x["value"] for x in json.load(sys.stdin)}
print(o.get("buildtype"), ",".join(o.get("b_sanitize") or []) if isinstance(o.get("b_sanitize"), list) else o.get("b_sanitize"))' || true)"
        case "$cfg" in
            "release none"|"release ") ;;
            *) echo "install.sh: build/ is configured as '$cfg' -- only a release build without sanitizers is installed" >&2
               echo "            (meson configure build -Dbuildtype=release -Db_sanitize=none)" >&2
               exit 1 ;;
        esac
    fi
    ninja -C "$ROOT/build"
else
    echo "no ninja build tree -- compiling directly"
    OUT="$(mktemp -d)"
    trap 'rm -rf "$OUT"' EXIT
    g++ -std=c++17 -O3 -DNDEBUG -D_FILE_OFFSET_BITS=64 -pthread -fPIC -shared -fvisibility=hidden -I "$ROOT/src" \
        "$ROOT/lv2/hawk_lv2.cpp" "$ROOT/src/pitch_detector.cpp" \
        "$ROOT/src/fine_tuner.cpp" "$ROOT/src/note_mapping.cpp" \
        -o "$OUT/hawk.so" -lpthread $(pkg-config --cflags lv2 2>/dev/null || true)
    g++ -std=c++17 -O3 -DNDEBUG -D_FILE_OFFSET_BITS=64 -pthread -fPIC -shared -fvisibility=hidden -I "$ROOT/src" \
        -fvisibility-inlines-hidden -fno-gnu-unique \
        "$ROOT/lv2/hawk_ui.cpp" -o "$OUT/hawk_ui.so" \
        $(pkg-config --cflags --libs cairo x11 freetype2)
fi

mkdir -p "$BUNDLE"

install_atomic() {  # src dst  — atomic replace, never corrupts a mapped lib
    cp "$1" "$2.new"
    mv -f "$2.new" "$2"
}

install_atomic "$OUT/hawk.so"    "$BUNDLE/hawk.so"
install_atomic "$OUT/hawk_ui.so" "$BUNDLE/hawk_ui.so"

# Data files (read at load time, not memory-mapped as code) — plain copy is fine.
cp "$ROOT/lv2/hawk.ttl"          "$BUNDLE/hawk.ttl"
cp "$ROOT/lv2/manifest.ttl"      "$BUNDLE/manifest.ttl"
# The rack GUI: its photograph and cut-outs (gui/assets) and its type (OFL, the licence beside it)
rm -f "$BUNDLE"/*.png                 # images a former version shipped and this one does not
cp "$ROOT"/gui/assets/*.png "$BUNDLE/"
cp "$ROOT/gui/fonts/Oswald.ttf" "$ROOT/gui/fonts/OFL-Oswald.txt" "$BUNDLE/"

echo "Installed to $BUNDLE (atomic library replace)"
sha256sum "$BUNDLE/hawk.so" "$BUNDLE/hawk_ui.so" | sed "s|$BUNDLE/||"
