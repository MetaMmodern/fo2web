# Tooling

## Required for milestones 0–2

- **Python 3.11+** with an isolated virtual environment.
- **Capstone 5.x** and **pefile** for PE parsing and x86 lifting.
- **Node.js 20+** for the local browser host and WebGPU TypeScript.
- **wasi-sdk 34** (Clang/wasm-ld): target `wasm32-wasip1-threads`.
- **Chrome** with WebGPU enabled. Cross-origin isolation headers are required
  once SharedArrayBuffer/Wasm threads are enabled.
- **Git** to bring in the selected recompilation runtime source as a reviewed,
  pinned upstream dependency.

The current machine already has Node 24, Rust, CMake, Ninja and Apple Clang.
It does not currently expose a wasi-sdk compiler. Rust is useful for a future
local asset server but is not a prerequisite for the first Wasm smoke test.

## Useful only after first boot

- A Windows environment capable of running the frozen build for reference
  screenshots, input/timing comparison and D3D9 API tracing.
- RenderDoc or apitrace on Windows to capture the D3D9 calls of boot and a
  controlled quick race.
- A local HTTPS setup for testing cross-origin isolation from another machine.

## Do not install yet

Do not install Emscripten, a general emulator, Three.js, or a full game engine
for this path. The intended toolchain is static recompilation plus Wasm/WebGPU;
those tools would not remove the compatibility-runtime work.
