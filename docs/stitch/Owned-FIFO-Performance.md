# Stitch 8 performance experiments

This branch integrates five guarded changes over Stitch 7 and an optional bounded draw diagnostic. **Stable 60 FPS remains unmet.** The quiet Stitch 7 stationary sample recorded 739 frames in 30.02 seconds, 24.6548556 FPS and 47.72 ms p99. The earlier Stitch 5 same-scene early-read ABBA measured a separate 7.66% improvement. Neither run measures Stitch 8 gameplay.

## Guest CPU compare links

On Linux x86-64, `STITCH_OOT_COMPARE_LINK=1` allows Dynarmic to reuse an immediately preceding compare result across one redundant compare-and-branch block. The loader verifies the title, full build ID and two executable pages after mods. The translator checks the current instructions, operands and targets at each eligible site. The original scalar loads, 4 MiB bound, registers, flags and both halt checkpoints are preserved. Modifying either folded instruction invalidates the compiled block. Debugger, single-step and cycle-counted execution use ordinary translation. The x86 fixture passed 165 assertions and 330 private differential scenarios. The candidate's 14 follower blocks accounted for 1.3885% of sampled process cycles.

## Mesh compute fallback

The original task-ownership lookup remains mandatory during pipeline replay. `STITCH_BOUNDED_MESH_TASK_LOOKUP=1` warms an optional bounded variant for eligible task-fed pipelines. With a configured `STITCH_MESH_BASELINE_FILE`, an existing marker selects the original, an absent marker selects bounded, and a filesystem error selects the original. The renderer captures the selection before queueing each dispatch and binds the original whenever the alternate is unavailable. Compare actual original/bounded binds while keeping the same task distribution, emitted geometry and compute fallback.

## Ordered FIFO publication

`STITCH_ORDERED_FIFO_MACRO=verify` enables observation. A regular file at the absolute path in `STITCH_ORDERED_FIFO_MODE_FILE` can request `verify` or `candidate`. A missing or invalid configured file selects baseline. Each certified macro image must first match an ordinary execution. VERIFY compares the original four method writes and six returned FIFO words without repeating any cache or renderer effects.

Deferral requires an owned command span, an eligible unique source page and ordinary tracked state with no pending draw, instance loop or sink. At its ring position the back thread publishes dirty buffer data, then held query reports, then reads all six words and applies the computed render mode. The front joins publication before leaving the command span or reaching a front-side observer. A later command resnapshot therefore sees published bytes. Mode and shadow readers reconcile with the back before use. A verification mismatch latches the candidate off. Publication-group waits and actual deferrals are the relevant counters.

## Scalar atlas initialization and blend state

`STITCH_R32_ZERO_BASELINE_FILE` enables evidence collection for the native 4096² R32 atlas. An existing marker keeps the ordinary upload. An absent marker permits a fresh, backed, color-only R32_SFLOAT image to use a transfer clear. Metadata must cover all 64 MiB and the complete host subresource. Every owned source run, including fallback reads, must contain only zero bits. Negative zero, NaNs and other mixed inputs use normal detiling and upload, reconstructing only previously verified zero bytes without rereading guest memory. Scratch stays bounded at 4 MiB. R32 uniformity and savings in the game remain unmeasured. The D16 path retains its independent control.

The iterated-blend enable register now marks both baked and dynamic blend equations dirty when written. The squashed-blend override currently serves Ninja Gaiden Ragebound. Ordinary draws incur no additional attachment scan.

## Bounded draw CPU diagnostic

`STITCH_DRAW_CPU_FILE` names a marker that arms one ten-second window. Without the variable, no probe is created. An absent marker leaves it inactive; marker checks occur at most once per sixteen presented frames. The split back executor samples approximately one draw in128 with a rotating phase, using the current thread CPU clock. Up to64 stage-hash groups plus labelled unknown/overflow rows report non-overlapping state, lookup, geometry, stage-buffer, target/feedback and enqueue/record zones, diagnostic coverage cost and an unclassified remainder. Driver-worker and blocked wall time are not measured.

Eligibility counters compare the complete pipeline key and separately check channel, draw kind, render-pass epoch/tick, ordinary direct draw shape and the absence of image descriptors. These counters identify possible reuse opportunities; they do not prove complete cache invalidation or implement a draw shortcut. No renderer command or resource update is bypassed. The ordinary first launcher leaves this diagnostic off.
