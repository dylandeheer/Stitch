# Stitch performance work

The current development line starts from Lichii `d5494116902d15f9616efbe7668bbcec33a93f9c`, with task/mesh fallback PR 569 (`8745ba3424f73cf0e7a548d6797e2c471afd7fe2`) integrated at `fb06779b2edd9f45b98d33db96bdf1724dc6c14d`. This is a pinned experimental base, not a claim to track the latest upstream nx2. Broad upstream integration needs its own rendering and regression checks.

## Established changes

- AMD compute mesh fallback retains per-primitive outputs and restores normal vertex/input state after fallback draws.
- Cached compatible mesh pipelines follow the normal warmup path. The test machine prebuilt 336 fallback pipelines at startup.
- Collector readbacks are batched, preserving ordering and staging lifetime ([Lichii PR 376](https://github.com/zortos293/Lichii/pull/376)).
- The initial guest upload is omitted only when a proven full alias copy replaces every destination mip and layer ([PR 288](https://github.com/zortos293/Lichii/pull/288), conservative port).
- Zero-input mesh draws can bypass setup while preserving query-slot boundaries. Merging non-precise sample-query slots is incorrect and is rejected by regression checks.
- Linux cold-memory hints use MADV_COLD ([PR 756](https://github.com/zortos293/Lichii/pull/756), `c1c26313b1da663fef2833ca3a9fa8d63fb77cd1` by Robert Veloso), preserving memory contents and allowing the kernel to reclaim under pressure. The prior code requested immediate swap-out. Stitch gameplay validation of this change is pending.

## Current priorities

1. Measure repeated-route texture upload hitches after the Linux reclaim fix. Large D16 refreshes are a confirmed host-thread cost; their read, detile and staging phases need separate attribution before designing a replacement.
2. Port the conservative physical-writer frontier from [PR 805](https://github.com/zortos293/Lichii/pull/805), commit `1d9b6ef31d7c271fcc955094cf37806478e2e3c6`, as a separate experiment. Unknown writers, remaps, pending draws and query locks must keep the conservative path. QA must compare against fully drained reads.
3. Evaluate image reuse only after measuring Navi33 allocation/destruction costs. Recycled Vulkan images must be retired behind the correct GPU tick, keyed by exact compatible parameters, accounted at actual resident size, and initialized before sampling.
4. Consider the decoder in [PR 784](https://github.com/zortos293/Lichii/pull/784) if target measurements show conversion dominates. Synthetic CPU throughput does not establish game FPS.

The broader survey also checked PRs 243, 267, 559, 692, 717, 752, 777, 802, 817, 818 and 836, related Eglantine work, and a PCSX2 target-lifetime example. Upstream performance claims apply to their stated hardware and test scenes. Speculative query releases, stale-content skips and shape-based shadow heuristics are excluded from the current branch.

## Diagnostics and validation

`EDEN_NX2_MESH_PERF=1` enables sampled fallback compute and graphics timestamps and layout counts. `EDEN_NX2_MESH_PERF_FILE` optionally controls sampling by marker presence, polled every 30 presentations. Full DKB and GPU_PROFILE remain separate; GPU_PROFILE changes command-processing overhead and must not be used for an ordinary FPS comparison.

`EDEN_NX2_UPLOAD_TIMING=1` logs host-wall refreshes and collector calls of at least 2 ms. Linux upload records include thread page-fault deltas captured before logging. The image footprint is not necessarily bytes transferred, and GPU timestamps perturb sampled work.

Run the normal CTest targets and the production-method regressions `python3 tools/nx2/check_cold_hint.py` (Linux), `python3 tools/nx2/check_empty_mesh_queries.py`, and `python3 tools/nx2/check_mesh_perf.py`. Packaged builds also need a GUI loader check, launcher checks and actual owner gameplay validation. Keep captures and game/system assets private.
