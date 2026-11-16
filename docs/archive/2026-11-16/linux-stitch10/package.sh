#!/bin/bash
set -euo pipefail
export PATH=/opt/build-venv/bin:/opt/Qt/6.8.3/gcc_64/bin:$PATH
mkdir -p /opt/pack-tools /opt/stitch-package-v10
cd /opt/pack-tools
fetch_verified() {
  local file=$1 url=$2 digest=$3
  if ! test -f "$file"; then curl -fL "$url" -o "$file"; fi
  printf '%s  %s\n' "$digest" "$file" | sha256sum -c -
  chmod +x "$file"
}
fetch_verified linuxdeploy-x86_64.AppImage https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage df908b19f1ed1cdafa79508d415aa12d6c56ce4d56d4149aa8c09286718706e5
fetch_verified linuxdeploy-plugin-qt-x86_64.AppImage https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage cfc1055b2b9dbc08412b579f20990b7b41a17b61beaa5847dc9477c96c9e9617
fetch_verified appimagetool-x86_64.AppImage https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage e8c1169c4af66c3184d7ee110713d4be04befe671fa358e2c1591ee840553b03
fetch_verified runtime-x86_64 https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64 617aa77314d57c8b8e4fae9c3f0b35af7b30d5529cb69e98f64f92510ed98a46
cd /opt/stitch-package-v10
mkdir -p AppDir/usr/bin
cp /opt/build-lichii/bin/nx2_switches.txt AppDir/usr/bin/
cp /work/lichii/dist/lichii.png io.github.dylandeheer.stitch.png
export APPIMAGE_EXTRACT_AND_RUN=1
export QMAKE=/opt/Qt/6.8.3/gcc_64/bin/qmake
export EXTRA_PLATFORM_PLUGINS=libqoffscreen.so
export EXTRA_QT_MODULES=svg
export ARCH=x86_64
export PATH=/opt/pack-tools:$PATH
/opt/pack-tools/linuxdeploy-x86_64.AppImage --appdir AppDir \
  --executable /opt/build-lichii/bin/stitch \
  --executable /opt/build-lichii/bin/eden-cli \
  --desktop-file /work/lichii/dist/io.github.dylandeheer.stitch.desktop \
  --icon-file io.github.dylandeheer.stitch.png --plugin qt
cp -L /opt/va-compat/lib/libva.so.2 /opt/va-compat/lib/libva-drm.so.2 /opt/va-compat/lib/libva-x11.so.2 AppDir/usr/lib/
mkdir -p AppDir/usr/share/licenses/libva
cp /work/tooling/libva-source/COPYING AppDir/usr/share/licenses/libva/
for executable in stitch eden-cli; do
  mv "AppDir/usr/bin/$executable" "AppDir/usr/bin/$executable-bin"
  cat > "AppDir/usr/bin/$executable" <<'LAUNCH'
#!/bin/sh
set -eu
resolved_launcher=$(readlink -f -- "$0")
binary_dir=$(CDPATH= cd -- "$(dirname -- "$resolved_launcher")" && pwd)
export EDEN_NX2_FORCE_MESH_FALLBACK=1
export QT_QPA_PLATFORM=${QT_QPA_PLATFORM:-xcb}
unset EDEN_NX2_NO_MESH EDEN_NX2_NO_MESH_EMULATION EDEN_NX2_NO_TASK_EMULATION
exec "$binary_dir/@PAYLOAD@" "$@"
LAUNCH
  sed -i "s/@PAYLOAD@/$executable-bin/g" "AppDir/usr/bin/$executable"
  chmod +x "AppDir/usr/bin/$executable"
done
python3 - <<'PY'
from pathlib import Path
for desktop in Path('AppDir').rglob('*.desktop'):
    if desktop.name == 'io.github.dylandeheer.stitch.desktop':
        text = desktop.read_text()
        text = '\n'.join('Name=Stitch (Ocarina AMD test)' if line.startswith('Name=') else line for line in text.splitlines()) + '\n'
        desktop.write_text(text)
PY
mkdir -p /work/package-result
export VERSION=stitch-amd-10-20261111
/opt/pack-tools/appimagetool-x86_64.AppImage --runtime-file /opt/pack-tools/runtime-x86_64 AppDir /work/package-result/Stitch-OoT-AMD-10-x86_64.AppImage
sha256sum /work/package-result/Stitch-OoT-AMD-10-x86_64.AppImage
