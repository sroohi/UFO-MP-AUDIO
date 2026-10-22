#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ "$(uname -s)" != Darwin ]]; then
  echo 'macOS with Xcode Command Line Tools is required.' >&2
  exit 1
fi
command -v cmake >/dev/null || { echo 'Install CMake 3.22+ (for example, brew install cmake).' >&2; exit 1; }
xcode-select -p >/dev/null || { echo 'Run xcode-select --install first.' >&2; exit 1; }
mkdir -p dist/validation
cmake -S . -B build-macos -G "Unix Makefiles" \
  -DCMAKE_OSX_ARCHITECTURES='arm64;x86_64' \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 -DCMAKE_BUILD_TYPE=Release 2>&1 | tee dist/validation/configure.log
cp build-macos/ufo-architecture-targets.txt dist/validation/architectures.txt
test_targets=(ufo_engine_tests ufo_sample_tests ufo_streaming_tests ufo_stream_asset_tests ufo_plugin_tests ufo_gpu_tests ufo_vst3_host_tests)
cmake --build build-macos --config Release --parallel 3 \
  --target UFO_Standalone UFO_VST3 UFO_AU "${test_targets[@]}" 2>&1 | tee dist/validation/build.log
{
  native_arch=$(uname -m)
  for test_target in "${test_targets[@]}"; do
    built_arch=$(lipo "$PWD/build-macos/$test_target" -archs)
    printf '%s: %s\n' "$test_target" "$built_arch"
    [[ "$built_arch" == "$native_arch" ]] || { echo "Unexpected test architecture for $test_target; expected only $native_arch." >&2; exit 1; }
  done
} | tee dist/validation/test-architectures.txt
UFO_SCREENSHOT="$PWD/dist/validation/UFO-native-editor.png" \
UFO_GPU_ARTIFACT_DIR="$PWD/dist/validation/gpu" \
  ctest --test-dir build-macos -C Release --output-on-failure 2>&1 | tee dist/validation/ctest.log

# Validate all staged formats before replacing any previously generated package.
stage=$(mktemp -d "$PWD/dist/.ufo-package.XXXXXX")
trap 'rm -rf "$stage"' EXIT
mkdir "$stage/UFO"
ditto build-macos/UFO_artefacts/Release/Standalone/UFO.app "$stage/UFO/UFO.app"
ditto build-macos/UFO_artefacts/Release/VST3/UFO.vst3 "$stage/UFO/UFO.vst3"
ditto build-macos/UFO_artefacts/Release/AU/UFO.component "$stage/UFO/UFO.component"
for bundle in "$stage/UFO/UFO.app" "$stage/UFO/UFO.vst3" "$stage/UFO/UFO.component"; do
  codesign --force --deep --sign - "$bundle"
done
scripts/validate-macos.sh "$stage/UFO" "$PWD/build-macos" 2>&1 | tee dist/validation/bundle-validation.log
cp scripts/install-local-macos.sh "$stage/UFO/Install.command"
chmod +x "$stage/UFO/Install.command"
printf 'Local development build; ad-hoc signed, not notarized.\nSee validation logs for native test results and unrun checks.\n' > "$stage/UFO/BUILD-STATUS.txt"
ditto -c -k --sequesterRsrc --keepParent "$stage/UFO" "$stage/UFO-macOS-universal-unsigned.zip"
# dist/UFO is generated only by this build script.
rm -rf dist/UFO
mv "$stage/UFO" dist/UFO
mv "$stage/UFO-macOS-universal-unsigned.zip" dist/UFO-macOS-universal-unsigned.zip
echo 'Built and validated universal app, VST3 and Logic Audio Unit for local development. See dist/validation.'
