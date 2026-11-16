from pathlib import Path
import hashlib,json,re,subprocess

t=Path(__file__).resolve().parent;w=t.parent;repo=t/'source'
base='/home/deck/Applications/Lichii/oot-amd-candidate-20261107';host='deck@$STEAM_HOST'
options=['-i',str(w/'steamos_ed25519'),'-o','IdentitiesOnly=yes','-o','BatchMode=yes',
         '-o','ConnectTimeout=5','-o','ConnectionAttempts=1','-o','StrictHostKeyChecking=yes',
         '-o','UserKnownHostsFile='+str(w/'steam-ssh-known_hosts')]
freeze=json.loads((t/'source-freeze.json').read_text());revision=freeze['revision']
assert subprocess.check_output(['git','rev-parse','HEAD'],cwd=repo,text=True).strip()==revision
assert not subprocess.check_output(['git','status','--porcelain','--untracked-files=no'],cwd=repo)
digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
package=json.loads((t/'package-receipt.json').read_text());image=w/'package-result'/package['filename']
launcher=t/'launch-stitch10.sh'
assert package['source_revision']==revision and digest(image)==package['sha256']
build=(t/'build-all.log').read_text()
assert '100% tests passed, 0 tests failed out of 9' in build
m=re.search(r'All tests passed \((\d+) assertions in (\d+) test cases\)',build)
assert m and int(m[1])>=50_000_000 and int(m[2])>=200
for name in ['smoke.log','package-launchers.log','package-identity.log','stage-launch-tests.log']:
    assert 'PASS' in (t/name).read_text(),name
for name in ['jit-encoding.log','jit-signal.log','cpu-focused.log']:
    assert 'All tests passed' in (t/name).read_text(),name
assert 'PASS negative control' in (t/'read-protection-red-green.log').read_text()

snapshot='''from pathlib import Path
import hashlib,json,os
b=Path.home()/'Applications/Lichii/oot-amd-candidate-20261107'
digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
files=['profile9/config/lichii/qt-config.ini','profile9/config/lichii/custom/0400486024DEC000.ini']
settings={p:digest(b/p) for p in files}
names={'stitch','stitch-bin','lichii','lichii-bin','eden-cli','eden-cli-bin'};pids=[]
for p in Path('/proc').iterdir():
    if not p.name.isdigit():continue
    try:
        if Path(os.readlink(p/'exe')).name.removesuffix(' (deleted)') in names or (p/'comm').read_text().strip() in names:pids.append(int(p.name))
    except OSError:pass
extra={}
for f in ['Stitch-OoT-AMD-10-x86_64.AppImage','launch-stitch9-before-cpu10.sh','stitch-10.json']:
    if (b/f).exists():extra[f]=digest(b/f)
print(json.dumps({'settings':settings,'running_pids':pids,'launcher_sha256':digest(b/'launch-lichii.sh'),
                  'source9_image_sha256':digest(b/'Stitch-OoT-AMD-9-x86_64.AppImage'),'extra':extra}))
'''

def remote(script,args=()):
    assert all(a and all(c in '0123456789abcdef' for c in a) for a in args)
    result=subprocess.run(['ssh',*options,host,'python3 - '+' '.join(args)],input=script,text=True,capture_output=True,timeout=300)
    if result.returncode:raise RuntimeError(result.stderr)
    return result.stdout

before=json.loads(remote(snapshot))
assert not before['running_pids'],'Owner game is running; existing launch and profile retained'
assert before['launcher_sha256']=='7f58d137b2e70bd8c627957356389950295d257717a6ce6c842a422cc17ee0e8','Inspect changed source9 launcher before staging'
assert before['source9_image_sha256']=='6247f39ba4fba3994a73037e38593e0b60a6e0d539bfc77ff9ab8871b819301c'
assert not before['extra'],'Existing source10 stage requires inspection'
(t/'before-stage.json').write_text(json.dumps(before,indent=2)+'\n')
for source,target in [(image,image.name+'.upload'),(launcher,'launch-stitch10.sh.upload'),
                      (w/'package-result/stitch10-unstripped','diagnostics/stitch10-unstripped')]:
    subprocess.run(['scp',*options,str(source),host+':'+base+'/'+target],check=True,timeout=300)
    print('Uploaded '+source.name,flush=True)
receipt=json.loads(remote((t/'stage.py').read_text(),[digest(image),revision,digest(launcher),before['launcher_sha256']]))
(t/'stage-receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
after=json.loads(remote(snapshot))
assert after['settings']==before['settings'] and after['source9_image_sha256']==before['source9_image_sha256']
assert after['launcher_sha256']==digest(launcher) and after['extra'][image.name]==digest(image)
assert after['extra']['launch-stitch9-before-cpu10.sh']==before['launcher_sha256']
assert receipt['source_revision']==revision and receipt['old_profile_unchanged']
after.update(profile_hashes_unchanged=True,stage_receipt=receipt,before_running_pids=before['running_pids'])
(t/'stage-verification.json').write_text(json.dumps(after,indent=2)+'\n')
print(json.dumps(after,indent=2))
