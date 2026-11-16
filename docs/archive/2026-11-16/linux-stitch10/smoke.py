import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time

appdir = Path('/opt/stitch-package-v10/AppDir')
with tempfile.TemporaryDirectory(prefix='stitch-v10-smoke-') as temporary:
    root = Path(temporary)
    env = dict(os.environ)
    for key in ['LD_LIBRARY_PATH', 'LD_PRELOAD', 'QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH',
                'QML2_IMPORT_PATH', 'EDEN_NX2_DEBUG_SERVER']:
        env.pop(key, None)
    env.update(HOME=str(root), XDG_CONFIG_HOME=str(root / 'config'),
               XDG_DATA_HOME=str(root / 'data'), XDG_CACHE_HOME=str(root / 'cache'),
               XDG_RUNTIME_DIR=str(root / 'runtime'), LD_BIND_NOW='1',
               QT_QPA_PLATFORM='offscreen')
    (root / 'runtime').mkdir(mode=0o700)
    for option in ['--version', '--help']:
        result = subprocess.run([str(appdir / 'usr/bin/eden-cli'), option], env=env,
                                capture_output=True, text=True, timeout=15)
        assert result.returncode == 0, result.stderr
        print('PASS packaged CLI ' + option, flush=True)
    with (root / 'gui.log').open('w+') as log:
        gui = subprocess.Popen([str(appdir / 'AppRun')], env=env, stdout=log,
                               stderr=subprocess.STDOUT, start_new_session=True)
        try:
            time.sleep(6)
            assert gui.poll() is None, 'GUI exited early'
            maps = Path('/proc/' + str(gui.pid) + '/maps').read_text()
            assert str(appdir / 'usr/lib/libQt6Core') in maps
            assert str(appdir / 'usr/lib/libva.so.2') in maps
            assert '/opt/Qt/' not in maps and '/opt/va-compat/' not in maps
            assert '/opt/build-lichii/externals/' not in maps
            assert 'libnxformat' not in maps, 'nxformat must be linked statically'
            needed = subprocess.check_output(['readelf', '-d', str(appdir / 'usr/bin/stitch-bin')], text=True)
            assert 'libnxformat' not in needed
            print('PASS GUI ran 6 s with eager binding; bundled Qt/libva and static nxformat; no build-prefix libraries', flush=True)
        finally:
            if gui.poll() is None:
                os.killpg(gui.pid, signal.SIGTERM)
                try:
                    gui.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(gui.pid, signal.SIGKILL)
                    gui.wait(timeout=5)
