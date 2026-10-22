#!/bin/bash
# Use existing secure SSH authentication. Never disables host-key verification.
set -euo pipefail
cd "$(dirname "$0")/.."
target=${1:?Usage: scripts/probe-remote-macos.sh user@host}
[[ "$target" != -* && "$target" != *$'\n'* && "$target" != *' '* ]] || { echo 'Invalid SSH target.' >&2; exit 1; }
ssh -o BatchMode=yes -o StrictHostKeyChecking=yes -o ConnectTimeout=10 -- "$target" /bin/bash -s < scripts/probe-macos-runner.sh
