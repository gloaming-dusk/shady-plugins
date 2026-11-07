#!/usr/bin/env bash
# Build every plugin against a Shady build and run its tests in a real
# compositor. This needs a GL render node (software is fine), so CI, which has
# none, runs only the unit tests.
#
#   tests/run-all.sh <Shady build directory>
#
# The directory needs shady, headless-automation-probe and
# libshady-plugin-fps-cube.so, built with the same flags as for its spatial suite.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SHADY_BUILD="$(cd "$1" && pwd)"
SHADY_SOURCE="${SHADY_SOURCE:-$(cd "$SHADY_BUILD/.." && pwd)}"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
for dir in "$ROOT"/plugins/*/; do
  name="$(basename "$dir")"
  CPATH="$SHADY_SOURCE/include${CPATH:+:$CPATH}" meson setup "$tmp/$name" "$dir" >/dev/null
  CPATH="$SHADY_SOURCE/include${CPATH:+:$CPATH}" ninja -C "$tmp/$name" >/dev/null
  meson test -C "$tmp/$name" --print-errorlogs >/dev/null
  "$ROOT/tests/smoke.sh" "$SHADY_BUILD" "$tmp/$name" "$name"
  for script in "$dir"tests/headless-*.sh; do
    [[ -x "$script" ]] && "$script" "$SHADY_BUILD" "$tmp/$name"
  done
done
echo "shady-plugins: PASS"
