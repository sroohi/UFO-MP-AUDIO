#!/bin/bash
# Revalidate identical universal binaries when only installation helpers changed.
set -euo pipefail
cd "$(dirname "$0")/.."
[[ "$(uname -s)" == Darwin && "${GITHUB_ACTIONS:-}" == true && "${RUNNER_OS:-}" == macOS ]] || {
  echo 'This package validation requires a disposable GitHub-hosted Mac.' >&2; exit 1;
}
[[ "${UFO_SOURCE_BUILD_RUN:-}" =~ ^[0-9]+$ ]] || { echo 'Missing numeric source build run.' >&2; exit 1; }
[[ "${UFO_NATIVE_ARCH:-}" == arm64 || "${UFO_NATIVE_ARCH:-}" == x86_64 ]] || exit 1
[[ "$(uname -m)" == "$UFO_NATIVE_ARCH" ]] || { echo 'Runner architecture does not match the requested native validation.' >&2; exit 1; }
source_commit=7eeaf5fe9f1c7feee441258d86a0def859f25f3c
logs="$PWD/dist/validation"
archive="$PWD/dist/upstream-package/UFO-macOS-universal-unsigned.zip"
upstream="$PWD/dist/upstream-validation/dist/validation"
mkdir -p "$logs"
gh api "repos/$GITHUB_REPOSITORY/actions/runs/$UFO_SOURCE_BUILD_RUN" > "$logs/upstream-run.json"
gh api "repos/$GITHUB_REPOSITORY/actions/runs/$UFO_SOURCE_BUILD_RUN/artifacts" > "$logs/upstream-artifacts.json"
python3 - "$logs" "$source_commit" "$UFO_NATIVE_ARCH" "$UFO_SOURCE_BUILD_RUN" <<'PY'
from pathlib import Path
import json,sys
logs,expected,arch,run=Path(sys.argv[1]),sys.argv[2],sys.argv[3],int(sys.argv[4])
metadata=json.loads((logs/'upstream-run.json').read_text())
if metadata['head_sha']!=expected or metadata['id']!=run or metadata['status']!='completed':
    raise SystemExit('The source run must be completed and contain exactly the approved unchanged C++ commit.')
artifacts=json.loads((logs/'upstream-artifacts.json').read_text())['artifacts']
for name in ('UFO-macOS-universal-development-arm64','UFO-macOS-native-validation-'+arch):
    matches=[item for item in artifacts if item['name']==name and not item['expired']]
    if len(matches)!=1:raise SystemExit('Missing unique unexpired source artifact: '+name)
    if matches[0].get('workflow_run',{}).get('head_sha',expected)!=expected:
        raise SystemExit('Source artifact commit differs from the approved C++ commit.')
PY
# Include uncommitted differences, and require the historic commit to be present.
git cat-file -e "$source_commit^{commit}"
if ! git diff --quiet "$source_commit" -- Source CMakeLists.txt ':(glob)Tests/*.cpp'; then
  echo 'C++ sources or CMake changed: run the complete native build workflow instead.' >&2
  exit 1
fi
git diff --stat "$source_commit" HEAD > "$logs/installer-source-diff.txt"
[[ -f "$upstream/ctest.log" && -f "$upstream/test-architectures.txt" && -f "$archive" ]] || {
  echo 'The exact source package and matching native CTest evidence are required.' >&2; exit 1;
}
python3 - "$upstream" "$UFO_NATIVE_ARCH" <<'PY'
from pathlib import Path
import sys
folder,arch=Path(sys.argv[1]),sys.argv[2]
log=(folder/'ctest.log').read_text()
if '100% tests passed' not in log or 'out of 6' not in log:
    raise SystemExit('The unchanged source must have six passing native CTests.')
targets=('ufo_engine_tests','ufo_sample_tests','ufo_streaming_tests','ufo_stream_asset_tests','ufo_plugin_tests','ufo_gpu_tests','ufo_vst3_host_tests')
lines=(folder/'test-architectures.txt').read_text().splitlines()
if any(target+': '+arch not in lines for target in targets):
    raise SystemExit('The inherited tests did not run as the requested native architecture.')
PY
cp "$upstream/ctest.log" "$logs/upstream-ctest.log"
cp "$upstream/test-architectures.txt" "$logs/upstream-test-architectures.txt"
cp "$upstream/architectures.txt" "$logs/upstream-architectures.txt"
cp "$upstream/machine.txt" "$logs/upstream-machine.txt"
ditto "$upstream/gpu" "$logs/upstream-gpu"
cp "$upstream/UFO-native-editor.png" "$logs/upstream-native-editor.png"
printf 'Inherited from source run %s, commit %s, native %s: six CTests, GPU shader/readback, and their architecture/device evidence. These suites are not rerun here. Current AU/VST3 host and installer results are recorded separately.\n' \
  "$UFO_SOURCE_BUILD_RUN" "$source_commit" "$UFO_NATIVE_ARCH" > "$logs/upstream-evidence.txt"
unzip -t "$archive" > "$logs/upstream-archive-check.txt"
shasum -a 256 "$archive" > "$logs/upstream-package-sha256.txt"
stage=$(mktemp -d "$PWD/dist/.ufo-revalidated-package.XXXXXX")
trap 'rm -rf "$stage"' EXIT
ditto -x -k "$archive" "$stage"
package="$stage/UFO"
bundle_manifest() {
  python3 - "$package" "$logs" "$1" <<'PY'
from pathlib import Path
import hashlib,json,os,stat,sys
package,logs,phase=Path(sys.argv[1]),Path(sys.argv[2]),sys.argv[3]
entries=[]
for name in ('UFO.app','UFO.vst3','UFO.component'):
    folder=package/name
    if not folder.is_dir() or folder.is_symlink():raise SystemExit('Missing original bundle: '+name)
    for path in sorted([folder,*folder.rglob('*')]):
        mode=path.lstat().st_mode
        entry={'path':str(path.relative_to(package)),'mode':stat.S_IMODE(mode)}
        if path.is_symlink():entry.update(kind='symlink',target=os.readlink(path))
        elif path.is_file():entry.update(kind='file',sha256=hashlib.sha256(path.read_bytes()).hexdigest())
        elif path.is_dir():entry.update(kind='directory')
        else:raise SystemExit('Unexpected bundle filesystem object: '+str(path))
        entries.append(entry)
if phase=='after' and entries!=json.loads((logs/'production-bundles-before.json').read_text()):
    raise SystemExit('Production bundle contents or permissions changed during packaging validation.')
(logs/('production-bundles-'+phase+'.json')).write_text(json.dumps(entries,indent=2)+'\n')
PY
}
bundle_manifest before
cmake -S . -B build-macos -G 'Unix Makefiles' \
  -DCMAKE_OSX_ARCHITECTURES='arm64;x86_64' \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 -DCMAKE_BUILD_TYPE=Release 2>&1 | tee "$logs/configure.log"
cp build-macos/ufo-architecture-targets.txt "$logs/architectures.txt"
cmake --build build-macos --config Release --parallel 3 --target ufo_vst3_host_tests 2>&1 | tee "$logs/host-build.log"
host_arch=$(lipo "$PWD/build-macos/ufo_vst3_host_tests" -archs)
printf 'ufo_vst3_host_tests: %s\n' "$host_arch" | tee "$logs/current-host-architecture.txt"
[[ "$host_arch" == "$UFO_NATIVE_ARCH" ]] || { echo 'The current host test must contain only the native architecture.' >&2; exit 1; }
scripts/validate-macos.sh "$package" "$PWD/build-macos" 2>&1 | tee "$logs/bundle-validation.log"
cp scripts/install-local-macos.sh "$package/Install.command"
chmod +x "$package/Install.command"
printf 'Local development build; ad-hoc signed, not notarized.\nProduction bundles unchanged from run %s, commit %s.\nCurrent installation helpers and native wrapper/installation validation: commit %s.\nSix source CTests and GPU shader checks are inherited; see validation provenance.\nLogic UI and human listening remain unrun.\n' \
  "$UFO_SOURCE_BUILD_RUN" "$source_commit" "$GITHUB_SHA" > "$package/BUILD-STATUS.txt"
bundle_manifest after
python3 - "$logs" "$archive" "$source_commit" "$UFO_NATIVE_ARCH" "$UFO_SOURCE_BUILD_RUN" "$GITHUB_SHA" <<'PY'
from pathlib import Path
import hashlib,json,os,sys
logs,archive,commit,arch,source_run,installer=Path(sys.argv[1]),Path(sys.argv[2]),sys.argv[3],sys.argv[4],int(sys.argv[5]),sys.argv[6]
manifest=json.loads((logs/'production-bundles-before.json').read_text())
data={'sourceBuildRun':source_run,'sourceCommit':commit,'installerCommit':installer,'validationRun':os.environ['GITHUB_RUN_ID'],
      'nativeArchitecture':arch,'sourcePackageSHA256':hashlib.sha256(archive.read_bytes()).hexdigest(),
      'productionBundleBinaries':{entry['path']:entry['sha256'] for entry in manifest if entry['path'].endswith('/Contents/MacOS/UFO')},
      'productionBundlesUnchanged':True,'currentChecks':['native AU/VST3 host MIDI/audio/state/editor and 12 rate/block combinations','Apple auval','standalone launch','actual packaged install/update with Apple auval and cleanup'],
      'inheritedChecks':{'sourceBuildRun':source_run,'nativeArchitecture':arch,'ctestCount':6,'includesGPUShaderReadback':True},
      'unrunChecks':['Logic Pro UI/project sessions','human listening','physical GPU performance']}
(logs/'package-provenance.json').write_text(json.dumps(data,indent=2)+'\n')
PY
cp "$logs/package-provenance.json" "$package/VALIDATION-PROVENANCE.json"
ditto -c -k --sequesterRsrc --keepParent "$package" "$stage/UFO-macOS-universal-unsigned.zip"
mv "$stage/UFO-macOS-universal-unsigned.zip" dist/UFO-macOS-universal-unsigned.zip
scripts/test-macos-installation.sh
python3 - "$logs/package-provenance.json" <<'PY'
from pathlib import Path
import hashlib,json,sys
path=Path(sys.argv[1]);data=json.loads(path.read_text())
data['packagedInstallationPassed']=True
data['packageSHA256']=hashlib.sha256(Path('dist/UFO-macOS-universal-unsigned.zip').read_bytes()).hexdigest()
path.write_text(json.dumps(data,indent=2)+'\n')
Path('dist/UFO-macOS-universal-unsigned.provenance.json').write_text(path.read_text())
PY
echo 'PASS: unchanged universal production bundles, current native AU/VST3 hosting, Apple auval and actual packaged install/update. Inherited CTests are labeled separately.'
