#!/bin/bash
# Read-only capability report. It does not install software or access credentials.
set -euo pipefail
[[ "$(uname -s)" == Darwin ]] || { echo 'Runner is not macOS.' >&2; exit 1; }
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
printf 'macOS: '; sw_vers -productVersion
printf 'Architecture: '; uname -m
printf 'Model: '; sysctl -n hw.model
printf 'Logical CPUs: '; sysctl -n hw.logicalcpu
printf 'Memory bytes: '; sysctl -n hw.memsize
printf 'Console session user: '; stat -f '%Su' /dev/console
if pgrep -x WindowServer >/dev/null; then echo 'WindowServer: present'; else echo 'WindowServer: absent; native GPU/editor checks need a desktop session'; fi
for tool in git cmake make codesign xcrun; do
 command -v "$tool" >/dev/null || { echo "Required tool missing: $tool" >&2; exit 1; }
done
printf 'Developer tools: '; xcode-select -p
cmake --version | head -1
git --version
df -h .
echo 'Installed DAW candidates (application bundle names only):'
for applications in /Applications "$HOME/Applications"; do
 if [[ -d "$applications" ]]; then
  find "$applications" -maxdepth 1 -type d \( -iname '*reaper*.app' -o -iname '*ableton*.app' -o -iname '*bitwig*.app' -o -iname '*cubase*.app' -o -iname '*logic*.app' -o -iname '*studio one*.app' -o -iname '*pro tools*.app' \) -exec basename '{}' \;
 fi
done
echo 'Physical audio availability is probed by the host test; human listening/DAW observations are separate.'
