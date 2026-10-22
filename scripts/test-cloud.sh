#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
source /workspace/.ufo-system/activate.sh
# Exercise the real embedded VST3 native window as well as shared-code UI painting.
display_file=$(mktemp)
wm_config=$(mktemp -d /tmp/ufo-wm.XXXXXX)
wm_pid=''
xvfb_pid=''
cleanup() {
 # The supervisor stops and reaps only the WM/private D-Bus tree it owns.
 if [[ -n "$wm_pid" ]]; then kill -TERM "$wm_pid" 2>/dev/null || true; wait "$wm_pid" 2>/dev/null || true; fi
 if [[ -n "$xvfb_pid" ]]; then kill "$xvfb_pid" 2>/dev/null || true; wait "$xvfb_pid" 2>/dev/null || true; fi
 rm -f "$display_file"
 rm -rf "$wm_config"
}
trap cleanup EXIT
/workspace/.ufo-system/root/usr/bin/Xvfb -displayfd 3 -screen 0 1280x1024x24 -nolisten tcp 3>"$display_file" > /tmp/ufo-xvfb.log 2>&1 &
xvfb_pid=$!
for attempt in {1..50}; do
 [[ -s "$display_file" ]] && break
 kill -0 "$xvfb_pid" 2>/dev/null || { cat /tmp/ufo-xvfb.log >&2; exit 1; }
 sleep .1
done
[[ -s "$display_file" ]] || { echo 'Virtual display did not start' >&2; exit 1; }
export DISPLAY=":$(cat "$display_file")"
export XDG_CACHE_HOME="$wm_config/cache"
mkdir -p "$XDG_CACHE_HOME"
for dependency in xfwm4 dbus-run-session xprop python3; do
 command -v "$dependency" >/dev/null || { echo "This cloud image needs $dependency to validate a native embedded plugin window." >&2; exit 1; }
done
# Xfconf must activate inside this writable configuration and private session bus.
# A pre-existing desktop bus can retain stale/unwritable settings from another run.
NO_AT_BRIDGE=1 XDG_CONFIG_HOME="$wm_config" python3 scripts/run-wm-session.py > /tmp/ufo-wm.log 2>&1 &
wm_pid=$!
wm_ready=false
for attempt in {1..50}; do
 if xprop -root _NET_SUPPORTING_WM_CHECK 2>/dev/null | rg -q 'window id # 0x[1-9a-fA-F]'; then wm_ready=true; break; fi
 kill -0 "$wm_pid" 2>/dev/null || { cat /tmp/ufo-wm.log >&2; exit 1; }
 sleep .1
done
[[ "$wm_ready" == true ]] || { echo 'Window manager did not become ready' >&2; exit 1; }
export UFO_SCREENSHOT="$PWD/build-native/UFO-editor.png"
export UFO_GPU_ARTIFACT_DIR="$PWD/build-native/validation/gpu"
if [[ "${1:-}" == --gpu-only ]]; then
 ctest --test-dir build-native --output-on-failure -R '^gpu_visualization$'
 exit
fi
ctest --test-dir build-native --output-on-failure
mkdir -p build-native/validation
build-native/ufo_vst3_host_tests "$PWD/build-native/UFO_artefacts/Release/VST3/UFO.vst3" "$PWD/build-native/validation" 2>&1 | tee build-native/validation/vst3-host.log
