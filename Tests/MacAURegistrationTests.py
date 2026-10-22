#!/usr/bin/env python3
"""Check temporary AU registration preservation with mocked native tools."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

REPOSITORY = Path(__file__).resolve().parents[1]

with tempfile.TemporaryDirectory(prefix="ufo-au-registration-test-") as temporary:
    root = Path(temporary)
    source = root / "staged/UFO.component"
    binary = source / "Contents/MacOS/UFO"
    binary.parent.mkdir(parents=True)
    binary.write_text("new-au")
    binary.chmod(0o700)
    (source / "Contents/Info.plist").write_text("unchanged signed metadata")
    tools = root / "tools"
    tools.mkdir()
    destination = root / "user"
    components = destination / "Library/Audio/Plug-Ins/Components"
    target = components / "UFO.component"
    marker = root / "host-called"
    refresh_marker = root / "registrar-refreshed"
    refresh_log = root / "registrar-calls"
    mocks = {
        "uname": 'if [ "$1" = -s ]; then echo Darwin; else echo arm64; fi\n',
        "pgrep": '[ "${UFO_TEST_LOGIC_RUNNING:-}" = 1 ]\n',
        "codesign": 'exit 0\n',
        "ditto": 'cp -a "$1" "$2"\n',
        "auval": '''[ "$(cat "$UFO_TEST_COMPONENT_TARGET/Contents/MacOS/UFO")" = new-au ] || exit 3
if [ "$1" = -a ]; then
  if [ "${UFO_TEST_ALWAYS_MISSING:-}" = 1 ] || { [ "${UFO_TEST_NEEDS_REFRESH:-}" = 1 ] && [ ! -f "$UFO_TEST_REFRESH_MARKER" ]; }; then
    echo 'aumu samp appl - Apple: AUSampler'; exit 0
  fi
  echo 'aumu Ufo1 MpAu - MP Audio: UFO'; exit 0
fi
[ "$*" = '-v aumu Ufo1 MpAu' ] || exit 4
[ "${UFO_TEST_AUVAL_FAIL:-}" != 1 ] || exit 7
[ "${UFO_TEST_AUVAL_HANG:-}" != 1 ] || exec sleep 20
echo 'AU VALIDATION SUCCEEDED.'
''',
        "killall": '''[ "$#" = 4 ] && [ "$1" = -TERM ] && [ "$2" = -u ] && [ "$3" = "$(id -un)" ] && [ "$4" = AudioComponentRegistrar ] || exit 18
state=absent
if [ -f "$UFO_TEST_COMPONENT_TARGET/Contents/MacOS/UFO" ]; then state=$(cat "$UFO_TEST_COMPONENT_TARGET/Contents/MacOS/UFO"); fi
echo "$*|$state" >> "$UFO_TEST_REFRESH_LOG"
if [ "${UFO_TEST_REFRESH_ERROR:-}" = 1 ]; then echo 'killall: Operation not permitted' >&2; exit 1; fi
touch "$UFO_TEST_REFRESH_MARKER"
if [ "${UFO_TEST_REFRESH_ABSENT:-}" = 1 ]; then echo 'No matching processes belonging to you were found' >&2; exit 1; fi
''',
    }
    for name, body in mocks.items():
        path = tools / name
        path.write_text("#!/bin/sh\n" + body)
        path.chmod(0o700)
    stat = tools / "stat"
    stat.write_text("#!/usr/bin/env python3\nimport os, sys\ns=os.stat(sys.argv[-1]); print(str(s.st_dev)+':'+str(s.st_ino))\n")
    stat.chmod(0o700)
    host = tools / "host-test"
    host.write_text('''#!/bin/sh
[ "$1" = "$UFO_TEST_COMPONENT_TARGET" ] && [ "$3" = --au ] || exit 5
[ "$(cat "$1/Contents/MacOS/UFO")" = new-au ] || exit 6
echo called > "$UFO_TEST_HOST_MARKER"
[ "${UFO_TEST_HOST_FAIL:-}" != 1 ] || exit 9
if [ "${UFO_TEST_REPLACEMENT:-}" = 1 ]; then
  mv "$1" "$1-displaced"
  mkdir -p "$1/Contents/MacOS"
  echo external-change > "$1/Contents/MacOS/UFO"
fi
''')
    host.chmod(0o700)
    environment = os.environ.copy()
    environment.update(PATH=str(tools) + os.pathsep + environment["PATH"],
                       UFO_INSTALL_ROOT=str(destination), UFO_TEST_COMPONENT_TARGET=str(target),
                       UFO_TEST_HOST_MARKER=str(marker), UFO_TEST_REFRESH_MARKER=str(refresh_marker),
                       UFO_TEST_REFRESH_LOG=str(refresh_log), UFO_AU_DISCOVERY_SECONDS="1",
                       UFO_AU_VALIDATION_SECONDS="2")

    def validate(**options):
        invocation = environment.copy()
        invocation.update(options)
        marker.unlink(missing_ok=True)
        refresh_marker.unlink(missing_ok=True)
        refresh_log.unlink(missing_ok=True)
        return subprocess.run(["bash", str(REPOSITORY / "scripts/validate-au-macos.sh"),
                               str(source), str(host), str(root / "artifacts")],
                              env=invocation, text=True, capture_output=True)

    blocked = validate(UFO_TEST_LOGIC_RUNNING="1")
    assert blocked.returncode != 0 and "Quit Logic Pro" in blocked.stderr
    assert not destination.exists(), "Running Logic permitted registration changes"

    first = validate()
    assert first.returncode == 0, first.stderr
    assert marker.exists() and not target.exists(), "Temporary AU was not removed"
    assert not list(components.glob(".ufo-au-validation*")), "Temporary registration files survived"
    assert not refresh_log.exists(), "Normal discovery unnecessarily signaled a registrar"
    assert (source / "Contents/Info.plist").read_text() == "unchanged signed metadata"

    old = target / "Contents/MacOS/UFO"
    old.parent.mkdir(parents=True)
    old.write_text("previous-au")
    original_inode = target.stat().st_ino
    for options in ({}, {"UFO_TEST_AUVAL_FAIL": "1"}, {"UFO_TEST_HOST_FAIL": "1"}):
        result = validate(**options)
        assert (result.returncode == 0) == (not options), result.stderr
        assert old.read_text() == "previous-au" and target.stat().st_ino == original_inode
        assert not list(components.glob(".ufo-au-validation*")), "Failed validation left staging or locks"
        if "UFO_TEST_AUVAL_FAIL" in options:
            assert not marker.exists(), "Host ran after Apple validation failed"

    # Intel's cached registrar can miss a new component until this user's
    # metadata service is refreshed; restoration must refresh the old copy too.
    for options in ({"UFO_TEST_NEEDS_REFRESH": "1"},
                    {"UFO_TEST_NEEDS_REFRESH": "1", "UFO_TEST_REFRESH_ABSENT": "1"}):
        refreshed = validate(**options)
        assert refreshed.returncode == 0, refreshed.stderr
        assert old.read_text() == "previous-au" and marker.exists()
        calls = refresh_log.read_text().splitlines()
        assert len(calls) == 2 and calls[0].endswith("|new-au") and calls[1].endswith("|previous-au")
        registration = (root / "artifacts/registration.txt").read_text()
        assert "Registrar refresh (temporary)" in registration and "Registrar refresh (restored)" in registration

    for options in ({"UFO_TEST_ALWAYS_MISSING": "1"},
                    {"UFO_TEST_NEEDS_REFRESH": "1", "UFO_TEST_REFRESH_ERROR": "1"}):
        failed_refresh = validate(**options)
        assert failed_refresh.returncode != 0 and not marker.exists()
        assert old.read_text() == "previous-au" and target.stat().st_ino == original_inode
        assert len(refresh_log.read_text().splitlines()) == 2, "Failed fallback did not refresh the restored registration"
        assert not list(components.glob(".ufo-au-validation*")), "Failed fallback left staging or locks"

    timed_out = validate(UFO_TEST_AUVAL_HANG="1")
    assert timed_out.returncode != 0 and "timed out" in timed_out.stderr
    assert old.read_text() == "previous-au" and target.stat().st_ino == original_inode
    assert not list(components.glob(".ufo-au-validation*")), "Timed-out validator left temporary registration"

    lock = components / ".ufo-au-validation.lock"
    lock.mkdir()
    locked = validate()
    assert locked.returncode != 0 and lock.exists() and old.read_text() == "previous-au"
    lock.rmdir()

    # A separate user action during validation must never be removed by cleanup.
    replaced = validate(UFO_TEST_REPLACEMENT="1")
    assert replaced.returncode != 0 and "changed during validation" in replaced.stderr
    assert old.read_text().strip() == "external-change", "Cleanup removed an unexpected replacement"
    backups = list(components.glob(".ufo-au-validation.*/previous-component/Contents/MacOS/UFO"))
    assert len(backups) == 1 and backups[0].read_text() == "previous-au", "Previous AU is not recoverable"

print("PASS: mocked AU registration, per-user registrar fallback/restoration, validator timeout, Logic guard, failure cleanup, lock refusal and concurrent-change preservation")
