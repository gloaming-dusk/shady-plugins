#!/usr/bin/env bash
# frozen-window plugin: shaders build and render on a live window, Super+Z
# freezes and thaws it, Super+Shift+Z freezes/thaws the workspace, and the
# plugin reloads and unloads cleanly while windows are frozen.
#
#   tests/headless-frozen-window.sh <Shady build directory> <directory of the plugin's .so>
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$(cd "$1" && pwd)"
PLUGIN_DIR="$(cd "$2" && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir -m 700 "$tmp/runtime"
export XDG_RUNTIME_DIR="$tmp/runtime"
# Spawned clients inherit our environment; let them find the nested socket.
unset WAYLAND_DISPLAY WAYLAND_SOCKET
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=gles2
export LIBGL_ALWAYS_SOFTWARE=1
export SHADY_LUA_INIT="$HERE/headless-frozen-window-init.lua"
export SHADY_TEST_PLUGIN_DIR="$PLUGIN_DIR"
export SHADY_AUTOMATION_PROBE="$BUILD_DIR/headless-automation-probe"
export ASAN_OPTIONS="${ASAN_OPTIONS:-abort_on_error=1:halt_on_error=1:detect_leaks=0}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}"
if ! timeout 15s "$BUILD_DIR/shady" -c "$HERE/headless-frozen-window-config.lua" > "$tmp/log" 2>&1; then
    cat "$tmp/log" >&2
    exit 1
fi
fail() { echo "headless-frozen-window: $1" >&2; cat "$tmp/log" >&2; exit 1; }
if grep -Eq 'AddressSanitizer|runtime error:|UndefinedBehaviorSanitizer|plugin shader (compile|link) failed' "$tmp/log"; then
    fail "sanitizer or shader error"
fi
for marker in "frozen-window: no focused window" "frozen-window: freezing$" "frozen-window: thawing$" \
        "frozen-window: freezing the workspace" \
        "frozen-window: thawing every window" "reloaded frozen-window" "unloaded frozen-window" \
        "frozen-window-test: PASS"; do
    grep -Eq "$marker" "$tmp/log" || fail "missing '$marker'"
done
echo "headless-frozen-window: PASS"
