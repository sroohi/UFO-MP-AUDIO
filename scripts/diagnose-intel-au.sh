#!/bin/bash
# Inspect real AU discovery on an empty, disposable Intel runner, without builds.
set -euo pipefail
cd "$(dirname "$0")/.."
[[ "$(uname -s)" == Darwin && "$(uname -m)" == x86_64 && "${GITHUB_ACTIONS:-}" == true && "${RUNNER_OS:-}" == macOS ]] || {
  echo 'This diagnostic requires a disposable GitHub-hosted Intel Mac.' >&2; exit 1;
}
[[ "${RUNNER_TEMP:-}" == /* && -d "$RUNNER_TEMP" ]] || exit 1
[[ "${UFO_DIAGNOSTIC_PACKAGE_RUN:-}" =~ ^[0-9]+$ ]] || exit 1
logs="$PWD/dist/diagnostics/intel-au"
archive="$PWD/dist/diagnostic-package/UFO-macOS-universal-unsigned.zip"
mkdir -p "$logs"
started=$(date +%s)
names=(UFO.app UFO.vst3 UFO.component)
parents=("$HOME/Applications" "$HOME/Library/Audio/Plug-Ins/VST3" "$HOME/Library/Audio/Plug-Ins/Components")
targets=("${parents[0]}/${names[0]}" "${parents[1]}/${names[1]}" "${parents[2]}/${names[2]}")
identities=("" "" "")
for index in "${!names[@]}"; do
  [[ ! -e "${targets[index]}" && ! -L "${targets[index]}" ]] || { echo 'Runner already has UFO installed.' >&2; exit 1; }
  for backup in "${parents[index]}"/UFO-backup.*; do
    [[ ! -e "$backup" && ! -L "$backup" ]] || { echo 'Runner already has UFO backups.' >&2; exit 1; }
  done
done
work=$(mktemp -d "$RUNNER_TEMP/ufo-au-diagnostic.XXXXXX")
lock="${parents[2]}/.ufo-au-validation.lock"
locked=false
cleanup() {
  status=$?
  trap - EXIT HUP INT TERM
  set +e
  for index in "${!names[@]}"; do
    target="${targets[index]}"
    if [[ -e "$target" || -L "$target" ]]; then
      if [[ ! -L "$target" && -n "${identities[index]}" && "$(stat -f '%d:%i' "$target" 2>/dev/null)" == "${identities[index]}" ]]; then
        rm -rf "$target"
      else
        echo "Preserving changed or unowned destination: $target" >&2
        status=1
      fi
    fi
  done
  [[ "$locked" != true ]] || rmdir "$lock" || status=1
  rm -rf "$work"
  printf 'Diagnostic exit: %s\n' "$status" > "$logs/result.txt"
  exit "$status"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

snapshot() {
  local phase="$1"
  {
    printf 'UTC: '; date -u
    printf 'UID: '; id -u
    printf 'Console user: '; stat -f '%Su' /dev/console
    sw_vers
    uname -m
    ps -U "$(id -u)" -o pid=,ppid=,uid=,comm= | awk '/AudioComponentRegistrar|AUHostingService|auval/ {print}'
  } > "$logs/$phase-processes.txt"
  python3 - "$logs/$phase-cache.json" <<'PY'
from pathlib import Path
import json,sys
folder=Path.home()/'Library/Caches/AudioUnitCache'
data=[{'name':p.name,'mtime':p.stat().st_mtime,'bytes':p.stat().st_size} for p in folder.iterdir() if p.is_file()] if folder.is_dir() else []
Path(sys.argv[1]).write_text(json.dumps(data,indent=2)+'\n')
PY
}

run_bounded() {
  local phase="$1" budget="$2"; shift 2
  python3 - "$logs" "$phase" "$budget" "$@" <<'PY'
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import json,os,signal,subprocess,sys,time
logs,phase,budget=Path(sys.argv[1]),sys.argv[2],int(sys.argv[3]);command=sys.argv[4:]
begin=time.monotonic();sampled=False;timed_out=False
def sample(pid,label):
    try:
        owner=subprocess.check_output(['ps','-p',str(pid),'-o','uid='],text=True).strip()
        if owner!=str(os.getuid()):return
        with (logs/(phase+'-'+label+'-sample-command.txt')).open('w') as output:
            subprocess.run(['/usr/bin/sample',str(pid),'3','1','-file',str(logs/(phase+'-'+label+'-sample.txt'))],stdout=output,stderr=subprocess.STDOUT,timeout=8)
    except (OSError,subprocess.SubprocessError):pass
with (logs/(phase+'.log')).open('w') as output:
    child=subprocess.Popen(command,stdout=output,stderr=subprocess.STDOUT,start_new_session=True)
    while child.poll() is None:
        elapsed=time.monotonic()-begin
        if elapsed>=budget:
            timed_out=True
            try:os.killpg(child.pid,signal.SIGTERM)
            except ProcessLookupError:pass
            try:child.wait(timeout=3)
            except subprocess.TimeoutExpired:
                try:os.killpg(child.pid,signal.SIGKILL)
                except ProcessLookupError:pass
                child.wait()
            break
        if elapsed>=10 and not sampled:
            sampled=True;processes=[(child.pid,'auval')]
            # Darwin's short process-name matching can truncate long names.
            # Read executable names from this user's processes and retain only
            # the registrar or children in the owned auval process group.
            result=subprocess.run(['ps','-U',str(os.getuid()),'-o','pid=,pgid=,comm='],capture_output=True,text=True)
            for line in result.stdout.splitlines():
                fields=line.split(None,2)
                if len(fields)!=3 or not fields[0].isdigit():continue
                pid,group,command_name=fields
                name=Path(command_name).name
                if name=='AudioComponentRegistrar':processes.append((int(pid),'registrar-'+pid))
                elif group==str(child.pid) and int(pid)!=child.pid and name in ('auval','auvaltool'):
                    processes.append((int(pid),'auval-child-'+pid))
            processes=processes[:6]
            with ThreadPoolExecutor(max_workers=4) as workers:list(workers.map(lambda item:sample(*item),processes))
        time.sleep(.1)
    native=child.wait()
code=124 if timed_out else native
(logs/(phase+'.json')).write_text(json.dumps({'command':command,'budgetSeconds':budget,'elapsedSeconds':time.monotonic()-begin,'timedOut':timed_out,'nativeReturnCode':native,'exitCode':code},indent=2)+'\n')
print(phase+': exit='+str(code)+', elapsed='+str(round(time.monotonic()-begin,2))+'s',flush=True)
sys.exit(code if code>=0 else 1)
PY
}

unzip -t "$archive" > "$logs/archive-check.txt"
shasum -a 256 "$archive" > "$logs/package-sha256.txt"
printf 'Package workflow run: %s\nDiagnostic source commit: %s\n' "$UFO_DIAGNOSTIC_PACKAGE_RUN" "${GITHUB_SHA:-unknown}" > "$logs/provenance.txt"
ditto -x -k "$archive" "$work"
package="$work/UFO"
[[ -x "$package/Install.command" ]] || exit 1
for name in "${names[@]}"; do
  lipo "$package/$name/Contents/MacOS/UFO" -verify_arch arm64 x86_64
  codesign --verify --deep --strict "$package/$name"
done
/usr/libexec/PlistBuddy -c Print "$package/UFO.component/Contents/Info.plist" > "$logs/component-plist.txt"
snapshot baseline
run_bounded baseline-list 60 /usr/bin/auval -a || true
mkdir -p "${parents[2]}"
mkdir "$lock" || { echo 'Another AU operation owns the registration lock.' >&2; exit 1; }
locked=true
ditto "$package/UFO.component" "${targets[2]}"
identities[2]=$(stat -f '%d:%i' "${targets[2]}")
touch "${targets[2]}" "${targets[2]}/Contents/Info.plist"
snapshot registered
run_bounded registered-list 90 /usr/bin/auval -a || true
validated=false
if run_bounded registered-target 90 /usr/bin/auval -v aumu Ufo1 MpAu && awk '/AU VALIDATION SUCCEEDED/ {found=1} END {exit !found}' "$logs/registered-target.log"; then
  validated=true
else
  snapshot before-refresh
  python3 - "$logs" "${targets[2]}" "${parents[2]}" <<'PY'
from pathlib import Path
import subprocess,sys
logs,component,parent=Path(sys.argv[1]),Path(sys.argv[2]),Path(sys.argv[3])
commands=[
    ('installed-paths.txt',['/bin/ls','-ld',str(parent.parent),str(parent),str(component),str(component/'Contents'),str(component/'Contents/Info.plist'),str(component/'Contents/MacOS/UFO')],8),
    ('installed-stat.txt',['/usr/bin/stat','-f','%N uid=%u gid=%g mode=%Sp inode=%d:%i modified=%m',str(parent),str(component),str(component/'Contents/Info.plist'),str(component/'Contents/MacOS/UFO')],8),
    ('installed-xattrs.txt',['/usr/bin/xattr','-lr',str(component)],8),
    ('registrar-unified-before-refresh.log',['/usr/bin/log','show','--style','compact','--last','5m','--info','--debug','--predicate','process == "AudioComponentRegistrar" OR process == "auvaltool"'],15),
]
for filename,command,budget in commands:
    with (logs/filename).open('w') as output:
        try:
            result=subprocess.run(command,stdout=output,stderr=subprocess.STDOUT,timeout=budget)
            output.write('\nDiagnostic command exit: '+str(result.returncode)+'\n')
        except subprocess.TimeoutExpired:output.write('\nDiagnostic command exceeded '+str(budget)+' seconds.\n')
        except OSError as error:output.write('\nDiagnostic command error: '+str(error)+'\n')
PY
  refresh_status=0
  python3 - "$logs/registrar-refresh.json" > "$logs/registrar-refresh.log" 2>&1 <<'PY' || refresh_status=$?
from pathlib import Path
import json,os,signal,subprocess,sys,time
expected='/System/Library/Frameworks/AudioToolbox.framework/AudioComponentRegistrar'
uid=os.getuid()
def identity(pid):
    fields=[]
    for field in ('uid','lstart','comm'):
        result=subprocess.run(['ps','-p',str(pid),'-o',field+'='],capture_output=True,text=True)
        if result.returncode:return None
        fields.append(result.stdout.strip())
    return {'pid':pid,'uid':fields[0],'started':fields[1],'executable':fields[2]}
def owned(record):
    return record is not None and record['uid']==str(uid) and record['executable']==expected
listing=subprocess.run(['ps','-U',str(uid),'-o','pid=,comm='],capture_output=True,text=True,check=True)
records=[]
for line in listing.stdout.splitlines():
    fields=line.split(None,1)
    if len(fields)!=2 or not fields[0].isdigit() or fields[1]!=expected:continue
    before=identity(int(fields[0]))
    if owned(before):records.append({'before':before})
failed=False
for record in records:
    before=record['before'];pid=before['pid']
    if identity(pid)!=before:
        record['term']='Skipped: identity changed before signal';failed=True;continue
    try:os.kill(pid,signal.SIGTERM);record['term']='Sent to captured owned PID'
    except ProcessLookupError:record['term']='Already exited'
    except OSError as error:record['term']=str(error);failed=True
deadline=time.monotonic()+1.0
while time.monotonic()<deadline and any(identity(record['before']['pid'])==record['before'] for record in records):time.sleep(.1)
for record in records:
    before=record['before'];pid=before['pid'];after=identity(pid);record['afterTerm']=after
    if after is None:record['kill']='Not needed: captured PID exited';continue
    if after!=before or not owned(after):
        record['kill']='Skipped: identity changed';continue
    # Only this same-user Apple metadata service with unchanged start time is
    # eligible. No process-name-wide KILL, audio daemon or cache deletion.
    if identity(pid)!=before:
        record['kill']='Skipped: identity changed before signal';continue
    try:os.kill(pid,signal.SIGKILL);record['kill']='Sent to surviving captured owned PID'
    except ProcessLookupError:record['kill']='Already exited'
    except OSError as error:record['kill']=str(error);failed=True
deadline=time.monotonic()+2.0
while time.monotonic()<deadline and any(identity(record['before']['pid'])==record['before'] for record in records):time.sleep(.1)
for record in records:
    record['afterKill']=identity(record['before']['pid'])
    if record['afterKill']==record['before']:failed=True
Path(sys.argv[1]).write_text(json.dumps({'expectedExecutable':expected,'uid':uid,'records':records,'failed':failed},indent=2)+'\n')
print('Captured owned registrars:',len(records),'refresh failed:',failed,flush=True)
sys.exit(1 if failed else 0)
PY
  printf 'Owned registrar refresh exit: %s\n' "$refresh_status" >> "$logs/registrar-refresh.log"
  run_bounded refreshed-list 90 /usr/bin/auval -a || true
  snapshot after-refresh-query
  if run_bounded refreshed-target 90 /usr/bin/auval -v aumu Ufo1 MpAu && awk '/AU VALIDATION SUCCEEDED/ {found=1} END {exit !found}' "$logs/refreshed-target.log"; then validated=true; fi
fi
snapshot after-validation
python3 - "$logs/crash-report-summaries.json" "$started" <<'PY'
from pathlib import Path
import json,sys
records=[];start=int(sys.argv[2]);folder=Path.home()/'Library/Logs/DiagnosticReports'
for path in folder.iterdir() if folder.is_dir() else []:
    if not path.is_file() or path.stat().st_mtime<start or not path.name.startswith(('AudioComponentRegistrar','AUHostingService','auval')):continue
    record={'name':path.name,'bytes':path.stat().st_size,'mtime':path.stat().st_mtime}
    text=path.read_text(errors='replace')
    if path.suffix=='.ips':
        try:
            detail=json.loads(text.partition('\n')[2]);record['summary']={key:detail[key] for key in ('procName','captureTime','exception','termination','faultingThread') if key in detail}
        except (ValueError,TypeError):pass
    else:record['summary']=[line for line in text.splitlines() if line.startswith(('Process:','Exception Type:','Termination Reason:','Crashed Thread:','Code Type:'))]
    records.append(record)
Path(sys.argv[1]).write_text(json.dumps(records,indent=2)+'\n')
PY
[[ "$validated" == true ]] || { echo 'Targeted Apple AU validation did not genuinely succeed.' >&2; exit 1; }
[[ "$(stat -f '%d:%i' "${targets[2]}")" == "${identities[2]}" ]] || exit 1
rm -rf "${targets[2]}"
identities[2]=''
rmdir "$lock"
locked=false
install_status=0
"$package/Install.command" > "$logs/packaged-install.log" 2>&1 || install_status=$?
for index in "${!names[@]}"; do
  if [[ -d "${targets[index]}" && ! -L "${targets[index]}" ]]; then identities[index]=$(stat -f '%d:%i' "${targets[index]}"); fi
done
for retained in "$HOME/Library/MP Audio/UFO/Install-validation"/install.*; do
  [[ ! -d "$retained" || -L "$retained" ]] || ditto "$retained" "$logs/$(basename "$retained")"
done
printf 'Packaged installer exit: %s\n' "$install_status" > "$logs/installer-status.txt"
[[ "$install_status" == 0 ]] || exit "$install_status"
echo 'PASS: real targeted Apple AU validator and unchanged packaged installer on Intel; Logic UI and human listening remain unrun.' > "$logs/diagnostic-success.txt"
