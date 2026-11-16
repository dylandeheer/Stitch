#!/bin/bash
set -euo pipefail
export PATH=/opt/build-venv/bin:$PATH
cmake -S /work/lichii -B /opt/build-lichii
cmake --build /opt/build-lichii --parallel 8
ctest --test-dir /opt/build-lichii --output-on-failure
/opt/build-lichii/bin/tests '[jit_run_frame],[jit_ordered_loads],[read_trap_protection],[split_progress_wait],[oot_compare_link],[jit_warmup],[instruction_page],[ordered_fifo],[ordered_fifo_ring],[maxwell_fast_method],[gpu_split_dependencies],[semaphore_acquire_order],[query_fence_lifetime],[semaphore_mirror_remap],[sysmix],[buffer_ports],[early_buffer],[atlas_upload_evidence],[zero_depth_upload],[zero_atlas_upload],[mesh_fallback],[texture_cache]'
