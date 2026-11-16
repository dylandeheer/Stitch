#!/bin/bash
set -euo pipefail
cp /opt/build-lichii/bin/stitch /work/package-result/stitch10-unstripped
bash /work/cpu-20261111/package.sh > /work/cpu-20261111/package.log 2>&1
python3 /work/cpu-20261111/smoke.py > /work/cpu-20261111/smoke.log 2>&1
python3 /work/launcher_check.py /opt/stitch-package-v10/AppDir stitch > /work/cpu-20261111/package-launchers.log 2>&1
python3 /work/cpu-20261111/package-identity.py > /work/cpu-20261111/package-identity.log 2>&1
