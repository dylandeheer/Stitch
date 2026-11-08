# Stitch6 source integration

Measured baseline is Stitch5 at 3a787138. This experimental branch integrates selected changes from the pinned Lichii release117/current nx2 audit, not an unreviewed whole-branch merge. Source selection and native checks precede the combined Linux/gameplay test; no FPS result is claimed yet.

The source audit pinned released build117 at89440b29a90708c0fefc126b2bd8c7a64a996bdd and newer nx2 head25a28bb4ecbaf8e6110507b701ce89b35ef4ac32, with67 merged commits after common d5494116902d15f9616efbe7668bbcec33a93f9c. The audit distinguishes already ported, applicable, hardware-specific and unproved changes. Rendering remains complete, with the existing AMD compute mesh fallback and owner profile.

## Adopted upstream mechanisms

- #699: keep a compatible active renderpass after a deferred clear. A Stitch repair preserves its former one-time host-query boundary without reloading attachments where safe. Active samples are paused, normal draw/clear setup chooses whether to resume, and closed banks, conditional rendering, transform feedback, pass stores or fragment barriers retain the full transition. This avoids changing nonprecise query counts or inventing zero slots. 768 production-method trace replays and six rejected mutants exercise this before integration.
- #488: retain required same-value stores while the backend method sink waits and invalidate front snapshots on split restart. Custom writer tracking is retained.
- #264: rearm only missing read-trap spans and forget trap metadata when texture reads have consumed the core traps. Texture and buffer locks retain their established order.
- #621: advance the pending-channel poll timer only on real progress, and stop sleeping after the last pending channel resumes. Linux keeps its existing short sleep mechanism.
- #878: release synchronous compiler graph pools after all replay, optional tensor-module emission and dumps, before the driver compile. Asynchronous ownership is unchanged.
- #880: retry allocation/collection for a normal backable image that cannot allocate, instead of rendering into a sparse unbacked substitute. Retired image-pool accounting/tickets are retained.
- #816: compute a conservative bound on native storage writes from actual shader stores/atomics and launch dimensions. Full-sized bindings and read synchronization remain. Unknown, loop-carried, globally routed, unsupported or group-dependent indirect offsets retain the full written binding. This adaptation removes the arbitrary fixed cap and reserves maximal device alignment residue instead of rereading a racing guest pointer. Checked arithmetic and invalid bit-field geometry decline to the original path. The independent Full-vs-Auto production fixture exercises a page-three GPU sentinel with a previously ready stale copy; deliberately underdeclared metadata demonstrates the failure it must detect.
- #609: stop fence-thread writes from racing other packed DMA flags.
- #614: query cooperative matrix properties only on a supporting physical device. The tested Navi33 tensor path remains enabled.
- #700: invalidate every touched32-byte block and respect intentional query walk early exits, while semi-flush visits every required query.
- #611: preserve software-engine texture invalidation when unrelated inner invalidations are skipped.

## Additional isolated research integrated for comparison

The bounded tensor-operand compiler experiment keeps exact per-lane SSA A/B values across eligible straight-line operations and constructs only exact positive-zero C matrices. No D-to-C forwarding or reassociation occurs. Original, paired, and reuse modules are warmed behind one fence, with original mandatory and replay parity required before optional variants. Requests choose/capture an immutable variant before queue execution. Optional rejection retains a working paired/original fallback. The ten-shape public Navi33 differential passed540,160 raw packed comparisons, but gameplay timing is pending. The target replay reduced369 matrix loads to224 with123 multiply-adds and252 barriers unchanged.

R32 source snapshots extend the already checked backing-file path only to the observed64 MiB native R32_FLOAT atlas shape. D16 evidence optionally classifies complete owned run snapshots, including fallback bytes, and changes no rendering. It retains at most4 MiB scratch and logs no texture content. Exact synthetic decoders matched67,108,864 R32 and301,989,888 D16 output bytes.

A ten-second opt-in macro/list probe attributes actual queue waits by executable/channel/generation, FIFO versus persistent RAM and parser reason. It changes no macro execution. Static shape classification rejects unproved operations and pending sink state, with real-interpreter negative controls. Full source and actual ring regression still need the combined Linux test.

## Deliberately separate

#718's broader query cap exposes a dense bank-order defect under recycled pool IDs; no cap increase is included. The shader scan cutoff correction is also held pending a focused GPU/bank oracle. Native preflush #861 is a competing readback design rather than an additive change to the measured private-copy winner. Initialization skipping #818 still needs a first-use/overwrite proof. #657's broad AMD tensor policy is not applied to this proven Navi33 path. Larger channel-parking #752, split-draw #858, archive #885 and new precache PRs remain separately audited reworks.

Validation must include the full Linux suite, package eager-binding/launch checks, then owner rendering/depth/query/audio/controller checks and controlled timing. Existing public numeric reference limitations remain documented. Do not infer frame-rate gains by adding CPU wait scopes, GPU categories or cold upload times from different captures.
