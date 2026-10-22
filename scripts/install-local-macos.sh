#!/bin/bash
set -euo pipefail
script_dir=$(cd "$(dirname "$0")" && pwd)
[[ "$(uname -s)" == Darwin ]] || { echo 'macOS required' >&2; exit 1; }
replace=false
case "${1:-}" in '' ) ;; --replace) replace=true ;; *) printf 'Usage: %s [--replace]\n' "$(basename "$0")" >&2; exit 1 ;; esac
packaged=false
if [[ -d "$script_dir/UFO.app" && -d "$script_dir/UFO.vst3" ]]; then
  package_dir="$script_dir"
  packaged=true
else
  package_dir="$script_dir/../dist/UFO"
fi
if pgrep -x -u "$(id -u)" UFO >/dev/null; then
  echo 'Quit the standalone UFO app before installing so unsaved patches stay under your control.' >&2
  exit 1
fi
app_source="$package_dir/UFO.app"
plugin_source="$package_dir/UFO.vst3"
native_arch=$(uname -m)
for source in "$app_source" "$plugin_source"; do
  [[ -d "$source" && -x "$source/Contents/MacOS/UFO" ]] || { echo "Missing $source; run scripts/build-macos.sh first." >&2; exit 1; }
  lipo -verify_arch "$native_arch" "$source/Contents/MacOS/UFO"
  codesign --verify --deep --strict "$source"
done
install_root=${UFO_INSTALL_ROOT:-$HOME}
[[ "$install_root" == /* ]] || { echo 'UFO_INSTALL_ROOT must be an absolute path.' >&2; exit 1; }
app_parent="$install_root/Applications"
plugin_parent="$install_root/Library/Audio/Plug-Ins/VST3"
app_target="$app_parent/UFO.app"
plugin_target="$plugin_parent/UFO.vst3"
mkdir -p "$app_parent" "$plugin_parent"
for target in "$app_target" "$plugin_target"; do
  if [[ -e "$target" || -L "$target" ]]; then
    [[ "$replace" == true ]] || { echo "Already exists: $target. Use --replace to retain a backup and install this build." >&2; exit 1; }
    [[ -d "$target" && ! -L "$target" ]] || { echo "Refusing to replace a non-bundle or symlink: $target" >&2; exit 1; }
  fi
done
app_stage=$(mktemp -d "$app_parent/.ufo-install.XXXXXX")
plugin_stage=''
app_backup=''
plugin_backup=''
installed_app=false
installed_plugin=false
success=false
cleanup() {
  if [[ "$success" != true ]]; then
    [[ "$installed_plugin" != true ]] || rm -rf "$plugin_target"
    [[ "$installed_app" != true ]] || rm -rf "$app_target"
    if [[ -n "$plugin_backup" && -d "$plugin_backup/UFO.vst3" ]]; then mv "$plugin_backup/UFO.vst3" "$plugin_parent/"; fi
    if [[ -n "$app_backup" && -d "$app_backup/UFO.app" ]]; then mv "$app_backup/UFO.app" "$app_parent/"; fi
  fi
  rm -rf "$app_stage"
  [[ -z "$plugin_stage" ]] || rm -rf "$plugin_stage"
}
trap cleanup EXIT
plugin_stage=$(mktemp -d "$plugin_parent/.ufo-install.XXXXXX")
ditto "$app_source" "$app_stage/UFO.app"
ditto "$plugin_source" "$plugin_stage/UFO.vst3"
for staged in "$app_stage/UFO.app" "$plugin_stage/UFO.vst3"; do
  codesign --verify --deep --strict "$staged"
  # The user requested this local development install. Clear browser quarantine
  # only on our own verified copies, including nested files; skip symlinks.
  while IFS= read -r -d '' item; do
    if xattr -p com.apple.quarantine "$item" >/dev/null 2>&1; then
      xattr -d com.apple.quarantine "$item"
    fi
  done < <(find "$staged" \( -type d -o -type f \) -print0)
  codesign --verify --deep --strict "$staged"
done
# Stage in each destination filesystem so final bundle renames are atomic.
# Existing bundles move into recoverable backups before either new bundle lands.
if [[ -d "$app_target" ]]; then
  app_backup=$(mktemp -d "$app_parent/UFO-backup.XXXXXX")
  mv "$app_target" "$app_backup/"
fi
if [[ -d "$plugin_target" ]]; then
  plugin_backup=$(mktemp -d "$plugin_parent/UFO-backup.XXXXXX")
  mv "$plugin_target" "$plugin_backup/"
fi
mv -n "$app_stage/UFO.app" "$app_parent/"
[[ ! -e "$app_stage/UFO.app" ]] || { echo 'Application destination changed during install.' >&2; exit 1; }
installed_app=true
mv -n "$plugin_stage/UFO.vst3" "$plugin_parent/"
[[ ! -e "$plugin_stage/UFO.vst3" ]] || { echo 'Plugin destination changed during install.' >&2; exit 1; }
installed_plugin=true
codesign --verify --deep --strict "$app_target"
codesign --verify --deep --strict "$plugin_target"
success=true
[[ -z "$app_backup" ]] || printf 'Previous app preserved in: %s\n' "$app_backup"
[[ -z "$plugin_backup" ]] || printf 'Previous VST3 preserved in: %s\n' "$plugin_backup"
printf 'Installed app: %s\nInstalled VST3: %s\n' "$app_target" "$plugin_target"
echo 'Open the app or rescan VST3 instruments in your DAW.'
if [[ "$packaged" == true ]]; then
  echo 'Audition the installed app and VST3 in your DAW; record human listening observations separately from automated test logs.'
else
  echo 'Run scripts/listen-macos.sh for an output-device audition; human/DAW observations remain separate.'
fi
