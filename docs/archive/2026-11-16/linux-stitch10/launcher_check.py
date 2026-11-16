import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

appdir = Path(sys.argv[1])
gui_name = sys.argv[2] if len(sys.argv) > 2 else "lichii"
flags = ['EDEN_NX2_NO_MESH', 'EDEN_NX2_NO_MESH_EMULATION', 'EDEN_NX2_NO_TASK_EMULATION']
with tempfile.TemporaryDirectory(prefix='lichii-launcher-check-') as directory:
    root = Path(directory)
    binary_dir = root / 'bin'
    binary_dir.mkdir()
    for name in [gui_name, 'eden-cli']:
        shutil.copy2(appdir / 'usr/bin' / name, binary_dir / name)
        stub = binary_dir / (name + '-bin')
        stub.write_text('#!/usr/bin/python3\nimport os,sys,json\nprint(json.dumps({"environment":dict(os.environ),"arguments":sys.argv[1:]}))\n')
        stub.chmod(0o755)
    (root / 'AppRun').symlink_to('bin/' + gui_name)
    for launcher in [binary_dir / gui_name, binary_dir / 'eden-cli', root / 'AppRun']:
        env = dict(os.environ, EDEN_NX2_FORCE_MESH_FALLBACK='0', QT_QPA_PLATFORM='offscreen')
        env.update({flag: '1' for flag in flags})
        result = subprocess.run([str(launcher), 'a game with spaces.nup'], env=env,
                                check=True, capture_output=True, text=True)
        record = json.loads(result.stdout)
        assert record['environment']['EDEN_NX2_FORCE_MESH_FALLBACK'] == '1'
        assert all(flag not in record['environment'] for flag in flags)
        assert record['environment']['QT_QPA_PLATFORM'] == 'offscreen'
        assert record['arguments'] == ['a game with spaces.nup']
    env = dict(os.environ)
    env.pop('QT_QPA_PLATFORM', None)
    result = subprocess.run([str(root / 'AppRun')], env=env, check=True,
                            capture_output=True, text=True)
    assert json.loads(result.stdout)['environment']['QT_QPA_PLATFORM'] == 'xcb'
print('PASS GUI, CLI and AppRun: forced fallback, disable flags cleared, arguments preserved, X11 default and platform override')
