#!/usr/bin/env python3
"""Check temporary AU registration preservation with mocked native tools."""
import json
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
    registrar_state = root / "registrar-state.json"
    mocks = {
        "uname": 'if [ "$1" = -s ]; then echo Darwin; else echo arm64; fi\n',
        "pgrep": '[ "${UFO_TEST_LOGIC_RUNNING:-}" = 1 ]\n',
        "codesign": 'exit 0\n',
        "ditto": 'cp -a "$1" "$2"\n',
    }
    for name, body in mocks.items():
        path = tools / name
        path.write_text("#!/bin/sh\n" + body)
        path.chmod(0o700)
    stat = tools / "stat"
    stat.write_text("#!/usr/bin/env python3\nimport os, sys\ns=os.stat(sys.argv[-1]); print(str(s.st_dev)+':'+str(s.st_ino))\n")
    stat.chmod(0o700)
    native_mock = tools / "registrar-tool"
    native_mock.write_text('''#!/usr/bin/env python3
import json, os, sys
from pathlib import Path
command=Path(sys.argv[0]).name
args=sys.argv[1:]
state_path=Path(os.environ['UFO_TEST_REGISTRAR_STATE'])
state=json.loads(state_path.read_text())
target=Path(os.environ['UFO_TEST_COMPONENT_TARGET'])/'Contents/MacOS/UFO'
marker=Path(os.environ['UFO_TEST_REFRESH_MARKER'])
def enabled(name): return os.environ.get(name)=='1'
def save(): state_path.write_text(json.dumps(state))
def row(): return str(state['pid'])+' '+str(state['uid'])+' '+state['start']+' '+state['executable']
if command=='ps':
    if enabled('UFO_TEST_PS_ERROR'):
        print('ps: injected metadata error',file=sys.stderr);sys.exit(2)
    if args==['-axo','pid=,uid=,lstart=,comm=']:
        if state['alive']:
            print(row())
            if enabled('UFO_TEST_BEFORE_TERM_CHANGE'):
                state['executable']='/tmp/AudioComponentRegistrar';save()
        else:
            state['absenceObserved']=True;save()
        sys.exit(0)
    if len(args)!=4 or args[0]!='-p' or args[2:]!=['-o','pid=,uid=,lstart=,comm=']:
        print('ps: unexpected command',file=sys.stderr);sys.exit(19)
    if state['alive'] and str(state['pid'])==args[1]:print(row());sys.exit(0)
    sys.exit(1)
if command=='kill':
    if len(args)!=2 or args[0] not in ('-TERM','-KILL') or not args[1].isdigit() or int(args[1])<=0:
        print('unsafe signal request',file=sys.stderr);sys.exit(20)
    if not state['alive'] or str(state['pid'])!=args[1]:sys.exit(1)
    record={'signal':args[0], 'pid':state['pid'], 'uid':state['uid'], 'start':state['start'], 'executable':state['executable'], 'target':target.read_text().strip() if target.exists() else 'absent'}
    with open(os.environ['UFO_TEST_REFRESH_LOG'],'a') as out:out.write(json.dumps(record)+'\\n')
    if enabled('UFO_TEST_REFRESH_ERROR'):
        print('kill: Operation not permitted',file=sys.stderr);sys.exit(1)
    if args[0]=='-KILL' and enabled('UFO_TEST_KILL_ERROR'):
        print('kill: injected KILL failure',file=sys.stderr);sys.exit(1)
    if args[0]=='-TERM':
        if enabled('UFO_TEST_UID_CHANGED'):state['uid']+=1
        elif enabled('UFO_TEST_PATH_CHANGED'):state['executable']='/tmp/AudioComponentRegistrar'
        elif enabled('UFO_TEST_START_CHANGED'):
            state['generation']+=1;state['start']='Thu Oct 22 18:16:'+str(48+state['generation'])+' 2026'
        elif not enabled('UFO_TEST_TERM_IGNORED'):state['alive']=False
    else:
        if not enabled('UFO_TEST_KILL_IGNORED'):state['alive']=False
    if not state['alive']:marker.touch()
    save();sys.exit(0)
if command=='auval':
    if not target.exists() or target.read_text()!='new-au':sys.exit(3)
    if args==['-a']:
        if not state['alive'] and (marker.exists() or state.get('absenceObserved')):
            state.update(alive=True,pid=state['pid']+1,uid=os.getuid(),start='Thu Oct 22 18:17:01 2026');save()
        if enabled('UFO_TEST_ALWAYS_MISSING') or (enabled('UFO_TEST_NEEDS_REFRESH') and not marker.exists() and not state.get('absenceObserved')):
            print('aumu samp appl - Apple: AUSampler');sys.exit(0)
        print('aumu Ufo1 MpAu - MP Audio: UFO');sys.exit(0)
    if args!=['-v','aumu','Ufo1','MpAu']:sys.exit(4)
    if enabled('UFO_TEST_AUVAL_FAIL'):sys.exit(7)
    if enabled('UFO_TEST_AUVAL_HANG'):os.execvp('sleep',['sleep','20'])
    print('AU VALIDATION SUCCEEDED.');sys.exit(0)
sys.exit(21)
''')
    native_mock.chmod(0o700)
    for name in ("ps", "kill", "auval"):
        shutil.copy2(native_mock, tools / name)
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
                       UFO_TEST_REFRESH_LOG=str(refresh_log), UFO_TEST_REGISTRAR_STATE=str(registrar_state),
                       UFO_AU_DISCOVERY_SECONDS="1", UFO_AU_VALIDATION_SECONDS="2")

    def validate(**options):
        invocation = environment.copy()
        invocation.update(options)
        marker.unlink(missing_ok=True)
        refresh_marker.unlink(missing_ok=True)
        refresh_log.unlink(missing_ok=True)
        registrar_state.write_text(json.dumps({"alive": options.get("UFO_TEST_REFRESH_ABSENT") != "1", "pid": 424242,
            "uid": os.getuid(), "start": "Thu Oct 22 18:16:48 2026", "generation": 0,
            "executable": "/System/Library/Frameworks/AudioToolbox.framework/" +
                ("Versions/A/" if options.get("UFO_TEST_FRAMEWORK_SUBPATH") == "1" else "") + "AudioComponentRegistrar"}))
        return subprocess.run(["bash", str(REPOSITORY / "scripts/validate-au-macos.sh"),
                               str(source), str(host), str(root / "artifacts")],
                              env=invocation, text=True, capture_output=True)

    def signal_records():
        return [json.loads(line) for line in refresh_log.read_text().splitlines()] if refresh_log.exists() else []

    blocked = validate(UFO_TEST_LOGIC_RUNNING="1")
    assert blocked.returncode != 0 and "Quit Logic Pro" in blocked.stderr
    assert not destination.exists(), "Running Logic permitted registration changes"

    first = validate()
    assert first.returncode == 0, first.stderr
    assert marker.exists() and not target.exists(), "Temporary AU was not removed"
    assert not list(components.glob(".ufo-au-validation*")), "Temporary registration files survived"
    assert not signal_records(), "Normal discovery unnecessarily signaled a registrar"
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

    for options in ({"UFO_TEST_NEEDS_REFRESH": "1"},
                    {"UFO_TEST_NEEDS_REFRESH": "1", "UFO_TEST_REFRESH_ABSENT": "1"},
                    {"UFO_TEST_NEEDS_REFRESH": "1", "UFO_TEST_FRAMEWORK_SUBPATH": "1"},
                    {"UFO_TEST_NEEDS_REFRESH": "1", "UFO_TEST_TERM_IGNORED": "1"}):
        refreshed = validate(**options)
        assert refreshed.returncode == 0, refreshed.stderr
        assert old.read_text() == "previous-au" and marker.exists()
        calls = signal_records()
        assert calls and calls[-1]["target"] == "previous-au", "Restored registration was not refreshed"
        assert all(call["uid"] == os.getuid() and call["pid"] > 0 and call["executable"].startswith("/System/Library/Frameworks/AudioToolbox.framework/") for call in calls)
        if "UFO_TEST_REFRESH_ABSENT" in options:
            assert len(calls) == 1 and calls[0]["signal"] == "-TERM"
        elif "UFO_TEST_TERM_IGNORED" in options:
            assert [call["signal"] for call in calls] == ["-TERM", "-KILL", "-TERM", "-KILL"]
            for term, kill in ((calls[0], calls[1]), (calls[2], calls[3])):
                assert all(term[key] == kill[key] for key in ("pid", "uid", "start", "executable")), "KILL identity differed from captured TERM identity"
        else:
            assert [call["signal"] for call in calls] == ["-TERM", "-TERM"]
        registration = (root / "artifacts/registration.txt").read_text()
        assert "Registrar refresh (temporary)" in registration and "Registrar refresh (restored)" in registration

    for changed in ("UFO_TEST_UID_CHANGED", "UFO_TEST_PATH_CHANGED", "UFO_TEST_START_CHANGED", "UFO_TEST_BEFORE_TERM_CHANGE"):
        failed_identity = validate(UFO_TEST_NEEDS_REFRESH="1", **{changed: "1"})
        assert failed_identity.returncode != 0 and not marker.exists()
        assert old.read_text() == "previous-au" and target.stat().st_ino == original_inode
        assert all(call["signal"] != "-KILL" for call in signal_records()), "Changed identity received KILL"
        if changed == "UFO_TEST_BEFORE_TERM_CHANGE":
            assert not signal_records(), "Changed identity received TERM before revalidation"

    for options in ({"UFO_TEST_ALWAYS_MISSING": "1"},
                    {"UFO_TEST_NEEDS_REFRESH": "1", "UFO_TEST_REFRESH_ERROR": "1"},
                    {"UFO_TEST_NEEDS_REFRESH": "1", "UFO_TEST_TERM_IGNORED": "1", "UFO_TEST_KILL_ERROR": "1"},
                    {"UFO_TEST_NEEDS_REFRESH": "1", "UFO_TEST_PS_ERROR": "1"}):
        failed_refresh = validate(**options)
        assert failed_refresh.returncode != 0 and not marker.exists()
        assert old.read_text() == "previous-au" and target.stat().st_ino == original_inode
        assert not list(components.glob(".ufo-au-validation*")), "Failed fallback left staging or locks"
        if "UFO_TEST_PS_ERROR" in options:
            assert not signal_records(), "Metadata errors permitted signals"
        elif "UFO_TEST_KILL_ERROR" in options:
            assert [call["signal"] for call in signal_records()] == ["-TERM", "-KILL", "-TERM", "-KILL"]
        else:
            assert len(signal_records()) == 2, "Failed fallback did not refresh the restored registration"

    timed_out = validate(UFO_TEST_AUVAL_HANG="1")
    assert timed_out.returncode != 0 and "timed out" in timed_out.stderr
    assert old.read_text() == "previous-au" and target.stat().st_ino == original_inode
    assert not list(components.glob(".ufo-au-validation*")), "Timed-out validator left temporary registration"

    lock = components / ".ufo-au-validation.lock"
    lock.mkdir()
    locked = validate()
    assert locked.returncode != 0 and lock.exists() and old.read_text() == "previous-au"
    lock.rmdir()

    replaced = validate(UFO_TEST_REPLACEMENT="1")
    assert replaced.returncode != 0 and "changed during validation" in replaced.stderr
    assert old.read_text().strip() == "external-change", "Cleanup removed an unexpected replacement"
    backups = list(components.glob(".ufo-au-validation.*/previous-component/Contents/MacOS/UFO"))
    assert len(backups) == 1 and backups[0].read_text() == "previous-au", "Previous AU is not recoverable"

print("PASS: mocked AU registration, verified PID TERM/KILL, identity-change refusal, restoration, timeout and Logic/concurrency guards")
