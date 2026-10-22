#!/bin/bash
set -euo pipefail
script_dir=$(cd "$(dirname "$0")" && pwd)
[[ "$(uname -s)" == Darwin ]] || { echo 'macOS required' >&2; exit 1; }
replace=false
case "${1:-}" in '' ) ;; --replace) replace=true ;; *) printf 'Usage: %s [--replace]\n' "$(basename "$0")" >&2; exit 1 ;; esac
packaged=false
if [[ -d "$script_dir/UFO.app" || -d "$script_dir/UFO.vst3" || -d "$script_dir/UFO.component" ]]; then
  package_dir="$script_dir"
  packaged=true
else
  package_dir="$script_dir/../dist/UFO"
fi
if pgrep -x -u "$(id -u)" UFO >/dev/null; then
  echo 'Quit the standalone UFO app before installing so unsaved patches stay under your control.' >&2
  exit 1
fi
if pgrep -x -u "$(id -u)" 'Logic Pro' >/dev/null || pgrep -x -u "$(id -u)" 'Logic Pro X' >/dev/null; then
  echo 'Quit Logic Pro before installing UFO so your current session stays under your control.' >&2
  exit 1
fi
bundle_names=("UFO.app" "UFO.vst3" "UFO.component")
bundle_labels=("app" "VST3" "Audio Unit")
native_arch=$(uname -m)
for name in "${bundle_names[@]}"; do
  source="$package_dir/$name"
  [[ -d "$source" && -x "$source/Contents/MacOS/UFO" ]] || { echo "Missing $source; run scripts/build-macos.sh first." >&2; exit 1; }
  lipo "$source/Contents/MacOS/UFO" -verify_arch "$native_arch"
  codesign --verify --deep --strict "$source"
done
install_root=${UFO_INSTALL_ROOT:-$HOME}
[[ "$install_root" == /* ]] || { echo 'UFO_INSTALL_ROOT must be an absolute path.' >&2; exit 1; }
parents=("$install_root/Applications" "$install_root/Library/Audio/Plug-Ins/VST3" "$install_root/Library/Audio/Plug-Ins/Components")
targets=("${parents[0]}/UFO.app" "${parents[1]}/UFO.vst3" "${parents[2]}/UFO.component")
mkdir -p "${parents[@]}"
for target in "${targets[@]}"; do
  if [[ -e "$target" || -L "$target" ]]; then
    [[ "$replace" == true ]] || { echo "Already exists: $target. Use --replace to retain a backup and install this build." >&2; exit 1; }
    [[ -d "$target" && ! -L "$target" ]] || { echo "Refusing to replace a non-bundle or symlink: $target" >&2; exit 1; }
  fi
done
stages=("" "" "")
backups=("" "" "")
installed=(false false false)
success=false
cleanup() {
  local index
  if [[ "$success" != true ]]; then
    for ((index=${#bundle_names[@]}-1; index>=0; --index)); do
      [[ "${installed[index]}" != true ]] || rm -rf "${targets[index]}"
      if [[ -n "${backups[index]}" && -d "${backups[index]}/${bundle_names[index]}" ]]; then
        mv "${backups[index]}/${bundle_names[index]}" "${parents[index]}/"
      fi
    done
  fi
  for index in "${!bundle_names[@]}"; do
    [[ -z "${stages[index]}" ]] || rm -rf "${stages[index]}"
  done
}
trap cleanup EXIT
for index in "${!bundle_names[@]}"; do
  stages[index]=$(mktemp -d "${parents[index]}/.ufo-install.XXXXXX")
  staged="${stages[index]}/${bundle_names[index]}"
  ditto "$package_dir/${bundle_names[index]}" "$staged"
  codesign --verify --deep --strict "$staged"
  # Clear browser quarantine only from these verified copies; skip symlinks.
  while IFS= read -r -d '' item; do
    if xattr -p com.apple.quarantine "$item" >/dev/null 2>&1; then
      xattr -d com.apple.quarantine "$item"
    fi
  done < <(find "$staged" \( -type d -o -type f \) -print0)
  codesign --verify --deep --strict "$staged"
done
# Stage on each destination filesystem. Retain every old bundle before installing.
for index in "${!bundle_names[@]}"; do
  if [[ -d "${targets[index]}" ]]; then
    backups[index]=$(mktemp -d "${parents[index]}/UFO-backup.XXXXXX")
    mv "${targets[index]}" "${backups[index]}/"
  fi
done
for index in "${!bundle_names[@]}"; do
  staged="${stages[index]}/${bundle_names[index]}"
  mv -n "$staged" "${parents[index]}/"
  [[ ! -e "$staged" ]] || { echo "${bundle_labels[index]} destination changed during install." >&2; exit 1; }
  installed[index]=true
done
for target in "${targets[@]}"; do codesign --verify --deep --strict "$target"; done
success=true
for index in "${!bundle_names[@]}"; do
  [[ -z "${backups[index]}" ]] || printf 'Previous %s preserved in: %s\n' "${bundle_labels[index]}" "${backups[index]}"
  printf 'Installed %s: %s\n' "${bundle_labels[index]}" "${targets[index]}"
done
echo 'Reopen Logic Pro and choose AU Instruments > MP Audio > UFO on a software instrument track.'
echo 'If needed, find UFO in Logic Plug-in Manager and use Reset & Rescan Selection.'
echo 'The VST3 remains available for VST3 hosts; open the standalone app for direct audio/MIDI use.'
if [[ "$packaged" == true ]]; then
  echo 'Record human listening and Logic session observations separately from automated test logs.'
else
  echo 'Run scripts/listen-macos.sh for an output-device audition; human/Logic observations remain separate.'
fi
