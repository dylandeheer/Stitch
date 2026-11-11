# CPU performance candidates after Stitch 9

The clean Stitch 9 stationary reading was 17.1306 FPS over 30 seconds, with a 70.27 ms p99 frame interval and 46.9% mean GPU busy time. Docked 1× and FXAA were retained. A separate 20-second, 21,661-sample user-cycle profile put 68.12% of cycles on the six guest CPU threads and 25.51% on the graphics command front/back threads. `GpuSplit::Backoff` accounted for 6.24% of total sampled user cycles. These concurrent CPU-cycle shares are not an additive breakdown of frame latency.

Much of the guest code was unnamed. Historical, independently checked captures identified a Zstandard-compatible sequence-expansion routine and two bounded string-comparison search arms. Their old percentages are not current-build measurements. The owner deferred a fresh named profile; the one-time profiler was disarmed and the exact normal Stitch 9 launcher restored. Identifying current guest hot routines remains pending.

## Changes

The split waits now perform a bounded short spin locally and park for longer waits. Full drains request the deferred-submit handoff before publishing their final batch, and wait for actual idle-hook completion. Ring reservations and partial writer waits publish the exact completion position they need once, while retaining periodic and batch-end progress. The actual consumed position and quiet state remain the completion conditions. Borrowed command data, writer ordering and FIFO publication joins retain their existing guards.

The wait-trace probe also handles an already-consumed ring with an outstanding idle hook. The previous probe could finish a wait without having selected a counter row. A real UBSan negative control reproduced that null access.

`STITCH_JIT_RUN_FRAME=1` enables an optional Linux x86-64 A64 dispatcher frame. One real SysV frame surrounds each Run or Step, replacing three frame instructions per translated block. The inner callee-save and StackLayout offsets remain unchanged. This preserves a native frame-pointer chain; it does not introduce DWARF exception-unwinding metadata. The option is off by default and excluded for debugger, Debugging and Paranoid configurations.

The read-protection correction addresses the recache window described in upstream issue 1381. An armed GPU-read page is never temporarily granted base read permission while cached protection is rebuilt. Protection changes and page-type publication serialize with arming/disarming. Uncaching retains its read/write policy. Other pre-existing slow-read/download landing questions remain separate; this is not a claim to have proved the whole memory protocol.

## Upstream

The upstream snapshot is Lichii `d5bb40ad4ca16b66e9c0961f823aff5f2a8c5c08`, six mainline commits after Stitch 9's `23a6a56c`. It includes unsigned shader clamp folding, additional shader lowering, static nxformat 1.0.3, the new interface and game-list work, and opt-in local wireless LAN support. Existing title, pipeline-cache, AMD compute mesh fallback and query/FIFO lifetime safeguards are retained.

Community PRs 1373 and 1376 were inspected. Their hardware-idle/suspended-channel path does not replace the measured GpuSplit backoff path; the longer fallback for guest stores without explicit notifications needs separate latency measurements. The previously held 1161/1336 command-address and macro assumptions remain unproved by their latest heads. Shader reuse/async jobs and other-game features were screened for applicability rather than presented as current Ocarina performance wins.

## Validation and owner comparison

The candidate passed 1,224 focused Linux assertions across 44 cases. A separate emitted-code check passed 13 assertions and confirmed all three baseline frame instructions are absent from candidate blocks. A fresh-process fastmem check passed 71 assertions across both frame modes and fault/recompile policies, using actual protected-page SIGSEGV handling and resumed guest execution. The cache-protection regression fails on the original whole-range read grant and passes all 23 assertions with the correction. Synthetic ring handoffs and negative controls additionally ran under ThreadSanitizer. ARM host timing results are not Steam Machine FPS measurements.

All configured Linux CTests, complete application packaging and owner rendering/quiet-FPS checks remain release gates. No current-game frame-rate gain or stable 60 FPS result is claimed from the source or synthetic tests.

The existing owned-command FIFO v3 path can be verified before promotion. Use the master gate `STITCH_ORDERED_FIFO_MACRO=verify` and an absolute regular `STITCH_ORDERED_FIFO_MODE_FILE`, initially containing exactly `verify\n`. Require actual eligible `checked > 0`, zero mismatches and owner rendering/audio/controller QA. Then atomically select `baseline\n` or `candidate\n` for a same-process ABBA comparison. Confirm each mode transition and growing deferred coverage. End with baseline unless the candidate is independently selected as a measured winner. Do not combine this trial with macro-DMA coverage, writer-frontier or other diagnostics that decline asynchronous execution. The JIT frame remains a separate opt-in comparison.
