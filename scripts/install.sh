#!/usr/bin/env bash
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
mkdir -p "$BUNDLE"

ninja -C "$ROOT/build"

install_atomic() {  # src dst  — atomic replace, never corrupts a mapped lib
    cp "$1" "$2.new"
    mv -f "$2.new" "$2"
}

install_atomic "$ROOT/build/lv2/hawk.so"    "$BUNDLE/hawk.so"
install_atomic "$ROOT/build/lv2/hawk_ui.so" "$BUNDLE/hawk_ui.so"

# Data files (read at load time, not memory-mapped as code) — plain copy is fine.
cp "$ROOT/lv2/hawk.ttl"          "$BUNDLE/hawk.ttl"
cp "$ROOT/lv2/manifest.ttl"      "$BUNDLE/manifest.ttl"
cp "$ROOT/assets/images/Hawk.png" "$BUNDLE/Hawk.png"

echo "Installed to $BUNDLE (atomic library replace)"
