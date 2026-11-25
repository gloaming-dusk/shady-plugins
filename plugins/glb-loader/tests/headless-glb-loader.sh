#!/usr/bin/env bash
# glb-loader in a real compositor: a configured .glb environment loads the
# plugin by name, the test room loads with its hidden collision proxy, again
# after a hot reload and after a broken file, and unloading drops it.
#
#   tests/headless-glb-loader.sh <Shady build directory> <directory of the plugin's .so>
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SHADY="$(cd "$1" && pwd)/shady"
PLUGIN_DIR="$(cd "$2" && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir -m 700 "$tmp/runtime"
python3 -I "$HERE/make-test-room.py" "$tmp/test-room.glb"
printf 'glTF\002\000\000\000' > "$tmp/broken.glb"
export XDG_RUNTIME_DIR="$tmp/runtime"
unset WAYLAND_DISPLAY WAYLAND_SOCKET SHADY_ROOT
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=gles2 LIBGL_ALWAYS_SOFTWARE=1
export SHADY_LUA_INIT="$HERE/headless-glb-loader-init.lua"
export SHADY_TEST_PLUGIN_DIR="$PLUGIN_DIR" SHADY_TEST_GLB="$tmp/test-room.glb"
export SHADY_TEST_BROKEN_GLB="$tmp/broken.glb"
export ASAN_OPTIONS="${ASAN_OPTIONS:-abort_on_error=1:halt_on_error=1:detect_leaks=0}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}"
if ! timeout 10s "$SHADY" -c "$HERE/headless-glb-loader-config.lua" > "$tmp/log" 2>&1; then
    cat "$tmp/log" >&2
    exit 1
fi
fail() { echo "headless-glb-loader: $1" >&2; cat "$tmp/log" >&2; exit 1; }
if grep -Eq 'AddressSanitizer|runtime error:|UndefinedBehaviorSanitizer' "$tmp/log"; then
    fail "sanitizer report"
fi
loaded="environment: 'glb' loaded .*test-room.glb \(10 triangles, 1 collision boxes, 12 collision triangles\)"
# Startup, after the reload, and after restoring the path.
[[ "$(grep -Ec "$loaded" "$tmp/log")" == 3 ]] || fail "expected three scene loads"
[[ "$(grep -c "environment: registered loader 'glb'" "$tmp/log")" == 2 ]] ||
    fail "expected registration at startup and after reload"
grep -q "glb-loader: .*broken.glb" "$tmp/log" || fail "the broken file was not reported"
grep -q "environment: unregistered loader 'glb'" "$tmp/log" || fail "unload did not unregister"
grep -q "glb-loader-test: PASS" "$tmp/log" || fail "the test did not finish"
echo "headless-glb-loader: PASS"
