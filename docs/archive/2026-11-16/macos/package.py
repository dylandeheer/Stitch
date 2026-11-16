from pathlib import Path
import hashlib,json,os,plistlib,re,shutil,subprocess,sys

t=Path(__file__).resolve().parent;w=t.parent;build=w/'build-stitch-mac'
staged=Path((t/'staging-path.txt').read_text().strip()) if (t/'staging-path.txt').exists() else t/'package/Stitch.app'
staged.parent.mkdir(exist_ok=True)
program=staged/'Contents/MacOS/stitch'
qt=t/'qt/6.11.1/macos'
payload=program.with_name('stitch-bin')
if not staged.exists():
    shutil.copytree(build/'bin/stitch.app',staged,symlinks=True)
    assert program.is_file()
    subprocess.run([str(qt/'bin/macdeployqt'),str(staged),'-always-overwrite','-no-strip','-verbose=1'],check=True)
    program.rename(payload)
else:
    assert '--resume' in sys.argv and payload.is_file() and not (t/'package-receipt.json').exists(), 'Inspect existing package before replacing'
shutil.copy2(t/'stitch-launcher',program.with_name('stitch-launcher'))
# macOS seals text resources separately from the MacOS code directory.
switches=staged/'Contents/MacOS/nx2_switches.txt'
if switches.exists():
    switches.replace(staged/'Contents/Resources/nx2_switches.txt')
info=staged/'Contents/Info.plist';raw=info.read_bytes();p=plistlib.loads(raw[raw.index(b'<?xml'):])
p.update(CFBundleExecutable='stitch-launcher',CFBundleName='Stitch',CFBundleDisplayName='Stitch',
         CFBundleIdentifier='io.github.dylandeheer.stitch',CFBundleVersion='1',
         CFBundleShortVersionString='1.0',LSMinimumSystemVersion='26.0',
         NSDocumentsFolderUsageDescription='Read the game backup you selected for Stitch.')
info.write_bytes(plistlib.dumps(p))
licenses=staged/'Contents/Resources/licenses';licenses.mkdir(parents=True,exist_ok=True)
shutil.copy2(w/'lichii/LICENSE.txt',licenses/'Stitch-GPL-3.0.txt')

def macho(path):
    if path.is_symlink() or not path.is_file():return False
    with path.open('rb') as f:magic=f.read(4)
    return magic in [b'\xcf\xfa\xed\xfe',b'\xfe\xed\xfa\xcf',b'\xca\xfe\xba\xbe',b'\xbe\xba\xfe\xca',b'\xca\xfe\xba\xbf']

for attribute in ['com.apple.FinderInfo','com.apple.ResourceFork']:
    cleaned=subprocess.run(['xattr','-dr',attribute,str(staged)],capture_output=True,text=True)
    if cleaned.returncode:
        assert all('Attribute not found' in line for line in cleaned.stderr.splitlines()),cleaned.stderr
binaries=[p for p in staged.rglob('*') if macho(p)]
for path in binaries:
    arch=subprocess.check_output(['lipo','-archs',str(path)],text=True).split()
    assert 'arm64' in arch,(path,arch)
    if len(arch)>1:
        thin=path.with_name(path.name+'.arm64')
        subprocess.run(['lipo',str(path),'-thin','arm64','-output',str(thin)],check=True)
        thin.chmod(path.stat().st_mode);thin.replace(path)
    load=subprocess.check_output(['otool','-l',str(path)],text=True)
    rpaths=re.findall(r'cmd LC_RPATH\n\s*cmdsize \d+\n\s*path (.*?) \(offset',load)
    for rpath in rpaths:
        if rpath.startswith(('/Users/','/opt/homebrew')):
            subprocess.run(['install_name_tool','-delete_rpath',rpath,str(path)],check=True)
    if path==payload and '@executable_path/../Frameworks' not in rpaths:
        subprocess.run(['install_name_tool','-add_rpath','@executable_path/../Frameworks',str(path)],check=True)
    # Reject runtime dependencies on this build tree or a Homebrew installation.
    deps=subprocess.check_output(['otool','-L',str(path)],text=True)
    for line in deps.splitlines()[1:]:
        dep=line.strip().split(' (',1)[0]
        assert not dep.startswith(('/opt/homebrew','/Users/')),(path,dep)
for path in sorted(binaries,key=lambda p:len(p.parts),reverse=True):
    # Signing the primary executable signs the enclosing bundle as well. Do it last,
    # after every nested library and the actual JIT payload have signatures.
    if path==program.with_name('stitch-launcher'):continue
    args=['codesign','--force','--sign','-','--timestamp=none']
    if path in [payload,program.with_name('stitch-launcher')]:
        args+=['--options','runtime','--entitlements',str(t/'jit.entitlements.plist')]
    subprocess.run([*args,str(path)],check=True)
for framework in sorted(staged.rglob('*.framework'),key=lambda p:len(p.parts),reverse=True):
    if not framework.is_symlink():subprocess.run(['codesign','--force','--sign','-','--timestamp=none',str(framework)],check=True)
subprocess.run(['codesign','--force','--sign','-','--timestamp=none','--options','runtime',
                '--entitlements',str(t/'jit.entitlements.plist'),str(staged)],check=True)
subprocess.run(['codesign','--verify','--deep','--strict','--verbose=2',str(staged)],check=True)
ent=subprocess.run(['codesign','-d','--entitlements',':-',str(payload)],capture_output=True,text=True,check=True)
assert 'com.apple.security.cs.allow-jit' in ent.stdout
revision=subprocess.check_output(['git','rev-parse','HEAD'],cwd=w/'lichii',text=True).strip()
assert revision==json.loads((t/'source-freeze.json').read_text())['revision']
assert revision.encode() in payload.read_bytes()
receipt={'source_revision':revision,'qt':'6.11.1','architecture':'arm64','mach_o_files':len(binaries),
         'payload_sha256':hashlib.sha256(payload.read_bytes()).hexdigest(),
         'launcher_sha256':hashlib.sha256(program.with_name('stitch-launcher').read_bytes()).hexdigest(),
         'bundle_signature':'deep strict verification passed','jit_entitlement_on_actual_payload':True,
         'external_build_prefix_runtime_dependencies':False,'app':str(staged)}
(t/'package-receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
print(json.dumps(receipt,indent=2))
