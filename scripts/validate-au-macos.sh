#!/bin/bash
# Register only our staged AU for native validation, then restore the prior copy.
set -euo pipefail
[[ "$(uname -s)" == Darwin ]] || { echo 'Native Audio Unit validation requires macOS.' >&2; exit 1; }
[[ $# == 3 ]] || { echo 'Usage: validate-au-macos.sh <UFO.component> <native-host-test> <artifact-directory>' >&2; exit 1; }
component=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
host=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
mkdir -p "$3"
artifacts=$(cd "$3" && pwd)
[[ -d "$component" && ! -L "$component" && -x "$component/Contents/MacOS/UFO" && -x "$host" ]] || { echo 'Missing staged AU or native AU host test.' >&2; exit 1; }
for logic_process in 'Logic Pro' 'Logic Pro X'; do
  if pgrep -x -u "$(id -u)" "$logic_process" >/dev/null; then
    echo 'Quit Logic Pro before temporary Audio Unit registration so existing sessions stay under your control.' >&2
    exit 1
  fi
done
command -v auval >/dev/null || { echo 'Apple auval is required.' >&2; exit 1; }
discovery_seconds=${UFO_AU_DISCOVERY_SECONDS:-15}
validation_seconds=${UFO_AU_VALIDATION_SECONDS:-90}
[[ "$discovery_seconds" =~ ^([1-9]|[1-5][0-9]|60)$ ]] || { echo 'AU discovery budget must be 1-60 seconds.' >&2; exit 1; }
[[ "$validation_seconds" =~ ^[1-9][0-9]*$ && ${#validation_seconds} -le 3 ]] && (( validation_seconds <= 600 )) || { echo 'AU validation budget must be 1-600 seconds.' >&2; exit 1; }
codesign --verify --deep --strict "$component"
# The override supports isolated transaction tests; native AU discovery uses the
# normal user Library, which is always the default.
validation_root=${UFO_INSTALL_ROOT:-$HOME}
[[ "$validation_root" == /* ]] || { echo 'UFO_INSTALL_ROOT must be absolute.' >&2; exit 1; }
components="$validation_root/Library/Audio/Plug-Ins/Components"
mkdir -p "$components"
target="$components/UFO.component"
lock="$components/.ufo-au-validation.lock"
mkdir "$lock" 2>/dev/null || { echo "Another UFO AU validation owns $lock; no component was changed." >&2; exit 1; }
work=''
installed=false
installed_identity=''
backup=''
validation_completed=false
registrar_refresh_used=false
validator_pid=''
stop_validator() {
  if [[ -n "$validator_pid" ]]; then
    if kill -0 "$validator_pid" 2>/dev/null; then
      kill -TERM "$validator_pid" 2>/dev/null || true
      sleep .25
      kill -KILL "$validator_pid" 2>/dev/null || true
    fi
    wait "$validator_pid" 2>/dev/null || true
    validator_pid=''
  fi
}
run_auval() {
  local output=$1 budget=$2 deadline result=0
  shift 2
  auval "$@" > "$output" 2>&1 &
  validator_pid=$!
  deadline=$((SECONDS+budget))
  while kill -0 "$validator_pid" 2>/dev/null; do
    if (( SECONDS >= deadline )); then
      stop_validator
      printf '\nApple auval timed out after %s seconds.\n' "$budget" >> "$output"
      return 124
    fi
    sleep .25
  done
  wait "$validator_pid" || result=$?
  validator_pid=''
  return "$result"
}
# Refresh only a captured, current-user Apple AudioComponentRegistrar identity.
# No process-name-wide KILL, cache deletion, sudo, or audio-device service restart.
_ufo_registrar_trusted_path() {
  case "$1" in
    *'/../'*|*'/./'*|*'//'*) return 1 ;;
    /System/Library/Frameworks/AudioToolbox.framework/AudioComponentRegistrar|/System/Library/Frameworks/AudioToolbox.framework/*/AudioComponentRegistrar) return 0 ;;
    *) return 1 ;;
  esac
}
_ufo_registrar_identity() {
  local row=$1 pid uid weekday month day clock year executable
  IFS=$' \t' read -r pid uid weekday month day clock year executable <<< "$row"
  [[ "$pid" =~ ^[1-9][0-9]*$ && "$uid" =~ ^[0-9]+$ && "$weekday" =~ ^[A-Za-z]{3}$ && "$month" =~ ^[A-Za-z]{3}$ && "$day" =~ ^[0-9]{1,2}$ && "$clock" =~ ^[0-9]{2}:[0-9]{2}:[0-9]{2}$ && "$year" =~ ^[0-9]{4}$ && -n "$executable" ]] || return 2
  printf '%s|%s|%s %s %s %s %s|%s' "$pid" "$uid" "$weekday" "$month" "$day" "$clock" "$year" "$executable"
}
_ufo_registrar_snapshot() {
  local pid=$1 log=$2 output result=0 error_file="$2.ps-errors"
  [[ "$pid" =~ ^[1-9][0-9]*$ ]] || return 2
  output=$(LC_ALL=C ps -p "$pid" -o pid=,uid=,lstart=,comm= 2> "$error_file") || result=$?
  if [[ -s "$error_file" ]]; then
    cat "$error_file" >&2
    rm -f "$error_file"
    return 2
  fi
  rm -f "$error_file"
  if [[ -z "$output" && ( "$result" == 0 || "$result" == 1 ) ]]; then return 1; fi
  [[ "$result" == 0 && "$output" != *$'\n'* ]] || return 2
  _ufo_registrar_identity "$output"
}
_ufo_registrar_same_identity() {
  local pid=$1 expected=$2 log=$3 current result
  if current=$(_ufo_registrar_snapshot "$pid" "$log"); then
    if [[ "$current" != "$expected" ]]; then
      printf 'Refusing further signals: PID %s identity changed. Expected %s; observed %s.\n' "$pid" "$expected" "$current"
      return 2
    fi
    return 0
  else
    result=$?
    if [[ "$result" == 1 ]]; then return 1; fi
    printf 'Cannot verify registrar PID %s identity; no further signal is safe.\n' "$pid"
    return 2
  fi
}
_ufo_refresh_owned_registrar() {
  local log=$1 owner_uid rows result=0 error_file="$1.ps-errors" row pid uid weekday month day clock year executable identity current attempt gone
  local candidates=("")
  owner_uid=$(id -u) || return 1
  [[ "$owner_uid" =~ ^[0-9]+$ ]] || return 1
  rows=$(LC_ALL=C ps -axo pid=,uid=,lstart=,comm= 2> "$error_file") || result=$?
  if [[ "$result" != 0 || -s "$error_file" ]]; then
    printf 'Cannot enumerate registrar identities (ps exit %s).\n' "$result"
    cat "$error_file"
    rm -f "$error_file"
    return 1
  fi
  rm -f "$error_file"
  while IFS= read -r row; do
    [[ -n "$row" ]] || continue
    IFS=$' \t' read -r pid uid weekday month day clock year executable <<< "$row"
    [[ "$uid" == "$owner_uid" ]] || continue
    _ufo_registrar_trusted_path "$executable" || continue
    identity=$(_ufo_registrar_identity "$row") || { printf 'Malformed registrar identity; refusing signals.\n'; return 1; }
    candidates[${#candidates[@]}]=$identity
  done <<< "$rows"
  if [[ ${#candidates[@]} == 1 ]]; then
    printf 'No current-user Apple AudioComponentRegistrar was running; the next AU query starts it.\n'
    return 0
  fi
  for identity in "${candidates[@]}"; do
    [[ -n "$identity" ]] || continue
    IFS='|' read -r pid uid current executable <<< "$identity"
    printf 'Captured registrar identity: %s\n' "$identity"
    if _ufo_registrar_same_identity "$pid" "$identity" "$log"; then :; else
      result=$?
      if [[ "$result" == 1 ]]; then printf 'PID %s exited before TERM; no signal sent.\n' "$pid"; continue; fi
      return 1
    fi
    if env kill -TERM "$pid"; then
      printf 'Sent TERM to verified current-user Apple registrar PID %s.\n' "$pid"
    else
      result=$?
      printf 'TERM returned %s for PID %s.\n' "$result" "$pid"
      if _ufo_registrar_same_identity "$pid" "$identity" "$log"; then return 1; else
        result=$?
        if [[ "$result" == 1 ]]; then printf 'PID %s exited concurrently with TERM.\n' "$pid"; continue; fi
        return 1
      fi
    fi
    gone=false
    for attempt in 1 2 3 4; do
      sleep .25
      if _ufo_registrar_same_identity "$pid" "$identity" "$log"; then :; else
        result=$?
        if [[ "$result" == 1 ]]; then gone=true; break; fi
        return 1
      fi
    done
    if [[ "$gone" == true ]]; then printf 'PID %s exited after TERM.\n' "$pid"; continue; fi
    # Revalidate immediately before escalation; never signal a reused or changed PID.
    if _ufo_registrar_same_identity "$pid" "$identity" "$log"; then :; else
      result=$?
      if [[ "$result" == 1 ]]; then printf 'PID %s exited before KILL; no escalation sent.\n' "$pid"; continue; fi
      return 1
    fi
    if env kill -KILL "$pid"; then
      printf 'Sent KILL only to unchanged surviving Apple registrar PID %s.\n' "$pid"
    else
      result=$?
      printf 'KILL returned %s for PID %s.\n' "$result" "$pid"
      if _ufo_registrar_same_identity "$pid" "$identity" "$log"; then return 1; else
        result=$?
        if [[ "$result" == 1 ]]; then printf 'PID %s exited concurrently with KILL.\n' "$pid"; continue; fi
        return 1
      fi
    fi
    for attempt in 1 2 3 4 5 6 7 8; do
      sleep .25
      if _ufo_registrar_same_identity "$pid" "$identity" "$log"; then :; else
        result=$?
        if [[ "$result" == 1 ]]; then gone=true; break; fi
        return 1
      fi
    done
    if [[ "$gone" != true ]]; then printf 'Verified registrar PID %s did not exit after KILL.\n' "$pid"; return 1; fi
    printf 'PID %s is absent after verified KILL.\n' "$pid"
  done
}
refresh_owned_registrar() {
  local log=$1
  : > "$log"
  _ufo_refresh_owned_registrar "$log" >> "$log" 2>&1
}

refresh_registrar() {
  local phase=$1 result=0 refresh_log="$artifacts/registrar-$1.log"
  registrar_refresh_used=true
  refresh_owned_registrar "$refresh_log" || result=$?
  printf 'Registrar refresh (%s): verified current-user Apple PID refresh; exit %s.\n' "$phase" "$result" >> "$artifacts/registration.txt"
  if [[ "$result" != 0 ]]; then cat "$refresh_log" >&2; fi
  return "$result"
}
discover_component() {
  local deadline=$((SECONDS+discovery_seconds)) remaining budget
  while (( SECONDS < deadline )); do
    remaining=$((deadline-SECONDS))
    budget=$remaining
    (( budget <= 5 )) || budget=5
    if run_auval "$artifacts/discovery.log" "$budget" -a && awk '$1=="aumu" && $2=="Ufo1" && $3=="MpAu" {found=1} END {exit !found}' "$artifacts/discovery.log"; then
      return 0
    fi
    sleep .25
  done
  return 1
}
cleanup() {
  status=$?
  trap - EXIT HUP INT TERM
  set +e
  stop_validator
  preserve=false
  if [[ "$installed" == true && ( -e "$target" || -L "$target" ) ]]; then
    current_identity=$(stat -f '%d:%i' "$target" 2>/dev/null)
    if [[ ! -L "$target" && -n "$installed_identity" && "$current_identity" == "$installed_identity" ]]; then
      rm -rf "$target"
      [[ ! -e "$target" ]] || { preserve=true; status=1; }
    else
      echo "AU registration changed during validation; preserving it and recovery files in $work." >&2
      preserve=true
      status=1
    fi
  fi
  if [[ -n "$backup" && -d "$backup" ]]; then
    if [[ ! -e "$target" && ! -L "$target" ]]; then
      mv "$backup" "$target" || { preserve=true; status=1; }
    else
      echo "Previous AU remains recoverable at $backup." >&2
      preserve=true
      status=1
    fi
  fi
  if [[ "$registrar_refresh_used" == true ]]; then
    refresh_registrar restored || status=1
  fi
  [[ "$preserve" == true || -z "$work" ]] || rm -rf "$work"
  rmdir "$lock" 2>/dev/null || status=1
  if [[ "$status" == 0 && "$validation_completed" == true ]]; then
    printf 'PASS: Apple auval and actual AU wrapper hosting; temporary registration restored. Logic UI and human listening remain unrun.\n' | tee "$artifacts/result.txt"
  else
    printf 'FAIL: AU validation or registration cleanup did not complete (status %s).\n' "$status" > "$artifacts/result.txt"
  fi
  exit "$status"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
if [[ -e "$target" || -L "$target" ]]; then
  [[ -d "$target" && ! -L "$target" ]] || { echo "Refusing to replace a non-component or symlink: $target" >&2; exit 1; }
fi
work=$(mktemp -d "$components/.ufo-au-validation.XXXXXX")
ditto "$component" "$work/UFO.component"
codesign --verify --deep --strict "$work/UFO.component"
if [[ -d "$target" ]]; then
  backup="$work/previous-component"
  mv "$target" "$backup"
fi
mv -n "$work/UFO.component" "$components/"
[[ ! -e "$work/UFO.component" ]] || { echo 'AU registration destination changed during validation.' >&2; exit 1; }
installed=true
installed_identity=$(stat -f '%d:%i' "$target")
# ditto retains old build times. Make only this owned copy visibly newer than
# an earlier registrar scan; changing timestamps does not alter signed contents.
touch "$target" "$target/Contents/Info.plist"
codesign --verify --deep --strict "$target"
{
  printf 'Temporary user registration: %s\n' "$target"
  printf 'Native architecture: '; uname -m
  printf 'Validated binary SHA256: '; shasum -a 256 "$target/Contents/MacOS/UFO" | awk '{print $1}'
  printf 'Previous user AU preserved during test: %s\n' "$([[ -n "$backup" ]] && echo yes || echo no)"
  printf 'Registry refresh: owned bundle timestamps and bounded discovery; verified current-user Apple registrar TERM, then identity-checked KILL only if needed. No cache files are deleted.\n'
} > "$artifacts/registration.txt"
if ! discover_component; then
  cp "$artifacts/discovery.log" "$artifacts/discovery-before-refresh.log"
  refresh_registrar temporary
  discover_component || { echo 'The temporary UFO Audio Unit was not discovered after a scoped registrar refresh.' >&2; cat "$artifacts/discovery.log" >&2; exit 1; }
fi
printf 'Discovered registered aumu/Ufo1/MpAu before validation.\n' >> "$artifacts/registration.txt"
if run_auval "$artifacts/auval.log" "$validation_seconds" -v aumu Ufo1 MpAu; then
  cat "$artifacts/auval.log"
else
  result=$?
  cat "$artifacts/auval.log" >&2
  exit "$result"
fi
awk '/AU VALIDATION SUCCEEDED/ {found=1} END {exit !found}' "$artifacts/auval.log" || { echo 'Apple auval did not report validation success.' >&2; exit 1; }
"$host" "$target" "$artifacts" --au 2>&1 | tee "$artifacts/au-host.log"
validation_completed=true
