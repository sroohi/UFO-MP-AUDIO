#!/bin/bash
# Debian cloud runner: install only inside /workspace, keeping signature checks enabled.
set -euo pipefail
cd "$(dirname "$0")/.."
root=/workspace/.ufo-system
python3 -m pip install --no-warn-script-location --target /workspace/.ufo-tools cmake==3.31.6 ninja==1.11.1.4
mkdir -p "$root/lists/partial" "$root/debs"
cat > "$root/sources.list" <<'SOURCES'
deb [signed-by=/usr/share/keyrings/debian-archive-keyring.gpg] https://deb.debian.org/debian trixie main
SOURCES
cat > "$root/apt.conf" <<'CONFIG'
Dir::Etc::parts "-";
Dir::Etc::main "-";
Dir::Etc::sourcelist "/workspace/.ufo-system/sources.list";
Dir::Etc::sourceparts "-";
Dir::State::lists "/workspace/.ufo-system/lists";
Dir::Cache "/workspace/.ufo-system/cache";
APT::Sandbox::User "agent";
CONFIG
export APT_CONFIG="$root/apt.conf"
/usr/bin/apt-get update
(
 cd "$root/debs"
 /usr/bin/apt-get download libasound2-dev libx11-dev libxext-dev libxinerama-dev libxrandr-dev libxcursor-dev libfreetype-dev libfontconfig-dev libxrender-dev libxfixes-dev x11proto-dev xorg-sgml-doctools libpng-dev libbrotli-dev zlib1g-dev libgl-dev libglx-dev libegl-dev libopengl-dev libharfbuzz-dev libgraphite2-dev libxau-dev libxdmcp-dev libxcb1-dev libbz2-dev xvfb
)
for package in "$root"/debs/*.deb; do dpkg-deb -x "$package" "$root/root"; done
python3 - <<'PY'
from pathlib import Path
import glob
root=Path('/workspace/.ufo-system/root')
for p in root.rglob('*.pc'):
    p.write_text(p.read_text().replace(str(root)+'/usr','/usr').replace('/usr',str(root)+'/usr'))
for p in (root/'usr/lib/x86_64-linux-gnu').glob('*.so'):
    if p.is_symlink() and not p.exists():
        candidates=glob.glob('/usr/lib/x86_64-linux-gnu/'+p.name+'.*')
        if candidates:
            p.unlink()
            p.symlink_to(sorted(candidates,key=len)[0])
PY
cat > "$root/activate.sh" <<'ACTIVATE'
export PATH=/workspace/.ufo-tools/cmake/data/bin:/workspace/.ufo-tools/bin:$PATH
export PKG_CONFIG_PATH=/workspace/.ufo-system/root/usr/lib/x86_64-linux-gnu/pkgconfig:/workspace/.ufo-system/root/usr/share/pkgconfig
export CPLUS_INCLUDE_PATH=/workspace/.ufo-system/root/usr/include:/workspace/.ufo-system/root/usr/include/x86_64-linux-gnu:/workspace/.ufo-system/root/usr/include/freetype2
export LIBRARY_PATH=/workspace/.ufo-system/root/usr/lib/x86_64-linux-gnu
export UFO_USER_DATA_DIR=${UFO_USER_DATA_DIR:-/workspace/.ufo-user-data}
ACTIVATE
source "$root/activate.sh"
CMAKE_BUILD_PARALLEL_LEVEL=3 cmake -S . -B build-native -G Ninja -DCMAKE_MAKE_PROGRAM=/workspace/.ufo-tools/bin/ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-native --parallel 3
scripts/test-cloud.sh
