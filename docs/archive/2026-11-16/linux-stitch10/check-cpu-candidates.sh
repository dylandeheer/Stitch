#!/bin/bash
set -euo pipefail
cd /opt/build-lichii
bin/tests '[jit_run_frame],[oot_compare_link],[jit_warmup],[jit_ordered_loads],[split_progress_wait],[gpu_split_dependencies],[query_fence_lifetime],[read_trap_protection]' > /work/cpu-20261111/cpu-focused.log 2>&1
encoding_dir=$(mktemp -d /tmp/stitch-run-frame-encoding.XXXXXX)
trap 'rm -rf "$encoding_dir"' EXIT
PERF_BUILDID_DIR="$encoding_dir" bin/tests '[.jit_run_frame_encoding]' -s > /work/cpu-20261111/jit-encoding.log 2>&1
timeout 40 bin/tests '[.jit_run_frame_signal]' -s > /work/cpu-20261111/jit-signal.log 2>&1
