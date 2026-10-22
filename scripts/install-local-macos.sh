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
command -v auval >/dev/null || { echo 'Apple auval is required to validate the installed Audio Unit.' >&2; exit 1; }
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
stages=("" "" "")
backups=("" "" "")
installed=(false false false)
installed_identity=("" "" "")
success=false
registration_changed=false
auval_pid=''
validation_logs=''
lock="${parents[2]}/.ufo-au-validation.lock"
lock_identity=''
lock_owned=false
stop_auval() {
  [[ -n "$auval_pid" ]] || return 0
  kill -TERM "$auval_pid" 2>/dev/null || true
  local attempt
  for attempt in {1..8}; do
    kill -0 "$auval_pid" 2>/dev/null || break
    sleep .05
  done
  kill -KILL "$auval_pid" 2>/dev/null || true
  wait "$auval_pid" 2>/dev/null || true
  auval_pid=''
}
refresh_registrar() {
  local log_file=$1 result=0 message
  LC_ALL=C killall -TERM -u "$(id -un)" AudioComponentRegistrar > "$log_file" 2>&1 || result=$?
  [[ "$result" != 0 ]] || return 0
  message=$(cat "$log_file")
  if [[ "$result" == 1 && ( "$message" == 'No matching processes were found' || "$message" == 'No matching processes belonging to you were found' ) ]]; then
    return 0
  fi
  cat "$log_file" >&2
  return 1
}
cleanup() {
  local status=$? index current preserve_au=false recovery_failed=false
  trap - EXIT HUP INT TERM
  set +e
  stop_auval
  if [[ "$success" != true ]]; then
    for ((index=${#bundle_names[@]}-1; index>=0; --index)); do
      if [[ "${installed[index]}" == true && ( -e "${targets[index]}" || -L "${targets[index]}" ) ]]; then
        current=$(stat -f '%d:%i' "${targets[index]}" 2>/dev/null)
        if [[ ! -L "${targets[index]}" && -n "${installed_identity[index]}" && "$current" == "${installed_identity[index]}" ]]; then
          rm -rf "${targets[index]}"
          [[ ! -e "${targets[index]}" ]] || recovery_failed=true
        else
          echo "Installation target changed; preserving ${targets[index]} and its recovery backup." >&2
          [[ "$index" != 2 ]] || preserve_au=true
          recovery_failed=true
        fi
      fi
      if [[ -n "${backups[index]}" && -d "${backups[index]}/${bundle_names[index]}" ]]; then
        if [[ ! -e "${targets[index]}" && ! -L "${targets[index]}" ]]; then
          mv -n "${backups[index]}/${bundle_names[index]}" "${parents[index]}/"
        fi
        if [[ -e "${backups[index]}/${bundle_names[index]}" ]]; then
          echo "Previous ${bundle_labels[index]} remains recoverable at ${backups[index]}." >&2
          [[ "$index" != 2 ]] || preserve_au=true
          recovery_failed=true
        fi
      fi
    done
    if [[ "$registration_changed" == true && "$preserve_au" != true ]]; then
      if [[ -d "${targets[2]}" ]]; then
        touch "${targets[2]}" || recovery_failed=true
        if [[ -f "${targets[2]}/Contents/Info.plist" && ! -L "${targets[2]}/Contents/Info.plist" ]]; then
          touch "${targets[2]}/Contents/Info.plist" || recovery_failed=true
        fi
      fi
      refresh_registrar "$validation_logs/rollback-registrar.log" || recovery_failed=true
    fi
  fi
  for index in "${!bundle_names[@]}"; do
    [[ -z "${stages[index]}" ]] || rm -rf "${stages[index]}"
  done
  if [[ "$lock_owned" == true ]]; then
    current=$(stat -f '%d:%i' "$lock" 2>/dev/null)
    if [[ ! -L "$lock" && "$current" == "$lock_identity" ]]; then
      rmdir "$lock" || recovery_failed=true
    else
      echo "The install lock changed; leaving it untouched at $lock." >&2
      recovery_failed=true
    fi
  fi
  [[ "$recovery_failed" != true ]] || status=1
  exit "$status"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir "$lock" 2>/dev/null || { echo "Another UFO install or validation owns $lock; no bundle was changed." >&2; exit 1; }
lock_owned=true
lock_identity=$(stat -f '%d:%i' "$lock")
for target in "${targets[@]}"; do
  if [[ -e "$target" || -L "$target" ]]; then
    [[ "$replace" == true ]] || { echo "Already exists: $target. Use --replace to retain a backup and install this build." >&2; exit 1; }
    [[ -d "$target" && ! -L "$target" ]] || { echo "Refusing to replace a non-bundle or symlink: $target" >&2; exit 1; }
  fi
done
validation_parent="$install_root/Library/MP Audio/UFO/Install-validation"
mkdir -p "$validation_parent"
validation_logs=$(mktemp -d "$validation_parent/install.XXXXXX")
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
  installed_identity[index]=$(stat -f '%d:%i' "${targets[index]}")
done
for target in "${targets[@]}"; do codesign --verify --deep --strict "$target"; done
# Bound Apple validation without requiring Python or a non-system timeout tool.
run_auval() {
  local budget=$1 log_file=$2 deadline result=0
  shift 2
  deadline=$((SECONDS+budget))
  auval "$@" > "$log_file" 2>&1 &
  auval_pid=$!
  while (( SECONDS < deadline )); do
    if ! kill -0 "$auval_pid" 2>/dev/null; then
      wait "$auval_pid" || result=$?
      auval_pid=''
      return "$result"
    fi
    sleep .25
  done
  stop_auval
  printf 'Apple auval exceeded its %s-second time limit.\n' "$budget" >> "$log_file"
  return 124
}
discover_au() {
  local deadline=$((SECONDS+15)) remaining budget result
  while (( SECONDS < deadline )); do
    remaining=$((deadline-SECONDS));budget=$remaining
    (( budget<=5 )) || budget=5
    result=0
    run_auval "$budget" "$validation_logs/discovery.log" -a || result=$?
    [[ "$result" == 0 ]] || return 1
    if awk '$1=="aumu" && $2=="Ufo1" && $3=="MpAu" {found=1} END {exit !found}' "$validation_logs/discovery.log"; then
      return 0
    fi
    sleep .25
  done
  return 1
}
registration_changed=true
# ditto preserves build timestamps; update only our installed copy for discovery.
[[ -f "${targets[2]}/Contents/Info.plist" && ! -L "${targets[2]}/Contents/Info.plist" ]] || { echo 'The installed AU has no regular Info.plist; restoring previous bundles.' >&2; exit 1; }
touch "${targets[2]}" "${targets[2]}/Contents/Info.plist"
codesign --verify --deep --strict "${targets[2]}"
echo 'Checking the installed Audio Unit with Apple auval...'
if ! discover_au; then
  cp "$validation_logs/discovery.log" "$validation_logs/discovery-before-refresh.log"
  echo 'Refreshing the current user Audio Unit registrar and retrying discovery...'
  refresh_registrar "$validation_logs/registrar-refresh.log" || { echo 'Audio Unit registrar refresh failed; restoring previous bundles.' >&2; exit 1; }
  discover_au || { echo "The installed UFO Audio Unit was not discovered; see $validation_logs." >&2; exit 1; }
fi
if ! run_auval 90 "$validation_logs/auval.log" -v aumu Ufo1 MpAu; then
  cat "$validation_logs/auval.log" >&2
  echo "Audio Unit validation failed; restoring previous bundles. Logs: $validation_logs" >&2
  exit 1
fi
grep -q 'AU VALIDATION SUCCEEDED' "$validation_logs/auval.log" || { echo "Apple auval did not report success; see $validation_logs." >&2; exit 1; }
success=true
for index in "${!bundle_names[@]}"; do
  [[ -z "${backups[index]}" ]] || printf 'Previous %s preserved in: %s\n' "${bundle_labels[index]}" "${backups[index]}"
  printf 'Installed %s: %s\n' "${bundle_labels[index]}" "${targets[index]}"
done
printf 'Apple Audio Unit validation passed. Logs: %s\n' "$validation_logs"
echo 'Reopen Logic Pro and choose AU Instruments > MP Audio > UFO on a software instrument track.'
echo 'If needed, find UFO in Logic Plug-in Manager and use Reset & Rescan Selection.'
echo 'The VST3 remains available for VST3 hosts; open the standalone app for direct audio/MIDI use.'
if [[ "$packaged" == true ]]; then
  echo 'Record human listening and Logic session observations separately from automated test logs.'
else
  echo 'Run scripts/listen-macos.sh --au for an output-device audition; human/Logic observations remain separate.'
fi
