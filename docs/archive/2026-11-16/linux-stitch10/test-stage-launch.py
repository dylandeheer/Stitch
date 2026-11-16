from pathlib import Path
import configparser,hashlib,json,os,shutil,subprocess,tempfile,time
t=Path('/work/cpu-20261111')
digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
with tempfile.TemporaryDirectory(prefix='stitch10-stage-') as tmp:
    home=Path(tmp);b=home/'Applications/Lichii/oot-amd-candidate-20261107';(b/'diagnostics').mkdir(parents=True)
    for root in ['config','data','cache']:(b/'profile9'/root/'lichii').mkdir(parents=True)
    custom=b/'profile9/config/lichii/custom';custom.mkdir()
    original='[Renderer]\nresolution_setup=3\nanti_aliasing=1\npin_gpu_threads=true\npin_gpu_threads\\default=true\n[Controls]\nplayer_0_button_a="binding with spaces é"\n[UI]\nshow_whats_new=true\nshow_whats_new\\default=true'
    ini=b/'profile9/config/lichii/qt-config.ini';ini.write_text(original)
    pergame=custom/'0400486024DEC000.ini';pergame.write_text('[Renderer]\npin_gpu_threads=false\npin_gpu_threads\\default=false\npin_gpu_threads\\use_global=false\n[System]\nuse_docked_mode=1\nuse_docked_mode\\default=false\n')
    pergame_bytes=pergame.read_bytes()
    (b/'profile9/data/lichii/sentinel').write_text('Public synthetic profile data\n')
    (b/'profile9/data/lichii/link').symlink_to('sentinel')
    (b/'profile9/cache/lichii/sentinel').write_bytes(bytes(range(128)))
    for name in ['tensor-baseline','stitch9-tensor-baseline']:(b/'diagnostics'/name).write_text('Keep old experiment state\n')
    new_markers=['stitch10-mesh-empty-baseline','stitch10-texture-gc-baseline','stitch10-join-upload-baseline']
    for name in new_markers:(b/'diagnostics'/name).touch()
    launcher=b/'launch-lichii.sh';launcher.write_text('#!/bin/sh\nexec "Stitch-OoT-AMD-9-x86_64.AppImage" "$@"\n');old=digest(launcher)
    image=b/'Stitch-OoT-AMD-10-x86_64.AppImage.upload';image.write_bytes(b'public test image\n')
    upload=b/'launch-stitch10.sh.upload';shutil.copy2(t/'launch-stitch10.sh',upload)
    args=['python3',str(t/'stage.py'),digest(image),'a'*40,digest(upload),old];env=dict(os.environ,HOME=str(home))
    def rejected(call=args):
        result=subprocess.run(call,env=env,capture_output=True,text=True)
        assert result.returncode!=0
        assert launcher.read_text().count('Stitch-OoT-AMD-9')==1 and ini.read_text()==original and not (b/'profile10').exists()
        return result
    bad=args.copy();bad[2]='0'*64;rejected(bad)
    held=home/'held.ini';ini.rename(held);ini.symlink_to(held)
    assert subprocess.run(args,env=env,capture_output=True).returncode!=0
    assert held.read_text()==original and not (b/'profile10').exists()
    ini.unlink();held.rename(ini)
    held=ini.with_name('another-title.ini');ini.rename(held);ini.symlink_to('another-title.ini')
    assert subprocess.run(args,env=env,capture_output=True).returncode!=0
    assert held.read_text()==original and not (b/'profile10').exists()
    ini.unlink();held.rename(ini)
    escaped=b/'profile9/cache/lichii/escape';escaped.symlink_to('../../../config/lichii/qt-config.ini');rejected();escaped.unlink()
    backup=b/'launch-stitch9-before-cpu10.sh';backup.symlink_to(home/'absent-target');rejected();assert not (home/'absent-target').exists();backup.unlink()
    (b/'stitch9-named-jit-once').mkdir();rejected();(b/'stitch9-named-jit-once').rmdir()
    sleepy=home/'stitch-bin';sleepy.symlink_to('/bin/sleep');process=subprocess.Popen([str(sleepy),'30'])
    try:
        time.sleep(.1);result=rejected();assert 'Close the emulator' in result.stderr
    finally:process.terminate();process.wait(timeout=3)
    result=subprocess.run(args,env=env,capture_output=True,text=True,check=True);receipt=json.loads(result.stdout)
    assert receipt['old_profile_unchanged'] and not receipt['jit_run_frame'] and not receipt['emulator_started_or_stopped']
    assert ini.read_text()==original and pergame.read_bytes()==pergame_bytes
    assert (b/'profile10/config/lichii/custom/0400486024DEC000.ini').read_bytes()==pergame_bytes
    c=configparser.ConfigParser(interpolation=None);c.optionxform=str;c.read(b/'profile10/config/lichii/qt-config.ini')
    assert c['Renderer']['resolution_setup']=='3' and c['Renderer']['anti_aliasing']=='1'
    assert c['Controls']['player_0_button_a']=='"binding with spaces é"'
    assert c['UI']['show_whats_new']=='false' and c['UI']['show_whats_new\\default']=='false'
    assert (b/'profile10/data/lichii/link').read_text()=='Public synthetic profile data\n'
    assert (b/'diagnostics/stitch10-fifo-mode').read_bytes()==b'verify\n'
    for name in ['tensor-baseline','stitch9-tensor-baseline']:assert (b/'diagnostics'/name).read_text()=='Keep old experiment state\n'
    for name in new_markers:assert not (b/'diagnostics'/name).exists()
    assert digest(backup)==old and subprocess.run(args,env=env,capture_output=True).returncode!=0
    print('PASS staging: digest, live process, escaping links, dangling backup and armed profiler rejected; exact prior profile/CPU/settings/markers retained; isolated profile10 and rollback9; FIFO verify; JIT frame off',flush=True)
    keys=['XDG_CONFIG_HOME','XDG_DATA_HOME','XDG_CACHE_HOME','PERF_BUILDID_DIR','STITCH_JIT_RUN_FRAME','STITCH_JIT_WARMUP','STITCH_ORDERED_FIFO_MACRO','STITCH_ORDERED_FIFO_MODE_FILE','EDEN_NX2_FAST_DRAW_VERIFY','EDEN_NX2_TEXTURE_MEMO_VERIFY','EDEN_NX2_GPU_PROFILE','MESA_VK_TRACE','MESA_VK_TRACE_TRIGGER']
    captures=['EDEN_NX2_FRAME_DUMP','EDEN_NX2_SHADER_DUMP','EDEN_NX2_PIPELINE_BUILD_TRACE','EDEN_NX2_STUTTER_TRACE','EDEN_NX2_READ_CHECK','EDEN_NX2_DMA_READ_VERIFY','EDEN_NX2_LIVE_READ_CHECK','EDEN_NX2_DELTA_DESCRIPTORS_VERIFY','EDEN_NX2_REJECT_HASH','EDEN_NX2_SPLIT_VERIFY','EDEN_NX2_PERF_VERIFY','EDEN_NX2_CMDPAGE_DIAG','EDEN_NX2_QUERY_DMA_TRACE']
    keys.extend(captures)
    stub=b/'Stitch-OoT-AMD-10-x86_64.AppImage';stub.write_text('#!/usr/bin/python3\nimport os,sys,json\nfrom pathlib import Path\nPath(__file__).with_name("result.json").write_text(json.dumps({"args":sys.argv[1:],"env":{k:os.environ.get(k) for k in '+repr(keys)+'}}))\n');stub.chmod(0o755)
    arguments=['-f','-g','game with spaces and é.nup'];poison=dict(env,**{k:'poison' for k in keys})
    subprocess.run(['bash',str(launcher),*arguments],env=poison,check=True)
    result=json.loads((b/'result.json').read_text());e=result['env'];assert result['args']==arguments
    for k in ['PERF_BUILDID_DIR','STITCH_JIT_RUN_FRAME','STITCH_JIT_WARMUP','EDEN_NX2_FAST_DRAW_VERIFY','EDEN_NX2_TEXTURE_MEMO_VERIFY','EDEN_NX2_GPU_PROFILE','MESA_VK_TRACE','MESA_VK_TRACE_TRIGGER']:assert e[k] is None,(k,e[k])
    for k in captures:assert e[k] is None,(k,e[k])
    for key,suffix in [('XDG_CONFIG_HOME','profile10/config'),('XDG_DATA_HOME','profile10/data'),('XDG_CACHE_HOME','profile10/cache')]:assert e[key]==str(b/suffix)
    assert e['STITCH_ORDERED_FIFO_MACRO']=='verify' and e['STITCH_ORDERED_FIFO_MODE_FILE']==str(b/'diagnostics/stitch10-fifo-mode')
    (b/'stitch10-jit-run-frame').touch();subprocess.run(['bash',str(launcher),*arguments],env=poison,check=True)
    assert json.loads((b/'result.json').read_text())['env']['STITCH_JIT_RUN_FRAME']=='1'
    print('PASS launcher: Unicode/spaced args, isolated roots, profiler/driver poisoning cleared, FIFO master verify, JIT opt-in only on explicit marker',flush=True)
