#!/bin/bash
# Stage the reviewed source archive on a known Mac, then build/validate/install.
# Authentication and a verified known-host entry must already be configured.
set -euo pipefail
cd "$(dirname "$0")/.."
target=${1:?Usage: scripts/install-remote-macos.sh user@host [--replace]}
replace=${2:-}
[[ "$target" != -* && "$target" != *$'\n'* && "$target" != *' '* ]] || { echo 'Invalid SSH target.' >&2; exit 1; }
[[ -z "$replace" || "$replace" == --replace ]] || { echo 'Only --replace is accepted as the second argument.' >&2; exit 1; }
archive="$PWD/dist/UFO-source.zip"
[[ -f "$archive" ]] || { echo 'Missing dist/UFO-source.zip; generate the current source archive first.' >&2; exit 1; }
scripts/probe-remote-macos.sh "$target"
remote_dir=$(ssh -o BatchMode=yes -o StrictHostKeyChecking=yes -o ConnectTimeout=10 -- "$target" 'mktemp -d /tmp/ufo-install.XXXXXXXX')
[[ "$remote_dir" =~ ^/tmp/ufo-install\.[A-Za-z0-9]+$ ]] || { echo 'Mac returned an unexpected staging path.' >&2; exit 1; }
scp -B -o StrictHostKeyChecking=yes "$archive" "$target:$remote_dir/UFO-source.zip"
# Only generated staging paths and a fixed option are inserted in this command.
ssh -o BatchMode=yes -o StrictHostKeyChecking=yes -- "$target" "/bin/bash -s -- '$remote_dir' '$replace'" <<'REMOTE'
set -euo pipefail
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
stage=$1
replace=$2
[[ "$(uname -s)" == Darwin ]] || { echo 'Destination is not macOS.' >&2; exit 1; }
workspace="$HOME/Library/Caches/MP Audio/UFO Setup/$(date +%Y%m%d-%H%M%S)-$$"
mkdir -p "$workspace"
unzip -q "$stage/UFO-source.zip" -d "$workspace"
cd "$workspace/UFO"
if [[ -n "$replace" ]]; then scripts/install-from-source-macos.sh "$replace"; else scripts/install-from-source-macos.sh; fi
rm -rf "$stage"
printf 'Mac build sources and validation logs retained in: %s\n' "$workspace/UFO"
printf 'For physical audition run there: scripts/listen-macos.sh\n'
REMOTE
