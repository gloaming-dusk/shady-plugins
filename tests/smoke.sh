#!/usr/bin/env bash
# Load a plugin in a real compositor, from where it is installed, with no
# $SHADY_ROOT: its shaders are found and compile, it runs, and it reloads and
# unloads where it can. A fake host cannot show this, as the real one runs a
# private copy of the plugin.
#
#   tests/smoke.sh <Shady build directory> <directory of the plugin's .so> <name>
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SHADY="$(cd "$1" && pwd)/shady"
export SHADY_TEST_PLUGIN_DIR="$(cd "$2" && pwd)" SHADY_TEST_PLUGIN="$3"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir -m 700 "$tmp/runtime"
export XDG_RUNTIME_DIR="$tmp/runtime"
unset WAYLAND_DISPLAY WAYLAND_SOCKET SHADY_ROOT
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=gles2 LIBGL_ALWAYS_SOFTWARE=1
export SHADY_LUA_INIT="$HERE/smoke-init.lua"
export ASAN_OPTIONS="${ASAN_OPTIONS:-abort_on_error=1:halt_on_error=1:detect_leaks=0}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}"
fail() { echo "smoke $SHADY_TEST_PLUGIN: $1" >&2; tail -n 25 "$tmp/log" >&2; exit 1; }
timeout 15s "$SHADY" -c "$HERE/smoke-config.lua" >"$tmp/log" 2>&1 || fail "the compositor failed (status $?)"
! grep -Eq 'AddressSanitizer|runtime error:|plugin shader|failed to build|shader (compile|link) failed' "$tmp/log" || fail "sanitizer or shader error"
grep -q 'smoke-test: PASS' "$tmp/log" || fail "the test did not pass"
echo "smoke $SHADY_TEST_PLUGIN: PASS"
