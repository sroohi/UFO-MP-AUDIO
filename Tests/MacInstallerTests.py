#!/usr/bin/env python3
"""Exercise bundle transaction/rollback with mocked native tools, not Mac validation."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

REPOSITORY = Path(__file__).resolve().parents[1]

with tempfile.TemporaryDirectory(prefix="ufo-installer-test-") as temporary:
    root = Path(temporary)
    project = root / "project"
    (project / "scripts").mkdir(parents=True)
    shutil.copy2(REPOSITORY / "scripts/install-local-macos.sh", project / "scripts/install-local-macos.sh")
    for name, contents in [("UFO.app", "new-app"), ("UFO.vst3", "new-plugin")]:
        binary = project / "dist/UFO" / name / "Contents/MacOS/UFO"
        binary.parent.mkdir(parents=True)
        binary.write_text(contents)
        binary.chmod(0o700)
    tools = root / "tools"
    tools.mkdir()
    mocks = {
        "uname": 'if [ "$1" = "-s" ]; then echo Darwin; else echo x86_64; fi\n',
        "pgrep": "exit 1\n",
        "lipo": "exit 0\n",
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
    application = destination / "Applications/UFO.app/Contents/MacOS/UFO"
    plugin = destination / "Library/Audio/Plug-Ins/VST3/UFO.vst3/Contents/MacOS/UFO"

    def install(*arguments, fail_target=None, fail_quarantine=False):
        invocation_environment = environment.copy()
        if fail_target:
            invocation_environment["UFO_TEST_FAIL_TARGET"] = str(fail_target)
        if fail_quarantine:
            invocation_environment["UFO_TEST_QUARANTINE"] = "1"
            invocation_environment["UFO_TEST_XATTR_FAIL"] = "1"
        return subprocess.run(["bash", str(project / "scripts/install-local-macos.sh"), *arguments],
                              env=invocation_environment, text=True, capture_output=True)

    first = install()
    assert first.returncode == 0, first.stderr
    assert application.read_text() == "new-app" and plugin.read_text() == "new-plugin"
    refused = install()
    assert refused.returncode != 0 and "Already exists" in refused.stderr
    assert application.read_text() == "new-app" and plugin.read_text() == "new-plugin"
    application.write_text("previous-app")
    plugin.write_text("previous-plugin")
    replaced = install("--replace")
    assert replaced.returncode == 0, replaced.stderr
    app_backups = list((destination / "Applications").glob("UFO-backup.*/UFO.app/Contents/MacOS/UFO"))
    plugin_backups = list((destination / "Library/Audio/Plug-Ins/VST3").glob("UFO-backup.*/UFO.vst3/Contents/MacOS/UFO"))
    assert any(path.read_text() == "previous-app" for path in app_backups)
    assert any(path.read_text() == "previous-plugin" for path in plugin_backups)
    assert application.read_text() == "new-app" and plugin.read_text() == "new-plugin"
    application.write_text("stable-app")
    plugin.write_text("stable-plugin")
    failed = install("--replace", fail_target=plugin.parents[2])
    assert failed.returncode != 0, "Injected final validation failure was not reached"
    assert application.read_text() == "stable-app", "Application rollback lost the previous version"
    assert plugin.read_text() == "stable-plugin", "VST3 rollback lost the previous version"
    failed_quarantine = install("--replace", fail_quarantine=True)
    assert failed_quarantine.returncode != 0, "Injected quarantine-clear failure was not reached"
    assert application.read_text() == "stable-app" and plugin.read_text() == "stable-plugin"
    assert not list((destination / "Applications").glob(".ufo-install.*"))
    assert not list((destination / "Library/Audio/Plug-Ins/VST3").glob(".ufo-install.*"))

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
    assert (package_destination / "Applications/UFO.app/Contents/MacOS/UFO").read_text() == "new-app"
    assert (package_destination / "Library/Audio/Plug-Ins/VST3/UFO.vst3/Contents/MacOS/UFO").read_text() == "new-plugin"
    cleared_paths = quarantine_log.read_text().splitlines()
    assert cleared_paths and all(path.startswith(str(package_destination)) and "/.ufo-install." in path
                                 for path in cleared_paths), "Quarantine clearing escaped owned staging directories"

print("PASS: mocked source/packaged installation, scoped quarantine clearing, existing-install preservation, backups and rollback")
