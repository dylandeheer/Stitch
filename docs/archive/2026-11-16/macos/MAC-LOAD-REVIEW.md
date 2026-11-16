# Native Mac load and settled-world review

2026-11-11. Read-only review of source `c234ec16`, the private load/quiet process samples, and the local runtime log. No app, source, configuration, or running-session changes were made. Raw guest addresses and shader data remain in the private evidence directory.

## Result

The copied Kokiri Forest save does load and rendering appears correct according to the parent agent’s observed screenshot. The parent measured **4.199 FPS** over a 30-second quiet reading (125 frames; frame-time p50 **237.97 ms**, p99 **248.40 ms**). This is a working native baseline, not acceptable gameplay performance.

The loading stalls and the settled-world bottleneck differ. Neither sample supports describing the entire delay as ordinary shader warmup.

## Settled world: prioritize readback and queue completion

`private/kokiri-quiet-sample.txt` contains 1,494 samples per thread over roughly 1.5 seconds. Counts below are per-thread inclusive samples; nested counts must not be added.

- All nine shader CPU workers and all five shader driver workers are asleep. The Mach exception handler spends all 1,494 samples in its message exchange paths, with no sampled FastmemCallback handling. The earlier fault/recompile storm is absent from this snapshot.
- GPUBack spends **443/1,494** samples in `TickWork → FlushRegionImpl → BufferCache::DownloadRangesMemory → BufferCacheRuntime::FinishForHostRead → Scheduler::Finish/Wait → MasterSemaphore::Wait → MoltenVK timeline semaphore wait`. Another 12 samples reach the same wait through draw preparation and buffer download. GPUBack also waits for its own work queue in 815 samples.
- The GPU front thread spends at least 445 samples draining the split backend during acquire resumption. CPU cores 0–3 largely idle; cores 4/5 show guest reads entering rasterizer download and CPU-read synchronization (159 and 205 samples in the main `OnCPURead` wait branches). This is a visible dependency chain rather than six saturated emulation cores.
- VulkanWorker is asleep in **1,316/1,494** samples, with 141 in queue submission; GPUFencingThread spends 905 in timeline semaphore waits. Sleeping recording workers do not prove the physical GPU is idle.

**Next measured target:** the synchronous buffer-readback/queue-completion chain. Establish which readbacks force full completion, their frequency and bytes per frame, and whether the wait is expensive submitted GPU work or preventable ordering/flush serialization. A process sample cannot identify a costly GPU kernel or prove that removing a wait is correct. Preserve ownership and readback correctness; do not disable synchronization on this evidence alone. The sample does not justify a definitive mesh/tensor GPU-kernel diagnosis.

## Initial load gap: CPU JIT faults and compilation

`private/game-load-sample.txt` is an earlier 1,501-sample snapshot taken roughly 205 seconds after launch, during a prolonged loading gap. GPU, GPUBack, VulkanWorker, and all shader CPU/driver workers are asleep for the full snapshot. Guest cores instead repeatedly enter Dynarmic `GetOrEmit`, IR generation, ARM64 emission and register allocation. The Mach handler has 142 samples in `FastmemCallback`, including 53 in basic-block invalidation/relink paths. These observations point to CPU-side JIT fault/recompilation work during that gap, rather than active Metal shader compilation.

Apple fastmem maps 16 KB host pages. In `src/common/host_memory.cpp`, `ResolveHostPage` requires all four 4 KB guest subpages to exist, be contiguous in backing storage, and begin at a host-page-aligned backing offset; protection is the intersection of their permissions. A guest mapping that fails these conditions becomes an inaccessible fastmem host page. Dynarmic fault handling marks the particular memory instruction as unsuitable for fastmem, invalidates its block and recompiles it to use a fallback. Each emulated core has its own JIT state. This is a plausible source of substantial cold cost across many instructions, but the sample does not show fault addresses or establish a repeated-fault bug. The exclusion set persists across code-cache clearing. Only one sampled host-page-update path was seen, so direct mmap/mprotect cost is not established as dominant.

A secondary observed JIT compilation cost is ARM64 register-allocation checking: `RegAlloc::AssertAllUnlocked`/`AssertNoMoreUses` use the project’s always-active `ASSERT`, whereas `DYNARMIC_IGNORE_ASSERTS` defines the MCL assertion flag. Consequently the release setting does not remove those particular checks. Their scans appear in the load sample. This warrants bounded measurement, not blanket assertion removal, and is not the current settled-world bottleneck.

## Cold shader work also occurs between the gaps

Later log bursts do build new pipelines. One ten-second report records 42 builds and about 2.99 seconds of draws held for unbuilt pipelines. That is real cold shader cost, but all shader workers were idle in the earlier load-gap sample and again in the settled-world sample. Runtime backing-memory remap messages also appear during loading; their connection to the JIT faults is not proven.

## Configuration caveat

Under Auto CPU accuracy, setting only `cpuopt_fastmem=false` does not disable fastmem: that switch is consulted under Debugging accuracy. Changing to Debugging also changes other behavior, including IR verification in this fork, and therefore is not a clean performance comparison. No configuration changes are proposed or applied by this review.
