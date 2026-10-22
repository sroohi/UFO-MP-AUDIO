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
registrar_user=$(id -un)
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
refresh_registrar() {
  local phase=$1 result=0 refresh_log="$artifacts/registrar-$1.log"
  registrar_refresh_used=true
  # Apple's killall documents -u as an owner restriction, and the positional
  # process name is exact. Never signal coreaudiod, another user, or all services.
  LC_ALL=C killall -TERM -u "$registrar_user" AudioComponentRegistrar > "$refresh_log" 2>&1 || result=$?
  printf 'Registrar refresh (%s): SIGTERM for user %s / AudioComponentRegistrar; exit %s.\n' "$phase" "$registrar_user" "$result" >> "$artifacts/registration.txt"
  if [[ "$result" == 0 ]]; then return 0; fi
  if [[ "$result" == 1 ]] && awk '/^No matching processes (belonging to you )?were found$/ {found=1} END {exit !found}' "$refresh_log"; then
    printf 'Registrar refresh (%s): no owned service was running; the next AU query starts it.\n' "$phase" >> "$artifacts/registration.txt"
    return 0
  fi
  cat "$refresh_log" >&2
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
  printf 'Registry refresh: owned bundle timestamps and bounded discovery; per-user registrar SIGTERM only if needed. No cache files are deleted.\n'
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
