from pathlib import Path
import hashlib,json,subprocess,tempfile
w=Path('/work/cpu-20261111');p=Path('/opt/stitch-package-v10/AppDir')
freeze=json.loads((w/'source-freeze.json').read_text())
image=w.parent/'package-result/Stitch-OoT-AMD-10-x86_64.AppImage'
expected=['STITCH_OOT_COMPARE_LINK','STITCH_BOUNDED_MESH_TASK_LOOKUP','STITCH_MESH_BASELINE_FILE',
          'STITCH_ORDERED_FIFO_MODE_FILE','STITCH_R32_ZERO_BASELINE_FILE','STITCH_DRAW_CPU_FILE','STITCH_JIT_RUN_FRAME']
with tempfile.TemporaryDirectory(prefix='stitch10-image-identity-') as tmp:
    subprocess.run([str(image),'--appimage-extract'],cwd=tmp,check=True,stdout=subprocess.DEVNULL,timeout=40)
    extracted=Path(tmp)/'squashfs-root'
    payload=extracted/'usr/bin/stitch-bin'
    b=payload.read_bytes()
    assert b[:4]==b'\x7fELF'
    assert hashlib.sha256(b).digest()==hashlib.sha256((p/'usr/bin/stitch-bin').read_bytes()).digest()
    for item in [freeze['revision'],*expected]:
        assert item.encode() in b,item
        print('PASS packaged binary identity '+item)
    for name in ['ShaderCacheV2', 'STITCH_JIT_WARMUP']:
        assert name.encode() in b, name
    for name in ['stitch','eden-cli']:
        assert (extracted/'usr/bin'/name).read_bytes()==(p/'usr/bin'/name).read_bytes()
    assert (extracted/'usr/bin/stitch-bin').stat().st_size>10_000_000
    print('PASS exact AppImage extracted GUI payload and both safe-AMD wrappers')
r={'source_revision':freeze['revision'],'sha256':hashlib.sha256(image.read_bytes()).hexdigest(),
   'filename':image.name,'bytes':image.stat().st_size,'extracted_gui_sha256':hashlib.sha256(b).hexdigest(),
   'validated_controls':expected}
(w/'package-receipt.json').write_text(json.dumps(r,indent=2)+'\n')
print(json.dumps(r))
