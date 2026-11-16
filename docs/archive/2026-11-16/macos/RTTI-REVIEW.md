# Apple RTTI / exception-matching gate review

Status: **no blocking finding in the two CMake changes**. Source inspection and generated-command verification only; no builds, probe execution or production edits by this reviewer.

## Reproduced evidence supplied by root

The native Apple build finished linking, but six existing tests reported standard exceptions as unknown rather than matching their typed handlers. The independent `toolchain-probe.cpp` calls `std::stoi("invalid")` and catches `std::invalid_argument`. Root reported exit 0 with normal RTTI, and uncaught termination/exit 134 with `-fno-rtti`; the flag-dependent result was reproduced with both Apple Clang 21 and Homebrew Clang 21. This is therefore a production exception-matching gate for the current compiler/SDK/runtime combination, not a reason to suppress or remove the existing test expectations. It does not establish a universal claim that every Apple compiler/libc++ release cannot combine exceptions with no RTTI.

## Patch scope

- `src/CMakeLists.txt` selects `-frtti` only under `APPLE` in the existing non-MSVC branch. Its generator expression restricts the RTTI flag to C++, so C/Objective-C-only translation units are not accidentally given C++ RTTI options. Other platforms retain the original `-fno-rtti`; the MSVC policy is untouched.
- `src/dynarmic/CMakeLists.txt` removes Dynarmic's own explicit `-fno-rtti` only on Apple. It retains every other warning/optimization/exception flag and does not alter the Linux/Windows list. It does not remove tests or catch clauses.
- Exact inheritance detail: `add_subdirectory(dynarmic)` occurs near the beginning of `src/CMakeLists.txt`, before the parent adds its RTTI compile option. Current Dynarmic commands therefore use the compiler's **default RTTI-on mode**, not an explicitly inherited `-frtti`. That is functional with the current Apple Clang toolchain. The generated-command inspection below proves that there is no later Dynarmic no-RTTI override. An explicit Apple `-frtti` in Dynarmic could make this intent more obvious, but it is not required to repair this build and is not a blocker.

## Generated command audit

Read current `work/build-stitch-mac/compile_commands.json`: 1957 commands, 1478 with explicit `-frtti`, **zero project or Dynarmic commands with `-fno-rtti`**. Dynarmic entries have neither flag and use the compiler default. There are 7 remaining no-RTTI commands, all the pinned Cubeb C++ implementation. Cubeb independently sets both `-fno-exceptions -fno-rtti` in its own CMake directory; the inspected seven sources contain no typed C++ catch, throw, `std::sto*`, `dynamic_cast` or `typeid` paths. That C API/audio implementation is not a hidden override on app/core/test compilation, and no vendor policy change is justified by this gate.

The compiler/stdlib switches and one-SDK rebuild should remain consistent through packaging. The root's full native rebuild and all nine CTest results remain the acceptance gate; this report does not claim those still-running checks have passed. The minimal probe remains a useful toolchain diagnostic, but does not replace those suites or a real game boot with JIT/fastmem.

## Reviewed source bytes

- `src/CMakeLists.txt`: `eae3ddbcd6f608f297e8af53c04b28b80f1e71f0791809364fad8b85935dd2f3`
- `src/dynarmic/CMakeLists.txt`: `040126cc0be2e52bb9c9b7fc97c2cf8ad7b91b46b5045d37c1f124aa068c65b4`
