#!/bin/bash
# Validate development artifacts on the actual Mac. No Developer ID is required.
set -euo pipefail
cd "$(dirname "$0")/.."
[[ "$(uname -s)" == Darwin ]] || { echo 'Native macOS validation requires a Mac.' >&2; exit 1; }
package_dir=${1:-dist/UFO}
build_dir=${2:-build-macos}
log_dir="$PWD/dist/validation"
mkdir -p "$log_dir"
package_dir=$(cd "$package_dir" && pwd)
build_dir=$(cd "$build_dir" && pwd)
for spec in 'UFO.app:APPL' 'UFO.vst3:BNDL'; do
  bundle_name=${spec%%:*}
  package_type=${spec##*:}
  bundle="$package_dir/$bundle_name"
  plist="$bundle/Contents/Info.plist"
  binary="$bundle/Contents/MacOS/UFO"
  [[ -f "$plist" && -x "$binary" ]] || { echo "Incomplete bundle: $bundle" >&2; exit 1; }
  [[ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$plist")" == UFO ]] || { echo "Unexpected executable in $bundle" >&2; exit 1; }
  [[ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundlePackageType' "$plist")" == "$package_type" ]] || { echo "Unexpected bundle type in $bundle" >&2; exit 1; }
  lipo -verify_arch arm64 x86_64 "$binary"
  codesign --verify --deep --strict --verbose=2 "$bundle"
  while IFS= read -r dependency; do
    case "$dependency" in
      /System/Library/*|/usr/lib/*|@rpath/*|@loader_path/*|@executable_path/*) ;;
      *) echo "Non-portable library reference in $binary: $dependency" >&2; exit 1 ;;
    esac
  done < <(otool -L "$binary" | awk '/^[[:space:]]/ {print $1}')
done
{
  printf 'macOS: '; sw_vers -productVersion
  printf 'Native architecture: '; uname -m
  printf 'CPU model: '; sysctl -n hw.model
  printf 'Logical CPUs: '; sysctl -n hw.logicalcpu
  printf 'Host test: %s\n' "$build_dir/ufo_vst3_host_tests"
} > "$log_dir/machine.txt"
[[ -x "$build_dir/ufo_vst3_host_tests" ]] || { echo 'Build ufo_vst3_host_tests before validation.' >&2; exit 1; }
"$build_dir/ufo_vst3_host_tests" "$package_dir/UFO.vst3" "$log_dir" --probe-device 2>&1 | tee "$log_dir/vst3-host.log"

# The host test above exercises audio/editor behavior. This separate check verifies
# the standalone process launches and remains alive; it is not a listening test.
app_pid=''
cleanup() {
  if [[ -n "$app_pid" ]] && kill -0 "$app_pid" 2>/dev/null; then
    kill -TERM "$app_pid" 2>/dev/null || true
    wait "$app_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT
"$package_dir/UFO.app/Contents/MacOS/UFO" > "$log_dir/standalone.log" 2>&1 &
app_pid=$!
for attempt in {1..5}; do
  sleep 1
  if ! kill -0 "$app_pid" 2>/dev/null; then
    status=0
    wait "$app_pid" || status=$?
    app_pid=''
    echo "Standalone exited during launch smoke check (status $status)." >&2
    cat "$log_dir/standalone.log" >&2
    exit 1
  fi
done
printf 'PASS: standalone remained alive for five seconds; audio was validated through the VST3 host test.\n' | tee "$log_dir/standalone-result.txt"
cleanup
app_pid=''
printf 'PASS: macOS universal bundle structure, signatures, portable linkage, native VST3 hosting and standalone launch.\n'
printf 'Unrun here: third-party DAW compatibility, human listening/third-party DAW assessment and execution on the other CPU architecture. Run scripts/listen-macos.sh for a physical output audition.\n'
