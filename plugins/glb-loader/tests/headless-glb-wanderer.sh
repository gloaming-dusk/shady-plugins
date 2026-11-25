#!/usr/bin/env bash
# glb-wanderer in a real compositor: it waits for the model init.lua
# publishes, shows it, survives a hot reload while it walks, draws something
# where the floor would be empty, and unloads cleanly.
#
#   tests/headless-glb-wanderer.sh <Shady build directory> <directory of the plugin's .so>
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SHADY="$(cd "$1" && pwd)/shady"
PLUGIN_DIR="$(cd "$2" && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir -m 700 "$tmp/runtime"
python3 -I "$HERE/make-test-room.py" "$tmp/model.glb"
export XDG_RUNTIME_DIR="$tmp/runtime"
unset WAYLAND_DISPLAY WAYLAND_SOCKET SHADY_ROOT
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=gles2 LIBGL_ALWAYS_SOFTWARE=1
export SHADY_LUA_INIT="$HERE/headless-glb-wanderer-init.lua"
export SHADY_TEST_PLUGIN_DIR="$PLUGIN_DIR" SHADY_TEST_GLB="$tmp/model.glb"
export SHADY_TEST_SHOT="$tmp/shot.png"
export ASAN_OPTIONS="${ASAN_OPTIONS:-abort_on_error=1:halt_on_error=1:detect_leaks=0}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}"
if ! timeout 30s "$SHADY" -c "$HERE/headless-glb-wanderer-config.lua" > "$tmp/log" 2>&1; then
    cat "$tmp/log" >&2
    exit 1
fi
fail() { echo "headless-glb-wanderer: $1" >&2; cat "$tmp/log" >&2; exit 1; }
if grep -Eq 'AddressSanitizer|runtime error:|UndefinedBehaviorSanitizer' "$tmp/log"; then
    fail "sanitizer report"
fi
grep -q "glb-wanderer: waiting for" "$tmp/log" || fail "it did not wait for a model"
# Shown at startup and again after the reload.
[[ "$(grep -Ec "glb-wanderer: .*model.glb: 10 triangles" "$tmp/log")" == 2 ]] ||
    fail "expected the model to load twice"
! grep -Eq "glb-wanderer: .*(refused|cannot|invalid)" "$tmp/log" || fail "the plugin reported an error"
grep -q "screenshot saved" "$tmp/log" || fail "no screenshot"
grep -q "unloaded glb-wanderer" "$tmp/log" || fail "unload failed"
grep -q "glb-wanderer-test: PASS" "$tmp/log" || fail "the test did not finish"
echo "headless-glb-wanderer: PASS"
