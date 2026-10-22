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
cleanup() {
  status=$?
  trap - EXIT HUP INT TERM
  set +e
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
codesign --verify --deep --strict "$target"
{
  printf 'Temporary user registration: %s\n' "$target"
  printf 'Native architecture: '; uname -m
  printf 'Validated binary SHA256: '; shasum -a 256 "$target/Contents/MacOS/UFO" | awk '{print $1}'
  printf 'Previous user AU preserved during test: %s\n' "$([[ -n "$backup" ]] && echo yes || echo no)"
  printf 'Registry refresh: filesystem change and bounded discovery; no daemon kills or cache deletion.\n'
} > "$artifacts/registration.txt"
# AudioComponentRegistrar observes the directory change. Poll discovery without
# terminating a shared audio service or deleting unrelated plug-in caches.
discovered=false
for attempt in {1..60}; do
  if auval -a > "$artifacts/discovery.log" 2>&1 && awk '$1=="aumu" && $2=="Ufo1" && $3=="MpAu" {found=1} END {exit !found}' "$artifacts/discovery.log"; then
    discovered=true
    break
  fi
  sleep .25
done
[[ "$discovered" == true ]] || { echo 'The temporary UFO Audio Unit was not discovered.' >&2; cat "$artifacts/discovery.log" >&2; exit 1; }
auval -v aumu Ufo1 MpAu 2>&1 | tee "$artifacts/auval.log"
"$host" "$target" "$artifacts" --au 2>&1 | tee "$artifacts/au-host.log"
validation_completed=true
