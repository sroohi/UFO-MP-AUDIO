#!/bin/bash
# Deliberately plays the host-rendered instrument through this Mac's audio output.
set -euo pipefail
cd "$(dirname "$0")/.."
[[ "$(uname -s)" == Darwin ]] || { echo 'Physical Mac audition requires macOS.' >&2; exit 1; }
host="$PWD/build-macos/ufo_vst3_host_tests"
bundle="$PWD/dist/UFO/UFO.vst3"
log_dir=dist/validation/listening
host_options=(--listen)
[[ $# -le 1 ]] || { echo 'Usage: scripts/listen-macos.sh [--au]' >&2; exit 1; }
case "${1:-}" in
  '') ;;
  --au)
    bundle="${UFO_INSTALL_ROOT:-$HOME}/Library/Audio/Plug-Ins/Components/UFO.component"
    log_dir=dist/validation/listening/au
    host_options=(--au --listen)
    ;;
  *) echo 'Usage: scripts/listen-macos.sh [--au]' >&2; exit 1 ;;
esac
[[ -x "$host" && -d "$bundle" ]] || { echo 'Build first with scripts/build-macos.sh; --au also requires installing UFO.component.' >&2; exit 1; }
mkdir -p "$log_dir"
"$host" "$bundle" "$PWD/$log_dir" "${host_options[@]}" 2>&1 | tee "$log_dir/listening.log"
printf 'Record what you heard and your actual DAW observations in %s/LISTENING-NOTES.md.\n' "$log_dir"
echo 'The audio callback completing is an automated check; hearing/DAW verdicts require your observations.'
