# Stitch

Stitch is an experimental open-source Nintendo Switch and Switch 2 emulator fork of [Lichii](https://github.com/zortos293/Lichii), focused on correct rendering and smoother frame delivery on AMD Linux and SteamOS. It inherits the work of Eglantine, Eden, yuzu and Sudachi.

The current development target is Ocarina of Time on a Navi33 Steam Machine. Rendering correctness, sustained frame rate and streaming hitches are tested separately. Measurements from one scene are not a guarantee for another game or GPU.

## Builds and existing data

The desktop application is **Stitch** and the GUI executable is `stitch`. The inherited CMake GUI target remains `yuzu`; the CLI target remains `yuzu-cmd` and its executable remains `eden-cli` for build compatibility.

Stitch retains Eden-compatible save, settings and cache formats. The SteamOS test launcher uses its own existing isolated profile. No game files, keys or firmware are distributed with this repository.

The [releases page](https://github.com/dylandeheer/Stitch/releases) belongs to this fork. Update links point here so upstream releases cannot replace a Stitch build. There are no published Stitch releases yet. Build and source notes for measured candidates are kept with the corresponding package; the active performance work is on `stitch/amd-performance`.

See [Build.md](docs/Build.md) for inherited build instructions. Current custom packages are validated on Linux x86-64; other platforms need their own validation.

## Performance work

See the [engineering notes and prioritized work](docs/Stitch-Performance.md) for the pinned base, source attribution and validation gates.

The AMD recovery branch uses a compute fallback for the native mesh path that caused GPU resets on the test machine. It retains per-primitive attributes, query-slot boundaries and graphics-state restoration. Earlier work includes mesh-pipeline cache warmup, batched texture-collector readbacks, and conservative full-alias upload elimination.

Performance changes require a reproducible measurement, regression checks and gameplay verification. Unsafe skipped draws, guessed texture contents and resolution reductions do not count as engine fixes.

## Credits and license

Original contributor and copyright notices are retained. This includes the Lichii, Eglantine, Eden, yuzu and Sudachi contributors, Eglantine's original developer Camille LaVey, and YOB (yobslab) for inherited Lichii artwork. Artwork is inherited for now; the app name does not imply a new artwork license.

See [LICENSE.txt](LICENSE.txt), source notices and [the upstream Lichii README](docs/Upstream-Lichii-README.md). Stitch is independent of its upstream projects and is not affiliated with Nintendo.
