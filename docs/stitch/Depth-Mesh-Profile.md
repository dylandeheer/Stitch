# Stitch7: exact depth initialization, mesh lookup and private attribution

This branch starts at Stitch6 f3fe39e5. The owner confirmed correct objects, lights, sound and controls. Its isolated stationary tensor-reuse ABBA was only +0.60% (21.9563 to 22.0884 FPS), so the existing paired path remains selected. Both arms reported full GPU utilization at roughly 2.2 GHz. These measurements are not a controlled cross-build comparison and do not establish stable60 FPS.

## Whole-depth initialization

One observed 12288-square native D16 atlas had complete zero coverage over all288 MiB, including the mapped fallback. Its expensive staging and detile can be omitted only when exact metadata and every owned source run prove that a fresh, ordinary actual D16 image can receive one transfer clear. The candidate is enabled only with STITCH_D16_ZERO_BASELINE_FILE naming an absent marker. An existing marker or filesystem error selects the original upload.

The implementation validates the complete source partition and destination rectangles before the first read. It captures each run once in at most4 MiB scratch. On the first nonzero byte, it reconstructs only the previously proven zero prefix into ordinary staging and decodes that run and all subsequent snapshots normally. It does not reread earlier guest memory or retain a whole-atlas snapshot. The all-zero path sets the normal initialized state, leaves any render pass and orders UNDEFINED to transfer-destination, clear0.0, then GENERAL with transfer-write visibility. Mixed inputs, unknown layout/format, prior use, registration, aliases, rescale, sparse or recycled storage retain ordinary behavior.

Native coverage/decoder tests and four intentionally wrong policies passed their required green/red controls. The actual clear command and readback passed on both M1 Max/MoltenVK and the owner's Navi33/RADV: all17262 bytes matched; a deliberate wrong depth made all8631 texels differ. This proves the primitive, not title transition speed. Owner rendering, observed admission and transition timing remain required.

## Bounded task ownership

STITCH_BOUNDED_MESH_TASK_LOOKUP=1 emits a bounded uniform helper only for task-fed compute mesh shaders. The unchanged prefix producer establishes offset0=0. The helper retains the rightmost accepted owner and its exact starting group, stops when the remaining search interval is one, and avoids the final offset reload. A separate function preserves guest entry-block phi predecessors. Taskless/native programs, serial distribution, descriptors, arena layout, barriers, draw order, counters and query/XFB behavior are unchanged.

Public production-generated distribution and mesh modules passed40 cases on Navi33 and MoltenVK,4328 groups per mode and1239080 complete arena words with independent expected values and untouched sentinels. CPU tests cover131112 count arrays and1096844 group cases. Wrong ties, lost offsets and wrong bases fail independently. Actual GPU stage/FPS gains and title rendering have not yet been measured.

## Private instruction capture

The new trace isolates a single extended macro with336688 FIFO reads in ten seconds, all under tracked shadow mode. It cannot be moved by the prior clean/passthrough admission rule. STITCH_PRIVATE_MME_IMAGE explicitly names one new absolute file inside an owner-private directory. It captures only a matching channel/address-space/method/start and full instruction-RAM hash, recording the current generation. The fixed64-byte manifest and128 KiB RAM are published once with Linux0600 permissions, exclusive temporary creation and complete nonclobber publication. No parameter, guest buffer or instruction content is logged. Unknown state and unset flags leave macro execution unchanged. Real captures and decoded instructions must remain private.

## Additional PR selection

PR914's isolated neutral-reactive texture correction allocates the extent fetched by the generic FSR3.1 SDK instead of a1x1 image. It is a correctness fix for that separate backend, not a measured optimization of the selected Ocarina presentation path. The rest of PR914 and PR915 depend on an unmerged scene-replacement pipeline with unresolved jitter/failure-frame ownership, and PR915 also requires an external FSR4 implementation. They are not imported wholesale. PR917's wider renderer/JIT/HLE changes are under separate review; its original lookup replacement's32 MiB bound disagrees with the actual4 MiB guest loop. No unchecked HLE or replacement window is included here.

Run the combined Linux tests and package checks, then collect an actual matching owner session before drawing performance conclusions. Keep source defaults and a working original path for unproved candidates.
