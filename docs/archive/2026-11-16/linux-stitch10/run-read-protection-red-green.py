from pathlib import Path
import hashlib,os,subprocess

t=Path(__file__).resolve().parent;w=t.parent;repo=t/'source'
patch=t/'upstream/read-protection-core-green.patch'
source=repo/'src/core/memory.cpp'
original=hashlib.sha256(source.read_bytes()).hexdigest()
env=dict(os.environ,LIMA_HOME=str(w/'lm'))
prefix=[str(w/'tooling/lima/bin/limactl'),'shell','b','--','sudo','chroot','/opt/lichii-amd64']

def build(name):
    with (t/(name+'-build.log')).open('w') as log:
        subprocess.run([*prefix,'/bin/bash','-c','export PATH=/opt/build-venv/bin:$PATH; cmake --build /opt/build-lichii --target tests --parallel 8'],env=env,stdout=log,stderr=subprocess.STDOUT,check=True)

def run(name):
    with (t/(name+'-test.log')).open('w') as log:
        result=subprocess.run([*prefix,'/opt/build-lichii/bin/tests','[read_trap_protection]'],env=env,stdout=log,stderr=subprocess.STDOUT)
    return result.returncode,(t/(name+'-test.log')).read_text()

subprocess.run(['git','apply','--reverse','--check',str(patch)],cwd=repo,check=True)
subprocess.run(['git','apply','--reverse',str(patch)],cwd=repo,check=True)
try:
    print('Building original cached-reprotection behavior with the new deterministic fixture',flush=True)
    build('read-protection-red')
    code,report=run('read-protection-red')
    assert code!=0 and 'probe.readable[1]' in report and 'FAILED' in report,report[-5000:]
    print('PASS negative control: original behavior made the armed page readable',flush=True)
finally:
    subprocess.run(['git','apply',str(patch)],cwd=repo,check=True)
    assert hashlib.sha256(source.read_bytes()).hexdigest()==original
print('Restored exact candidate source; building and running corrected behavior',flush=True)
build('read-protection-green')
code,report=run('read-protection-green')
assert code==0 and 'All tests passed' in report,report[-5000:]
print(report,flush=True)
