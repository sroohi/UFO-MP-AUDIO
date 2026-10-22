#!/bin/bash
# Build, validate and install locally; --replace retains backups of prior bundles.
set -euo pipefail
cd "$(dirname "$0")/.."
[[ "$(uname -s)" == Darwin ]] || { echo 'Run this script on the Mac receiving UFO.' >&2; exit 1; }
case "${1:-}" in ''|--replace) ;; *) echo 'Usage: scripts/install-from-source-macos.sh [--replace]' >&2; exit 1 ;; esac
scripts/build-macos.sh
scripts/install-local-macos.sh "$@"
open "${UFO_INSTALL_ROOT:-$HOME}/Applications/UFO.app"
echo 'UFO is installed and opened. Rescan VST3 instruments in your DAW.'
echo 'Run scripts/listen-macos.sh for the output-device audition and observation record.'
