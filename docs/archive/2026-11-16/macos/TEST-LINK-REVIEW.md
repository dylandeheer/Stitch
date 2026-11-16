# Mac test-link guard review

Status: **no blocking finding in the three-file adaptation**. Reviewed the current working-tree diff against source `562dfa90e1e678966f168b5813850e43e27f9d0b`. No edits, builds, SSH or app launches were performed.

The change is confined to three existing test translation units. It makes their GLSL/GLASM references conditional on the same `HAS_OPENGL` feature convention used by production code. The parent identified the upstream basis as PR 1223, head `5fab1d9f109174db1d7f3a0bde99195cd551eb28`; this review checks the local adaptation, not an independent remote PR retrieval.

## Coverage preserved

- `fast_fusion_byte_store.cpp`: SPIR-V headers and all four image/global-coherence combinations remain unconditional. With OpenGL disabled, the backend list is exactly `{0}` and every retained backend-0 SPIR-V binding/storage/barrier assertion runs. With OpenGL enabled, `{0,1,2}` restores both original GLSL/GLASM branches and their assertions.
- `global_memory_coherence.cpp`: only the exclusively GLSL/GLASM test is wholly feature guarded. The mixed sparse-slot test keeps backend 0 unconditionally and restores backends 1/2 with OpenGL. Other SPIR-V emission tests, descriptor/coherence checks and joining cases are unchanged.
- `maxwell_numeric.cpp`: the backend list changes from an integer loop to the same conditional array. It keeps both native/lowered float64 cases, three formats and both signs for SPIR-V. OpenGL builds retain all three emitters. All standalone numeric/IR oracle tests and every SPIR-V emission remain present; the adaptation does not take upstream's test-deletion shortcut.

Each file already includes `<array>`. Every remaining `Backend::GLSL`/`Backend::GLASM` reference in these files is within a `HAS_OPENGL` region. Conditional removal leaves valid `if` statements: the loop cannot enumerate a removed branch when OpenGL is off.

## Feature propagation

`src/shader_recompiler/CMakeLists.txt:205` builds GLSL/GLASM only under `ENABLE_OPENGL`. `src/video_core/CMakeLists.txt:614` publishes `HAS_OPENGL` when that option is on, and `src/tests/CMakeLists.txt:234` links the main `tests` target to `video_core`. These three files belong to that target. Thus their new guards receive the feature macro in the OpenGL-enabled build; the macro is not accidentally confined to the shader library or another test executable. The separate `shader_translation_tests` target does not own these translation units.

`git diff --check` passed. Actual Mac link/unit-test results belong to the root's ongoing build. No OpenGL-enabled build was executed by this review, so cross-platform runtime validation must not be inferred from this source inspection.

## Reviewed bytes

- `src/tests/video_core/fast_fusion_byte_store.cpp`: `d65df98806b86a48d2b079a14838a74b4893e69f8db29e986ccab2f6cc9c997d`
- `src/tests/video_core/global_memory_coherence.cpp`: `68909e6674860b895ae85f7698d32403847a36c6d5796be7494b7ccf6aaae0b5`
- `src/tests/video_core/maxwell_numeric.cpp`: `a7f15c6a9df373faef1f2840dac10c2b88fdba858af57115427cf348fd7cdd0d`
