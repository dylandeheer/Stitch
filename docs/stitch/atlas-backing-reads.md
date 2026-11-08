# Linux D16 atlas reads in Stitch5

Stitch4 measured 129.32 ms and 73,728 minor faults while reading one 288 MiB D16 atlas. Stitch5 can use Linux `pread` to snapshot each eligible run directly from the emulator’s shared physical-memory file. The kernel supplies written page contents and zeros for file holes. Stitch retains the chunk boundaries, detiling and upload operations introduced by Eglantine #233. The file-read experiment is a Stitch change. [Eglantine #233](https://github.com/Eglantine-Community/Eglantine/pull/233), [Linux pread manual](https://man7.org/linux/man-pages/man2/pread.2.html), [Linux shmem implementation](https://github.com/torvalds/linux/blob/master/mm/shmem.c)

Set `STITCH_ATLAS_PREAD_BASELINE_FILE` to an absolute marker path before launch. A present marker selects the existing pinned memcpy. An absent marker selects `pread`. This experiment is off when the variable is unset. Both arms use the same GMMU and device-mapping checks, enforce a 4 MiB run limit and fill owned scratch before Vulkan work begins. Only the original shared file backing qualifies. Interrupted reads retry. A short or failed read triggers a full memcpy, replacing any partially written prefix. Unsupported mappings take the original page walk.

## SteamOS synthetic results

Mean source-read time for 288 MiB over 102 runs, two samples per arm in ABBA order on kernel 7.2.7. Every returned byte matched the independent reference.

| Fresh memfd backing | memcpy | pread |
|---|---:|---:|
| Fully written through another mapping | 30.24 ms | 29.72 ms |
| 25% written, 75% holes | 127.05 ms | 17.72 ms |
| Entirely holes | 142.91 ms | 8.65 ms |

The game’s atlas contents and in-game gain remain unmeasured. Compare the first equivalent upload on fresh backing for each arm. A prior memcpy can materialize file holes. Record `file_read_bytes`, read time, full upload time and frame-time tails. The Linux production fixture passed 76 assertions in five cases, including aliases, remaps, a paused page-table writer, anonymous fallback, interrupted reads and recovery from a poisoned partial read.

## Compatibility with early buffer reads

Reviewed against `fd5aabdf87b5b39ff415156a22105429dd3714ed`. The rebased PREAD patch changes one declaration hunk’s surrounding context in `memory_manager.h`. The atlas path holds the GMMU run guard, then the device mapping guard, through its source copy. Early-buffer publication uses the same device mapping guard to protect its guest writes. Its report exclusions release that guard before taking the GMMU guard to finish publication. The code paths retain their independent eligibility, lock order and fallback behavior. [Lichii #805](https://github.com/zortos293/Lichii/pull/805)
