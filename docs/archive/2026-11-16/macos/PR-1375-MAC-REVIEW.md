# MoltenVK cached-buffer tail review

Reviewed the actual seven-file diff of [Lichii PR 1375](https://github.com/zortos293/Lichii/pull/1375), pinned head `e5cd012149cc27bfb24c062cb090444e486ab3d5`, base `5b09546918418589e26a304accabff21cc89db3b`, through the GitHub connector. Compared the changed code with current Stitch buffer-cache, early-read, descriptor and staging implementations. No branches fetched, source applied, build run or application launched.

## When it is relevant

The issue is concrete: SPIR-V constant-buffer declarations are always 65,536 bytes (`spirv_emit_context.cpp:279`), but a cached Vulkan buffer currently allocates only `SizeBytes()`. A binding starting near its end can therefore have fewer than 65,536 backing bytes remaining even when the shader's actual accesses fit its smaller logical bound. The PR supplies a zero-filled 64 KiB physical tail specifically for MoltenVK's Metal buffer-length validation. Its enlarged null buffer also backs an unmapped uniform binding with that declared length.

This is a compatibility fix, not an established FPS optimization. It is warranted if the Mac baseline reports a Metal argument-buffer/buffer-length validation error at a constant-buffer binding, rejects an otherwise valid cbuf near a cached buffer's end, or exhibits the matching null uniform binding failure. Nothing retrieved proves that OoT on this M1 Max reaches the failing case. The author's Mac game test is Pokopia on M3 Pro with the entire older #656 stack, before this rebase; Linux/Windows CI does not exercise this MoltenVK allocation path. Keep the pending unmodified Mac baseline first.

The stream allocator already allocates its own 64 KiB tail (`vk_staging_buffer_pool.cpp:48,98`), so PR 1375 addresses a remaining cached/null-buffer gap rather than introducing the first tail anywhere in the renderer.

## Ownership and integration observations

- The PR keeps `BufferBase::SizeBytes`, guest address ranges, page registration, usage tracking and copy lengths logical. Its tail is only extra physical allocation and cache-budget accounting. That separation is compatible with Stitch's owned-readback boundaries; do not broaden guest ranges, readback spans or writer coverage to include the padding.
- `ChangeRegister` charges and subtracts the same aligned logical-plus-tail amount. OpenGL and non-MoltenVK runtimes return zero. Stitch's only buffer-port fake runtime currently has no positive-tail mode: adding a zero-return method makes it compile but does not validate tail accounting. A later port should add positive-tail register/unregister coverage while retaining the existing mapping/loan/invalidator tests.
- Keep the current owner/version, actual VMA visibility and per-manager download-landing logic. None of those needs to be replaced by the tail patch. MoltenVK cached buffers are host-visible in this source; `Buffer::Mapped()` will expose an allocation span including the new tail, while all valid guest copy ranges must still be bounded by logical size.
- The new tail fill is recorded outside a render pass before use. Its explicit barrier names uniform reads; current `BufferCache::CreateBuffer` subsequently clears the logical region through a broader transfer-write to memory-read/write barrier. Any adaptation that bypasses or reorders that construction path must preserve the tail visibility dependency rather than assuming allocation implies initialized zeros.
- Constructor offsets are cache-page aligned in current Stitch, satisfying the fill's four-byte alignment. Sparse-compatible source allocations use the same helper and their VMA allocation covers the extra size; this is not a license to expand a sparse virtual range's guest ownership.

## Hunks that must stay together / remaining caveats

1. Use the pinned **e5cd012** head, not the earlier fcf2c34 snapshot. The latest head rejects accelerated image downloads to the shared null slot (`Buffer::IsNull`) before they can overwrite its zeros, and adds `VK_ACCESS_TRANSFER_READ_BIT` to null initialization visibility for image uploads. Both are real dependencies of the enlarged null-buffer contract.
2. The advertised “other drivers unchanged” scope is narrower than the actual diff: null-buffer usage flags, initialization barrier, zero-size descriptor normalization and the null-download rejection change all Vulkan drivers. Physical 64 KiB cached tails and cache accounting are MoltenVK-only. Preserve/qualify those shared changes separately when porting.
3. A remaining **unproved integration risk**, not a reproduced OoT regression: `BindBuffer` normalizes every zero-size null binding to a real descriptor range, including writable storage-buffer callers. The shared null buffer must remain zero across unmapped writes. The new DMA guard covers image downloads, but does not by itself prove that shader storage writes, texel writes and other consumers cannot modify that shared backing. A future port should exercise unmapped-write followed by null-read behavior, preserving discard/zero semantics. Do not assume that increasing its capacity alone establishes immutability.
4. The old null texel-view path still sets a zero `VkBufferView` range; the `BindBuffer` normalization does not cover that separate API. This was already present in the baseline and is not proved introduced by the PR. A matching texel-view validation failure would need its own diagnosis.

## Optional PR 1367 note (body/metadata only)

[PR 1367](https://github.com/zortos293/Lichii/pull/1367), pinned head `e23cc77cd10a8d3c5a2d6f4c0eccf9bae5222a78`, is an opt-in Hypervisor.framework CPU backend port from Eglantine #405. It is not a configuration-only way to enable native guest execution in our current build. Its body says Dynarmic stays default and reports slower GPU-bound scenes on the tested M3 Pro despite lighter guest CPU work. The reported Linux/Windows checks do not compile this Apple-only backend. It also changes guest mappings/read traps and adds a Hypervisor entitlement. Defer this larger backend integration until the native Dynarmic baseline is measured. No independent review of its implementation or GPU/CPU ownership correctness is claimed here.

Metadata and the public PR diff are recorded locally. No game bytes, private shader data or instruction captures are included.
