from datetime import datetime,timezone
from pathlib import Path
import configparser,hashlib,json,os,shutil,sys,threading,uuid

b=Path.home()/'Applications/Lichii/oot-amd-candidate-20261107'
name='Stitch-OoT-AMD-10-x86_64.AppImage'
image_sha,revision,launcher_sha,old_launcher_sha=sys.argv[1:]
assert len(revision)==40 and all(c in '0123456789abcdef' for c in revision)
for value in [image_sha,launcher_sha,old_launcher_sha]:assert len(value)==64 and all(c in '0123456789abcdef' for c in value)
digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
launcher=b/'launch-lichii.sh';upload=b/(name+'.upload');next_launcher=b/'launch-stitch10.sh.upload'
for p in [b,b/'diagnostics',launcher,upload,next_launcher]:assert p==p.resolve(strict=True),'Unexpected staging symlink'
assert digest(launcher)==old_launcher_sha
assert digest(upload)==image_sha and digest(next_launcher)==launcher_sha
assert not (b/name).exists() and not (b/'profile10').exists() and not (b/'user').exists()
backup=b/'launch-stitch9-before-cpu10.sh'
assert not os.path.lexists(backup) and not os.path.lexists(b/'stitch-10.json')
assert launcher.read_text().count('Stitch-OoT-AMD-9-x86_64.AppImage')==1
assert next_launcher.read_text().count(name)==1
assert not (b/'stitch9-named-jit-once').exists(),'Disarm the one-time source9 profiler first'
assert not (b/'stitch10-jit-run-frame').exists(),'First owner run keeps the optional JIT frame off'
assert not (b/'nx2_switches.txt').exists() and not (b/'profile9/config/lichii/nx2_switches.txt').exists(),'Inspect runtime switch overrides before staging'

def emulators():
    found=[];names={'stitch','stitch-bin','lichii','lichii-bin','eden-cli','eden-cli-bin'}
    for p in Path('/proc').iterdir():
        if not p.name.isdigit():continue
        try:
            exe=Path(os.readlink(p/'exe')).name.removesuffix(' (deleted)')
            if exe in names or (p/'comm').read_text().strip() in names:found.append(int(p.name))
        except(OSError,ValueError):pass
    return found

assert not emulators(),'Close the emulator before copying its profile'
roots=['config','data','cache']
for root in roots:
    source=b/'profile9'/root/'lichii'
    assert source.is_dir()
    for parent in [source,*source.parents]:assert not parent.is_symlink(),'Symlinked profile ancestor'
    for p in source.rglob('*'):
        if p.is_symlink():
            assert root!='config','Configuration aliases must be resolved before an isolated migration'
            assert not Path(os.readlink(p)).is_absolute() and p.resolve().is_relative_to(source),'Profile symlink escapes isolated root'
ini_names=['qt-config.ini','custom/0400486024DEC000.ini']
old_ini={p:digest(b/'profile9/config/lichii'/p) for p in ini_names}

def metadata(root):
    records={}
    for p in root.rglob('*'):
        st=p.lstat();rel=p.relative_to(root).as_posix()
        if p.is_symlink():records[rel]=('link',os.readlink(p))
        elif p.is_file():records[rel]=('file',st.st_size,st.st_mtime_ns)
        elif p.is_dir():records[rel]=('dir',)
        else:raise RuntimeError('Unsupported node: '+rel)
    return records

before={root:metadata(b/'profile9'/root/'lichii') for root in roots}
stop=threading.Event();interrupted=threading.Event()
def watch():
    while not stop.wait(.05):
        if emulators():interrupted.set();return
monitor=threading.Thread(target=watch,daemon=True);monitor.start()
prepared=b/('profile10.preparing-'+uuid.uuid4().hex);prepared.mkdir()
try:
    def copy(source,destination):
        if interrupted.is_set():raise RuntimeError('Emulator started during copy')
        return shutil.copy2(source,destination)
    for root in roots:shutil.copytree(b/'profile9'/root/'lichii',prepared/root/'lichii',symlinks=True,copy_function=copy)
    assert not interrupted.is_set() and not emulators()
    for root in roots:
        assert metadata(b/'profile9'/root/'lichii')==before[root],'Source profile changed during copy'
        assert metadata(prepared/root/'lichii')==before[root],'Copied profile differs'
    assert {p:digest(prepared/'config/lichii'/p) for p in ini_names}==old_ini
    # Only suppress the new startup information dialog in this separate Steam test profile.
    ini=prepared/'config/lichii/qt-config.ini';raw=ini.read_bytes().decode('utf-8')
    def parsed(text):
        c=configparser.ConfigParser(interpolation=None,strict=False);c.optionxform=str;c.read_string(text)
        return {(section,key):value for section in c.sections() for key,value in c.items(section)}
    values={'show_whats_new':'false','show_whats_new\\default':'false'}
    lines=raw.splitlines(keepends=True);out=[];section='';seen=set();inserted=False
    def append_missing():
        if out and not out[-1].endswith(('\n','\r')):out[-1]+='\n'
        for key,value in values.items():
            if key not in seen:out.append(key+'='+value+'\n')
    for line in lines:
        text=line.strip()
        if text.startswith('[') and text.endswith(']'):
            if section=='UI':append_missing();inserted=True
            section=text[1:-1]
        key=line.partition('=')[0].strip()
        if section=='UI' and key in values and '=' in line:
            out.append(key+'='+values[key]+'\n');seen.add(key)
        else:out.append(line)
    if section=='UI':append_missing();inserted=True
    if not inserted:
        if out and not out[-1].endswith(('\n','\r')):out[-1]+='\n'
        out.extend(['\n[UI]\n',*(key+'='+value+'\n' for key,value in values.items())])
    text=''.join(out);prior=parsed(raw);after=parsed(text);allowed={('UI',key) for key in values}
    assert {k:v for k,v in prior.items() if k not in allowed}=={k:v for k,v in after.items() if k not in allowed}
    for key,value in values.items():assert after[('UI',key)]==value
    ini.write_bytes(text.encode('utf-8'))
    assert digest(prepared/'config/lichii/custom/0400486024DEC000.ini')==old_ini['custom/0400486024DEC000.ini']
    assert {p:digest(b/'profile9/config/lichii'/p) for p in ini_names}==old_ini
    assert not interrupted.is_set() and not emulators()
    prepared.replace(b/'profile10')
finally:
    stop.set();monitor.join(timeout=2)
    if prepared.exists():shutil.rmtree(prepared)

assert not emulators(),'Emulator started before launcher publication; original launcher retained'
shutil.copy2(launcher,backup)
for marker in ['tensor-baseline','early-read-baseline','atlas-pread-baseline','d16-zero-baseline','r32-baseline','mesh-empty-baseline','texture-gc-baseline','join-upload-baseline']:
    (b/'diagnostics'/('stitch10-'+marker)).unlink(missing_ok=True)
mode=b/'diagnostics/stitch10-fifo-mode';temporary=mode.with_name(mode.name+'.preparing-'+uuid.uuid4().hex)
with temporary.open('x') as out:out.write('verify\n')
temporary.replace(mode)
upload.chmod(0o755);upload.replace(b/name)
next_launcher.chmod(0o755);next_launcher.replace(launcher)
assert digest(launcher)==launcher_sha and digest(backup)==old_launcher_sha
receipt={'staged_at':datetime.now(timezone.utc).isoformat(),'filename':name,'sha256':image_sha,
         'bytes':(b/name).stat().st_size,'source_revision':revision,'launcher_sha256':launcher_sha,
         'profile':'profile10, copied from profile9; both use lichii XDG folders',
         'original_ini_sha256':old_ini,'old_profile_unchanged':True,'copied_nodes':sum(len(v) for v in before.values()),
         'renderer_and_per_title_settings_copied_byte_exact':True,
         'new_profile_changed_settings':['UI startup Whats new disabled; all other settings retained'],
         'explicit_storage_paths':'retained; later saves can use configured shared paths',
         'initial_mode':'owned FIFO verification only; manual owner rendering QA pending',
         'fifo_mode_file':str(mode),'jit_run_frame':False,'named_jit_profile':False,
         'emulator_started_or_stopped':False,'rollback_launcher':str(backup),'owner_performance_validation':'pending'}
with (b/'stitch-10.json').open('x') as out:out.write(json.dumps(receipt,indent=2)+'\n')
print(json.dumps(receipt,indent=2))
