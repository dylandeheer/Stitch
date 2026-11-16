# Local development handoff before cleanup (2026-11-16)

This archive preserves source and conclusions before removal of local Stitch apps, profiles, workspaces, build tools and temporary files. It is a recovery snapshot, not a release, merge, or promotion of unfinished experiments.

## Engine branches

- Apple Silicon source: `stitch/macos-20261111`, `c234ec16efa8dd63332b76df410c3e19d9b1264b`.
- Latest Linux/Steam Machine CPU candidate: `stitch/cpu-bottlenecks-20261111`, `da4c8bd084e656ca67b91355ef1d6f53b1afd677`.
- Stitch 9 control: `stitch/upstream-150-20261111`, `562dfa90e1e678966f168b5813850e43e27f9d0b`.

These are separate development lines. The archive branch descends from the Mac branch and does not merge the CPU candidate. Existing renderer/performance branches are retained remotely.

## Mac result and reproduction notes

The native arm64 Release app built with Apple Clang 21, one Xcode SDK (27.2), deployment target 26.0, and Qt 6.11.1. All nine CTest suites passed. Apple-only RTTI repaired standard-exception catch matching for this toolchain; Vulkan-only test guards retained numeric SPIR-V coverage. The packaged executable and bundled libraries passed deep strict ad-hoc signature verification, with allow-jit on the actual payload.

The original game was read in place. A separate Mac save/account profile was copied from Steam, and the supplied IPS patch application was logged. Settings were docked, 1x, FXAA, extended dynamic state disabled, asynchronous/cached shaders enabled, compute mesh fallback, ARM64 Dynarmic. No Linux shader cache was imported.

Zelda's title/save menu and copied Kokiri Forest save rendered. A stationary 30-second gameplay sample averaged **4.199421 FPS**, median **237.97 ms**, p99 **248.40 ms**. This did not achieve playable/stable 60 FPS. The earlier 53.2 FPS loading/menu sample is excluded. Cold CPU JIT/fastmem compilation gaps, intermittent shader builds and later settled-world readback/queue waits are distinct observations; see `macos/MAC-LOAD-REVIEW.md`. Owner audio, keyboard and physical-gamepad QA was deferred.

The first startup command-line game open blocked before the Qt event loop. Selecting the original file through File > Load File booted it. No privacy database was changed. Direct MoltenVK's optional portability-enumeration diagnostic was nonfatal. There is no Mac Hypervisor backend in this pinned source.

`macos/launcher.c`, entitlements and `package.py` preserve the original deployment recipe. Build/package from a separate checkout pinned to `c234ec16efa8dd63332b76df410c3e19d9b1264b`, not the archive branch HEAD: the original package recipe deliberately checks HEAD against that source freeze. It is workspace-oriented: put the repository in `$WORKSPACE/work/lichii`, the native build in `$WORKSPACE/work/build-stitch-mac`, and the Qt SDK/compiled launcher/source freeze beside `package.py` in `$WORKSPACE/work/mac-20261111`. Build options retain `$WORKSPACE` placeholders. Compile the wrapper using `/usr/bin/clang -arch arm64 -mmacosx-version-min=26.0 -O2 -Wall -Wextra launcher.c -o stitch-launcher`. Package outside a cloud-synced Documents directory if file-provider FinderInfo invalidates signatures. Text resources belong in Resources; sign the actual JIT payload and all nested code before the enclosing app.

## Latest Linux deployment recipes

`linux-stitch10/` retains the small text build, package, launcher, staging, smoke/regression and profiling helper sources from the latest Stitch 10 handoff. The engine itself is preserved at the CPU branch SHA above. These are historical workspace-specific templates: replace `$WORKSPACE`, `$HOME` and `$STEAM_HOST` placeholders before use, and review remote/profile paths for a fresh setup. They are archival material, not automatically runnable from this archive directory. Binary distributions and redundant full-source tarballs/patches are excluded.

## Recovery material

`source-recovery/numbered-backups/` preserves all 60 pre-existing numbered C++/header/CMake copies, with SHA-256 manifest. They are historical snapshots, not build inputs. The five `.patch` files preserve dirty PR experiment workspaces, with original base commits in `experiments.json`. They are not established fixes and must be reviewed/tested before reuse.

No game, IPS bytes, save/NAND data, keys, credentials, private screenshots, guest shader caches, raw process profiles or binary builds are committed here. The user requested deletion of local Stitch copies after source preservation. The original game/download files and the Steam Machine installation are outside this local cleanup scope.
