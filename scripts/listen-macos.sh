#!/bin/bash
# Deliberately plays the host-rendered instrument through this Mac's audio output.
set -euo pipefail
cd "$(dirname "$0")/.."
[[ "$(uname -s)" == Darwin ]] || { echo 'Physical Mac audition requires macOS.' >&2; exit 1; }
host="$PWD/build-macos/ufo_vst3_host_tests"
bundle="$PWD/dist/UFO/UFO.vst3"
[[ -x "$host" && -d "$bundle" ]] || { echo 'Run scripts/build-macos.sh first.' >&2; exit 1; }
mkdir -p dist/validation/listening
"$host" "$bundle" "$PWD/dist/validation/listening" --listen 2>&1 | tee dist/validation/listening/listening.log
echo 'Record what you heard and your actual DAW observations in dist/validation/listening/LISTENING-NOTES.md.'
echo 'The audio callback completing is an automated check; hearing/DAW verdicts require your observations.'
