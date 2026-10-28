# Architecture

## What is being ported

The original FlatOut 2 executable remains the authority for simulation:

```text
FlatOut2.exe (x86 machine code)
        │ static lift, once per frozen EXE hash
        ▼
generated C functions + guest-memory dispatch
        │ compile with wasi-sdk
        ▼
WebAssembly module
        │ calls compatible host APIs
        ▼
browser host: Win32 subset · D3D9 over WebGPU · Web Audio · input · files
```

This preserves the original game loop, vehicle behavior, physics and AI. It is
very different from extracting meshes and rebuilding gameplay in JavaScript.

## Reusable versus FlatOut-specific work

An x86 lifter and a basic Win32/D3D9 compatibility runtime can be reusable
infrastructure. A finished port is still game-specific: the guest executable's
addresses, imports actually reached, timing, file behavior, D3D9 state patterns,
shader bytecode, data paths and edge-case workarounds must be discovered and
tested against FlatOut.

The NFSU2 Web project is therefore a strong technical reference and potential
starting codebase, not a drop-in engine or package. Its generated game code,
NFS-specific overrides and multiplayer layer are deliberately out of scope here.

## Browser responsibilities

| Original dependency | Browser equivalent |
| --- | --- |
| `KERNEL32`, `USER32`, registry, files, threads | Narrow compatibility layer over Wasm, workers and browser storage |
| `d3d9.dll` / `d3dx9_30.dll` | Guest COM objects which record D3D9 calls; TypeScript/WebGPU consumes them |
| WinMM/FM0D output | Audio ring buffer consumed by an AudioWorklet |
| `dinput8.dll` / window messages | Browser keyboard, mouse and later controller events |
| Game `data/` files | Local development server; later range-fetch plus OPFS cache |
| GameSpy/Winsock | Explicitly deferred until after local single-player works |

The project will first implement only API calls observed during boot. Unsupported
calls must fail loudly with an address and API name, rather than silently return
success and create misleading behavior.

## Data policy

The repository must never contain game assets or generated code derived from the
game executable. Development points at an existing user-owned install. A future
hosted version must use a legally appropriate bring-your-own-data flow; serving
FlatOut assets publicly is not part of this project.
