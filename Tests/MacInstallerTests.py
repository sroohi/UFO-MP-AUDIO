#!/usr/bin/env python3
"""Exercise bundle transactions with mocked native tools, not native Mac validation."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import shlex
import sys
import time

REPOSITORY = Path(__file__).resolve().parents[1]
BUNDLES = (
    ("UFO.app", "Applications", "app"),
    ("UFO.vst3", "Library/Audio/Plug-Ins/VST3", "plugin"),
    ("UFO.component", "Library/Audio/Plug-Ins/Components", "au"),
)

with tempfile.TemporaryDirectory(prefix="ufo-installer-test-") as temporary:
    root = Path(temporary)
    project = root / "project"
    (project / "scripts").mkdir(parents=True)
    shutil.copy2(REPOSITORY / "scripts/install-local-macos.sh", project / "scripts/install-local-macos.sh")
    for name, _, label in BUNDLES:
        binary = project / "dist/UFO" / name / "Contents/MacOS/UFO"
        binary.parent.mkdir(parents=True)
        binary.write_text("new-" + label)
        binary.chmod(0o700)
        (binary.parents[1] / "Info.plist").write_text("fixture metadata")
    tools = root / "tools"
    tools.mkdir()
    mocks = {
        "uname": 'if [ "$1" = "-s" ]; then echo Darwin; else echo x86_64; fi\n',
        "pgrep": '''for arg do process="$arg"; done
if [ "${UFO_TEST_RUNNING_LOGIC:-}" = 1 ] && [ "$process" = "Logic Pro" ]; then exit 0; fi
exit 1
''',
        "lipo": '[ "$#" -eq 3 ] && [ -f "$1" ] && [ "$2" = "-verify_arch" ] && [ "$3" = x86_64 ]\n',
        "stat": "exec " + shlex.quote(sys.executable) + """ -c 'import os,sys; s=os.lstat(sys.argv[1]); print(str(s.st_dev)+":"+str(s.st_ino))' "$3"\n""",
        "auval": '''component="$UFO_INSTALL_ROOT/Library/Audio/Plug-Ins/Components/UFO.component"
[ -f "$component/Contents/MacOS/UFO" ] || exit 20
mode="${UFO_TEST_AU_MODE:-ready}"
if [ "$#" = 1 ] && [ "$1" = -a ]; then
  case "$mode" in
    stale|refresh_absent|refresh_error)
      [ -f "$UFO_TEST_REGISTRAR_STATE" ] || exit 21 ;;
    missing) exit 22 ;;
  esac
  echo 'aumu Ufo1 MpAu MP Audio: UFO'
  exit 0
fi
[ "$#" = 4 ] && [ "$1" = -v ] && [ "$2" = aumu ] && [ "$3" = Ufo1 ] && [ "$4" = MpAu ] || exit 23
case "$mode" in
  validation_error) echo 'AU VALIDATION FAILED'; exit 24 ;;
  invalid_output) echo 'No validation was performed'; exit 0 ;;
  hanging) echo "$$" > "$UFO_TEST_AU_PID_LOG"; exec sleep 20 ;;
  external_replacement)
    mv "$component" "$component.external-held"
    mkdir -p "$component/Contents/MacOS"
    echo replacement-au > "$component/Contents/MacOS/UFO"
    exit 25 ;;
esac
echo 'AU VALIDATION SUCCEEDED.'
''',
        "killall": '''[ "$#" = 4 ] && [ "$1" = -TERM ] && [ "$2" = -u ] && [ "$3" = "$(id -un)" ] && [ "$4" = AudioComponentRegistrar ] || exit 30
printf '%s\\n' "$@" >> "$UFO_TEST_REFRESH_LOG"
[ "${UFO_TEST_AU_MODE:-}" != refresh_error ] || { echo 'Operation not permitted' >&2; exit 31; }
touch "$UFO_TEST_REGISTRAR_STATE"
if [ "${UFO_TEST_AU_MODE:-}" = refresh_absent ]; then
  echo 'No matching processes belonging to you were found' >&2
  exit 1
fi
''',
        "codesign": '''for arg do target="$arg"; done
if [ "${UFO_TEST_FAIL_TARGET:-}" = "$target" ]; then exit 7; fi
exit 0
''',
        "ditto": 'cp -a "$1" "$2"\n',
        "xattr": '''if [ "$1" = "-p" ]; then
  [ "${UFO_TEST_QUARANTINE:-}" = 1 ]; exit "$?"
fi
[ "${UFO_TEST_XATTR_FAIL:-}" != 1 ] || exit 8
printf '%s\\n' "$3" >> "$UFO_TEST_XATTR_LOG"
''',
    }
    for name, body in mocks.items():
        command = tools / name
        command.write_text("#!/bin/sh\n" + body)
        command.chmod(0o700)
    destination = root / "installation"
    environment = os.environ.copy()
    environment["PATH"] = str(tools) + os.pathsep + environment["PATH"]
    environment["UFO_INSTALL_ROOT"] = str(destination)
    current_user = subprocess.check_output(["id", "-un"], text=True).strip()
    binaries = [destination / parent / name / "Contents/MacOS/UFO" for name, parent, _ in BUNDLES]

    invocation_count = 0

    def invocation_environment(au_mode="ready"):
        global invocation_count
        invocation_count += 1
        evidence = root / ("invocation-" + str(invocation_count))
        evidence.mkdir()
        result = environment.copy()
        result["UFO_TEST_AU_MODE"] = au_mode
        result["UFO_TEST_REGISTRAR_STATE"] = str(evidence / "registrar-refreshed")
        result["UFO_TEST_REFRESH_LOG"] = str(evidence / "refresh.log")
        result["UFO_TEST_AU_PID_LOG"] = str(evidence / "auval.pid")
        return result, evidence

    def install(*arguments, fail_target=None, fail_quarantine=False, running_logic=False, au_mode="ready"):
        command_environment, evidence = invocation_environment(au_mode)
        if fail_target:
            command_environment["UFO_TEST_FAIL_TARGET"] = str(fail_target)
        if fail_quarantine:
            command_environment["UFO_TEST_QUARANTINE"] = "1"
            command_environment["UFO_TEST_XATTR_FAIL"] = "1"
        if running_logic:
            command_environment["UFO_TEST_RUNNING_LOGIC"] = "1"
        result = subprocess.run(["bash", str(project / "scripts/install-local-macos.sh"), *arguments],
                                env=command_environment, text=True, capture_output=True)
        result.evidence = evidence
        return result

    def expect_versions(prefix):
        for binary, (_, _, label) in zip(binaries, BUNDLES):
            assert binary.read_text() == prefix + "-" + label, str(binary)

    blocked = install(running_logic=True)
    assert blocked.returncode != 0 and "Quit Logic Pro" in blocked.stderr
    assert not destination.exists(), "Installation touched destinations while Logic was running"

    # A package missing the AU is rejected before any part of this installation lands.
    au_source = project / "dist/UFO/UFO.component"
    held_au = project / "dist/UFO/AU-held"
    au_source.rename(held_au)
    missing = install()
    assert missing.returncode != 0 and "UFO.component" in missing.stderr
    assert not destination.exists(), "Missing AU permitted a partial installation"
    held_au.rename(au_source)

    first = install()
    assert first.returncode == 0, first.stderr
    expect_versions("new")
    assert not (first.evidence / "refresh.log").exists(), "Normal discovery unnecessarily refreshed the registrar"
    assert "Apple Audio Unit validation passed. Logs:" in first.stdout
    refused = install()
    assert refused.returncode != 0 and "Already exists" in refused.stderr
    expect_versions("new")

    for binary, (_, _, label) in zip(binaries, BUNDLES):
        binary.write_text("previous-" + label)
    replaced = install("--replace")
    assert replaced.returncode == 0, replaced.stderr
    for name, parent, label in BUNDLES:
        backups = list((destination / parent).glob("UFO-backup.*/" + name + "/Contents/MacOS/UFO"))
        assert any(path.read_text() == "previous-" + label for path in backups), "Missing retained backup: " + name
    expect_versions("new")

    for binary, (_, _, label) in zip(binaries, BUNDLES):
        binary.write_text("stable-" + label)
    # Fail final validation only after all three new bundles have reached their targets.
    failed = install("--replace", fail_target=binaries[2].parents[2])
    assert failed.returncode != 0, "Injected AU final-validation failure was not reached"
    expect_versions("stable")
    failed_quarantine = install("--replace", fail_quarantine=True)
    assert failed_quarantine.returncode != 0, "Injected quarantine-clear failure was not reached"
    expect_versions("stable")
    for _, parent, _ in BUNDLES:
        assert not list((destination / parent).glob(".ufo-install.*")), "Staging directory survived rollback"

    lock = destination / BUNDLES[2][1] / ".ufo-au-validation.lock"
    lock.mkdir()
    locked = install("--replace")
    assert locked.returncode != 0 and "Another UFO install or validation" in locked.stderr
    assert lock.is_dir(), "Installer removed another transaction's lock"
    expect_versions("stable")
    lock.rmdir()

    for mode in ("stale", "refresh_absent"):
        refreshed = install("--replace", au_mode=mode)
        assert refreshed.returncode == 0, refreshed.stderr
        expect_versions("new")
        calls = (refreshed.evidence / "refresh.log").read_text().splitlines()
        assert calls == ["-TERM", "-u", current_user, "AudioComponentRegistrar"]
        assert not lock.exists(), "Successful refresh left a transaction lock"
        for binary, (_, _, label) in zip(binaries, BUNDLES):
            binary.write_text("stable-" + label)

    for mode in ("missing", "refresh_error", "validation_error", "invalid_output"):
        invalid = install("--replace", au_mode=mode)
        assert invalid.returncode != 0, "Invalid AU discovery/validation was accepted: " + mode
        expect_versions("stable")
        assert not lock.exists(), "Failed validation left a transaction lock"
        assert (invalid.evidence / "refresh.log").exists(), "Rollback did not refresh restored AU metadata"

    # A signal while auval is running must stop the owned child and restore all bundles.
    interrupted_environment, interrupted_evidence = invocation_environment("hanging")
    process = subprocess.Popen(["bash", str(project / "scripts/install-local-macos.sh"), "--replace"],
                               env=interrupted_environment, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    deadline = time.monotonic() + 10
    pid_log = interrupted_evidence / "auval.pid"
    while not pid_log.exists() and process.poll() is None and time.monotonic() < deadline:
        time.sleep(0.02)
    assert pid_log.exists(), "Interrupted-install fixture never reached native validation"
    validator_pid = int(pid_log.read_text())
    process.terminate()
    stdout, stderr = process.communicate(timeout=10)
    assert process.returncode != 0, stdout + stderr
    expect_versions("stable")
    assert not lock.exists(), "Interrupted installation left a lock"
    try:
        os.kill(validator_pid, 0)
    except ProcessLookupError:
        pass
    else:
        raise AssertionError("Interrupted installation left its auval process running")

    # Preserve an unrelated replacement and the old AU recovery backup on rollback.
    backup_pattern = "UFO-backup.*/UFO.component/Contents/MacOS/UFO"
    old_recoveries = set((destination / BUNDLES[2][1]).glob(backup_pattern))
    changed = install("--replace", au_mode="external_replacement")
    assert changed.returncode != 0 and "target changed" in changed.stderr
    assert binaries[2].read_text().strip() == "replacement-au"
    recovery = set((destination / BUNDLES[2][1]).glob(backup_pattern)) - old_recoveries
    recoverable = [p for p in recovery if p.read_text() == "stable-au"]
    assert len(recoverable) == 1, "Previous AU was not preserved for recovery"
    au_path = binaries[2].parents[2]
    shutil.rmtree(au_path)
    shutil.rmtree(au_path.with_name("UFO.component.external-held"))
    recoverable[0].parents[2].rename(au_path)
    expect_versions("stable")

    # Never replace a component symlink or mutate its referent, even with --replace.
    au_target = binaries[2].parents[2]
    held_target = au_target.with_name("UFO-held.component")
    au_target.rename(held_target)
    outside = root / "external-component"
    outside.mkdir()
    marker = outside / "unchanged"
    marker.write_text("owned elsewhere")
    au_target.symlink_to(outside, target_is_directory=True)
    symlink_refusal = install("--replace")
    assert symlink_refusal.returncode != 0 and "symlink" in symlink_refusal.stderr
    assert marker.read_text() == "owned elsewhere" and au_target.is_symlink()
    au_target.unlink()
    held_target.rename(au_target)
    expect_versions("stable")

    package = root / "download/UFO"
    shutil.copytree(project / "dist/UFO", package)
    shutil.copy2(project / "scripts/install-local-macos.sh", package / "Install.command")
    package_destination = root / "package-installation"
    package_environment, _ = invocation_environment()
    package_environment["UFO_INSTALL_ROOT"] = str(package_destination)
    quarantine_log = root / "quarantine-cleared.txt"
    package_environment["UFO_TEST_QUARANTINE"] = "1"
    package_environment["UFO_TEST_XATTR_LOG"] = str(quarantine_log)
    packaged = subprocess.run(["bash", str(package / "Install.command")], cwd=root,
                              env=package_environment, text=True, capture_output=True)
    assert packaged.returncode == 0, packaged.stderr
    for name, parent, label in BUNDLES:
        assert (package_destination / parent / name / "Contents/MacOS/UFO").read_text() == "new-" + label
    assert "AU Instruments > MP Audio > UFO" in packaged.stdout
    cleared_paths = quarantine_log.read_text().splitlines()
    assert cleared_paths and all(path.startswith(str(package_destination)) and "/.ufo-install." in path
                                 for path in cleared_paths), "Quarantine clearing escaped owned staging directories"
    assert any("/UFO.component" in path for path in cleared_paths), "AU quarantine was never checked"

print("PASS: mocked app/VST3/AU installation, scoped registrar refresh, AU validation failure/interruption rollback, lock/ownership guards, quarantine and backups")
