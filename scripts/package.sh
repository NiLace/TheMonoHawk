#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) The NL Sounds contributors
# Build the release tarball: a clean release build, the bundle exactly as scripts/install.sh lays it out, packed as
# <PRODUCT>-<version>-linux-x86_64.tar.gz with the bundle folder at its root (extract it into ~/.lv2/).
#
# It refuses to pack a binary that uses AVX/AVX2/FMA (ymm registers): the bundle is for generic x86-64, and a binary
# with those instructions does not run slowly on an older CPU, it crashes the host.
#
# usage: scripts/package.sh [OUT_DIR]      (default: dist/)
set -euo pipefail

PRODUCT=TheMonoHawk
BUNDLE_NAME=themonohawk.lv2
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT_DIR="$(mkdir -p "${1:-$ROOT/dist}" && cd "${1:-$ROOT/dist}" && pwd)"
VERSION="$(sed -n "s/^ *version *: *'\([^']*\)'.*/\1/p" "$ROOT/meson.build" | head -1)"
[ -n "$VERSION" ] || { echo "package.sh: no version in meson.build" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
meson setup "$WORK/build" "$ROOT" --buildtype=release -Db_sanitize=none >/dev/null
ninja -C "$WORK/build" lv2/hawk.so lv2/hawk_ui.so >/dev/null

for so in hawk.so hawk_ui.so; do
    if objdump -d --no-show-raw-insn "$WORK/build/lv2/$so" | grep -q '%ymm'; then
        echo "package.sh: $so uses 256-bit AVX instructions -- not a generic x86-64 build" >&2
        exit 1
    fi
done

B="$WORK/stage/$BUNDLE_NAME"
mkdir -p "$B"
cp "$WORK/build/lv2/hawk.so" "$WORK/build/lv2/hawk_ui.so" "$B/"
cp "$ROOT/lv2/hawk.ttl" "$ROOT/lv2/manifest.ttl" "$B/"
cp "$ROOT"/gui/assets/*.png "$B/"
cp "$ROOT/gui/fonts/Oswald.ttf" "$ROOT/gui/fonts/OFL-Oswald.txt" "$B/"

TAR="$OUT_DIR/$PRODUCT-$VERSION-linux-x86_64.tar.gz"
# Reproducible: sorted names, numeric ids 0, every file stamped with the last commit's time.
STAMP="$(git -C "$ROOT" log -1 --format=%ct 2>/dev/null || date +%s)"
tar --sort=name --owner=0 --group=0 --numeric-owner --mtime="@$STAMP" -C "$WORK/stage" -cf - "$BUNDLE_NAME" \
    | gzip -n -9 > "$TAR"
echo "$TAR"
sha256sum "$TAR" | sed "s|$OUT_DIR/||"
