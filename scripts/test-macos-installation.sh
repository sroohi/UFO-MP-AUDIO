#!/bin/bash
# Exercise the actual downloaded-package entry point only on a disposable CI Mac.
set -euo pipefail
cd "$(dirname "$0")/.."
[[ "$(uname -s)" == Darwin && "${GITHUB_ACTIONS:-}" == true && "${RUNNER_OS:-}" == macOS ]] || {
  echo 'This installation smoke check requires a disposable GitHub-hosted Mac.' >&2
  exit 1
}
[[ -z "${UFO_INSTALL_ROOT:-}" || "$UFO_INSTALL_ROOT" == "$HOME" ]] || {
  echo 'Native AU installation smoke checks use the runner user Library.' >&2
  exit 1
}
[[ "${RUNNER_TEMP:-}" == /* && -d "$RUNNER_TEMP" ]] || { echo 'Missing absolute RUNNER_TEMP.' >&2; exit 1; }
archive="$PWD/dist/UFO-macOS-universal-unsigned.zip"
logs="$PWD/dist/validation/installation"
mkdir -p "$logs"
[[ -f "$archive" ]] || { echo 'Build the development ZIP before installation validation.' >&2; exit 1; }
names=(UFO.app UFO.vst3 UFO.component)
parents=("$HOME/Applications" "$HOME/Library/Audio/Plug-Ins/VST3" "$HOME/Library/Audio/Plug-Ins/Components")
targets=("${parents[0]}/${names[0]}" "${parents[1]}/${names[1]}" "${parents[2]}/${names[2]}")
identities=("" "" "")
first_identities=("" "" "")
backup_paths=("" "" "")
backup_identities=("" "" "")
created_parents=(false false false)
for index in "${!names[@]}"; do
  [[ ! -e "${targets[index]}" && ! -L "${targets[index]}" ]] || {
    echo "Refusing to replace an existing runner installation: ${targets[index]}" >&2
    exit 1
  }
  for existing_backup in "${parents[index]}"/UFO-backup.*; do
    [[ ! -e "$existing_backup" && ! -L "$existing_backup" ]] || {
      echo "Refusing to change a runner with existing UFO backups: $existing_backup" >&2
      exit 1
    }
  done
  [[ -d "${parents[index]}" ]] || created_parents[index]=true
done
work=$(mktemp -d "$RUNNER_TEMP/ufo-install-smoke.XXXXXX")
completed=false
validation_root="$HOME/Library/MP Audio/UFO/Install-validation"
seen_validation_dirs=("$validation_root/.no-install-validation-directory")
for validation_dir in "$validation_root"/install.*; do
  [[ ! -d "$validation_dir" ]] || seen_validation_dirs+=("$validation_dir")
done
cleanup() {
  status=$?
  trap - EXIT HUP INT TERM
  set +e
  for index in "${!names[@]}"; do
    target="${targets[index]}"
    if [[ -e "$target" || -L "$target" ]]; then
      current=$(stat -f '%d:%i' "$target" 2>/dev/null)
      if [[ ! -L "$target" && -n "${identities[index]}" && "$current" == "${identities[index]}" ]]; then
        rm -rf "$target"
        [[ ! -e "$target" ]] || status=1
      else
        printf 'Preserved changed or unowned destination: %s\n' "$target" >&2
        status=1
      fi
    fi
    backup="${backup_paths[index]}"
    if [[ -n "$backup" && -d "$backup/${names[index]}" ]]; then
      current=$(stat -f '%d:%i' "$backup/${names[index]}" 2>/dev/null)
      if [[ ! -L "$backup" && "$current" == "${first_identities[index]}" && "$(stat -f '%d:%i' "$backup" 2>/dev/null)" == "${backup_identities[index]}" ]]; then
        rm -rf "$backup/${names[index]}"
        rmdir "$backup" || status=1
      else
        printf 'Preserved changed installer backup: %s\n' "$backup" >&2
        status=1
      fi
    fi
    [[ "${created_parents[index]}" != true ]] || rmdir "${parents[index]}" 2>/dev/null || true
  done
  rm -rf "$work"
  if [[ "$status" == 0 && "$completed" == true ]]; then
    echo 'PASS: packaged Install.command, real signatures/architectures/quarantine clearing, three-bundle install/update, Apple auval and owned-install cleanup.' | tee "$logs/result.txt"
  else
    printf 'FAIL: packaged installation smoke check or cleanup failed (status %s).\n' "$status" > "$logs/result.txt"
  fi
  exit "$status"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
unzip -t "$archive" > "$logs/archive-check.txt"
ditto -x -k "$archive" "$work"
package="$work/UFO"
[[ -x "$package/Install.command" ]] || { echo 'The ZIP has no executable Install.command.' >&2; exit 1; }
for name in "${names[@]}"; do
  [[ -x "$package/$name/Contents/MacOS/UFO" ]] || { echo "Incomplete packaged $name." >&2; exit 1; }
  # Simulate a browser-downloaded bundle, including a nested executable attribute.
  xattr -w com.apple.quarantine '0083;00000000;UFO CI;' "$package/$name"
  xattr -w com.apple.quarantine '0083;00000000;UFO CI;' "$package/$name/Contents/MacOS/UFO"
done
record_identities() {
  for index in "${!names[@]}"; do identities[index]=$(stat -f '%d:%i' "${targets[index]}"); done
}
verify_installation() {
  for index in "${!names[@]}"; do
    target="${targets[index]}"
    binary="$target/Contents/MacOS/UFO"
    lipo "$binary" -verify_arch arm64 x86_64
    codesign --verify --deep --strict "$target"
    expected=$(shasum -a 256 "$package/${names[index]}/Contents/MacOS/UFO" | awk '{print $1}')
    actual=$(shasum -a 256 "$binary" | awk '{print $1}')
    [[ "$actual" == "$expected" ]] || { echo "Installed binary differs from package: $target" >&2; exit 1; }
    for item in "$target" "$binary"; do
      if xattr -p com.apple.quarantine "$item" >/dev/null 2>&1; then
        echo "Installed item retained download quarantine: $item" >&2
        exit 1
      fi
    done
    printf '%s SHA256: %s\n' "${names[index]}" "$actual"
  done
}
capture_validation_logs() {
  local phase="$1" require_success="$2" count=0 candidate seen known
  for candidate in "$validation_root"/install.*; do
    [[ -d "$candidate" && ! -L "$candidate" ]] || continue
    known=false
    for seen in "${seen_validation_dirs[@]}"; do [[ "$candidate" != "$seen" ]] || known=true; done
    [[ "$known" != true ]] || continue
    count=$((count+1))
    seen_validation_dirs+=("$candidate")
    ditto "$candidate" "$logs/$phase-validation-$count"
    if [[ "$require_success" == true ]]; then
      awk 'index($0,"AU VALIDATION SUCCEEDED") {found=1} END {exit !found}' "$candidate/auval.log" || {
        echo "Installer did not retain Apple's AU validation success: $candidate" >&2
        exit 1
      }
    fi
  done
  [[ "$require_success" != true || "$count" == 1 ]] || {
    echo "Expected one fresh installer validation log directory for $phase; found $count." >&2
    exit 1
  }
}
if ! "$package/Install.command" 2>&1 | tee "$logs/install.log"; then
  capture_validation_logs install false
  exit 1
fi
record_identities
first_identities=("${identities[@]}")
capture_validation_logs install true
verify_installation > "$logs/installed-bundles.txt" 2>&1
if ! "$package/Install.command" --replace 2>&1 | tee "$logs/update.log"; then
  capture_validation_logs update false
  exit 1
fi
record_identities
for index in "${!names[@]}"; do
  for backup in "${parents[index]}"/UFO-backup.*; do
    [[ -d "$backup/${names[index]}" && ! -L "$backup" ]] || continue
    if [[ "$(stat -f '%d:%i' "$backup/${names[index]}")" == "${first_identities[index]}" ]]; then
      backup_paths[index]="$backup"
      backup_identities[index]=$(stat -f '%d:%i' "$backup")
      break
    fi
  done
  [[ -n "${backup_paths[index]}" ]] || { echo "No retained prior ${names[index]} from --replace." >&2; exit 1; }
done
capture_validation_logs update true
verify_installation > "$logs/updated-bundles.txt" 2>&1
completed=true
