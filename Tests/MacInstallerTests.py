#!/usr/bin/env python3
"""Exercise bundle transactions with mocked native tools, not native Mac validation."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

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
    tools = root / "tools"
    tools.mkdir()
    mocks = {
        "uname": 'if [ "$1" = "-s" ]; then echo Darwin; else echo x86_64; fi\n',
        "pgrep": '''for arg do process="$arg"; done
if [ "${UFO_TEST_RUNNING_LOGIC:-}" = 1 ] && [ "$process" = "Logic Pro" ]; then exit 0; fi
exit 1
''',
        "lipo": '[ "$#" -eq 3 ] && [ -f "$1" ] && [ "$2" = "-verify_arch" ] && [ "$3" = x86_64 ]\n',
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
    binaries = [destination / parent / name / "Contents/MacOS/UFO" for name, parent, _ in BUNDLES]

    def install(*arguments, fail_target=None, fail_quarantine=False, running_logic=False):
        invocation_environment = environment.copy()
        if fail_target:
            invocation_environment["UFO_TEST_FAIL_TARGET"] = str(fail_target)
        if fail_quarantine:
            invocation_environment["UFO_TEST_QUARANTINE"] = "1"
            invocation_environment["UFO_TEST_XATTR_FAIL"] = "1"
        if running_logic:
            invocation_environment["UFO_TEST_RUNNING_LOGIC"] = "1"
        return subprocess.run(["bash", str(project / "scripts/install-local-macos.sh"), *arguments],
                              env=invocation_environment, text=True, capture_output=True)

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
    package_environment = environment.copy()
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

print("PASS: mocked app/VST3/AU source and packaged installation, Logic guard, scoped quarantine, backups and three-bundle rollback")
