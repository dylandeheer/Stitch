# Stitch 9: Lichii build 150 and later mainline updates

This merge pins Lichii `23a6a56c6a357f44c69da1890cd144295fca2087`, newer than released build 150 (`17baed716da04fd743fbd8f9604bec96eab0a123`). It integrates the 164 upstream commits since common ancestor `d5494116`, including 97 after the previous survey. Stitch 8 `464f286e` is the local parent. Its package was checked but not installed. The last installed quiet Stitch 7 sample was 24.6549 FPS; this is no measurement of Stitch 9. Stable 60 FPS remains an unverified objective.

## Mainline changes

Shader cache v2 validates records and coordinates writers, retains pipelines by a RAM budget, and can rebuild evicted graphics pipelines from kept SPIR-V. Cached-graphics and archive compute warmup are separate settings. Pipeline compilation, scalar shader emission, dispatch/barrier recording, ordered upload batching and block-linear swizzling incorporate the new upstream work. Device backing destruction/reuse, read-only native storage bounds, allocation failure handling, Linux audio queue sizing and ADPCM decoding are updated.

The x86 backend uses ordinary ordered loads where the host memory model permits, retains locked 128-bit operations and full barriers, and separates exclusive reservations by cache line. Host timing sleeps between events. The split GPU front can suspend a pending command copy and let another channel progress, while retaining the actual completion tick and complete writer dependencies.

The owning Ocarina temporal FSR 3.1 stack is now present. Its scene replacement remains off unless selected. It is distinct from the existing presentation filter. The optional FSR 4 runtime has separate driver/platform requirements and is not enabled on this RADV machine. The former scene-pipeline dependency exclusions for PRs 914/915 therefore no longer describe the merged mainline implementation.

## Preserved correctness boundaries

Stitch's paired tensor replay retains the mandatory original compute program and independently compiled optional modules. Optional variants never reuse a different program's driver binary key. Mesh replay and kept-SPIR-V rebuilds retain the bounded alternative's ownership and arena metadata. The tested compute mesh fallback remains mandatory on the target; new native-driver mitigations do not establish safety on its Mesa build.

Unrestricted generic back-thread macros remain disabled. Certified FIFO verification, dirty-buffer then query publication, owned-command lifetimes, borrowed-entry fallback and both refetch boundaries are preserved. Semaphore acquires use actual values after full writer ordering. DAddr predictions and an initially satisfied value cannot prove that queued GPU work will not change it. Ocarina does not consume synthetic mirrored semaphore values. Compatibility shadows for other titles retire with their registrations and device mappings; their remaining same-value CPU-write limitation is not an Ocarina fast path.

GPU query callbacks retain their slots until completion. The low-accuracy constant-payload shortcut requires no earlier asynchronous callbacks and a captured Inline/Deferred fence disposition; a live settings change cannot turn an already-freed slot into a deferred callback. The query accumulation cap remains one bank until numerical packing agrees with chronological bank reuse.

The selected versioned early-copy path and per-manager CPU-write landing barrier remain. New upstream asynchronous/predictive readback is dormant for every production renderer pending noncoherent allocation and mapping-lifetime proof. A buffer-cache mock specialization exists only for its tests.

Owned D16/R32 initialization still proves every source byte, exact subresource coverage and original Vulkan format. Mixed, negative-zero or NaN bytes take ordinary decoding. Layer-copy upload elision requires all destination mips/layers and valid, current, backed, nonalias source subresources. The retained-image pool counts actual allocations and waits for both GPU completion and expired views. No stale-page hint, partial newer overlap or unexecuted clear may discard original image contents.

## Community additions and adaptations

- PR 1265 (`ac01af80`): avoid unused perf-map naming and its global lock when launch-time profiling is absent.
- PR 1266 (`0e806dbf`), corrected: default safe flags already disable the read-only IR verifier. Explicitly clear that bit in Debugging and Paranoid. No normal-mode speedup is claimed.
- PR 1298 (`494fcad9`): bounded whole-block sysmix refill; additionally preserve free-guest-space notifications when the host queue is full. Null-sink pacing remains one block per tick.
- PR 1350 (`48d3f61f`): scroll Linux configuration pages while routing eligible wheel input away from closed selectors.
- PR 1328 (`55507743`): claim every collected CPU write under the cache lock before any download can overwrite another collected write.
- PR 1311 (`5d9a8f5a`): diagnostic minimum-age overrides cannot disable emergency collection. Actual allocation accounting from PR 1194 is included without dropping authored mip tails.
- PR 1099 (`c0b8b7b5`): adapted constant-payload fence lifetime, with the stronger alias/callback and captured-disposition guards above.
- PR 1081 (`67e2cc5a`): verify fast-draw and texture-memo decisions only after the relevant locks and full resolve. Verification is a correctness run with extra CPU work.
- PR 933 (`836062c4`): reduce repeated timeline polling on the single front executor; only a completed requested tick can return success.

PR 1225 (`33368817`) is retained as an experimental idle-core JIT warmup implementation, off by default and requiring `STITCH_JIT_WARMUP=1`. It records locations, not compiled machine code. Precompile preserves checked compare links and queued invalidations, never executes guest code, stops speculative decoding after 256 instruction fetches, and preserves cache space. A generation captured before an instruction-page fill prevents a racing cross-core invalidation from being lost. Its ordinary code-read callback can still cause an incidental GPU download when a recorded address is no longer ordinary text; a restricted speculative-read contract remains required before selecting this experiment for the owner. The launch package leaves it off.

NUP sibling stand-ins from mainline PR 1197 remain an explicit `EDEN_NX2_NUP_STAND_INS=1` loader option. Default RomFS and extraction retain the stored member bytes. This recovery substitutes authored content and is not a repair of those bytes.

## Exclusions and validation

Compound PRs 1161 and 1336 remain excluded: their current DAddr/front-view and concrete macro-probe assumptions do not establish physical producer or command-word identity. Guardhouse/early-deletion PRs 1314, 1285, 976 and 1241 do not prove replacement of every old pixel. Chunked ASTC, shader-library linking, shared shader functions, PGO and guest zstd replacement have separate dependency, workload or fidelity requirements. Unrelated platform, networking and other-game PRs were inventoried without being presented as line-by-line Ocarina audits.

Combined Linux build and all configured CTests, packaged loader/launcher checks, profile-copy isolation, and actual owner rendering/audio/controller QA are release gates. Public primitive readback proves transfer commands, not game uniformity or a frame-rate gain. Keep source/target hashes and distinguish verification-mode timings from a quiet performance sample.
