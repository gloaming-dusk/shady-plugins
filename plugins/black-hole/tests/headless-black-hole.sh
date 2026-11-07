#!/usr/bin/env bash
# black-hole plugin: shaders build, the hole opens/closes on Super+H, a second
# press while busy is ignored, and the plugin reloads and unloads cleanly.
#
#   tests/headless-black-hole.sh <Shady build directory> <directory of the plugin's .so>
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SHADY="$(cd "$1" && pwd)/shady"
PLUGIN_DIR="$(cd "$2" && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir -m 700 "$tmp/runtime"
export XDG_RUNTIME_DIR="$tmp/runtime"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=gles2
export LIBGL_ALWAYS_SOFTWARE=1
export SHADY_LUA_INIT="$HERE/headless-black-hole-init.lua"
export SHADY_TEST_PLUGIN_DIR="$PLUGIN_DIR"
export ASAN_OPTIONS="${ASAN_OPTIONS:-abort_on_error=1:halt_on_error=1:detect_leaks=0}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}"
if ! timeout 10s "$SHADY" -c "$HERE/headless-black-hole-config.lua" > "$tmp/log" 2>&1; then
    cat "$tmp/log" >&2
    exit 1
fi
fail() { echo "headless-black-hole: $1" >&2; cat "$tmp/log" >&2; exit 1; }
if grep -Eq 'AddressSanitizer|runtime error:|UndefinedBehaviorSanitizer|plugin shader (compile|link) failed|scene capture failed' "$tmp/log"; then
    fail "sanitizer or shader error"
fi
for marker in "black-hole: swallowing" "black-hole: lensing the scene" "black-hole: busy" "black-hole: closed" \
        "reloaded black-hole" "unloaded black-hole" "black-hole-test: PASS"; do
    grep -Eq "$marker" "$tmp/log" || fail "missing '$marker'"
done
echo "headless-black-hole: PASS"
