# FlatOut 2 Web Recompilation

Run one exact, user-owned build of FlatOut 2 in a modern browser. The game logic
will remain the original x86 program, statically translated ahead of time to C
and then WebAssembly. The browser host will supply compatible Win32, Direct3D 9,
audio, filesystem and input interfaces, with Direct3D implemented over WebGPU.

This is **not** a Three.js recreation and it does not contain FlatOut code,
executables, music, textures, or other game assets.

## Current target binary

| Field | Value |
| --- | --- |
| Path (local development only) | `/Volumes/BOOTCAMP/Program Files (x86)/FlatOut 2/FlatOut2.exe` |
| SHA-256 | `612c6c45d6e675b11751fedb3a11d7674fa381cda1921ec852073464c3d5ba20` |
| Format | PE32 / x86 / Windows GUI |
| Image base | `0x00400000` |
| Entry point RVA | `0x00202638` |
| Base relocations | Present (`.reloc`) |

Only this binary is in scope until the first browser boot. A different EXE is a
different port target: generated code, hooks and debugging addresses must not be
mixed between builds.

## First proof of concept

The success criterion is: Chrome opens a local page, loads a Wasm module made
from this executable, and the original program reaches its first visible boot
state. Network play, saves, controllers, videos, and a complete race are not
part of that milestone.

See [architecture](docs/ARCHITECTURE.md), [milestones](docs/MILESTONES.md), and
[tooling](docs/TOOLING.md). The initial [lift report](docs/LIFT_REPORT.md)
records the successful static-recompilation feasibility gate.

## Preflight

No game files are copied. This command only reads the installation and writes a
report under `build/diagnostics/`:

```sh
FO2_GAME_DIR='/Volumes/BOOTCAMP/Program Files (x86)/FlatOut 2' ./scripts/preflight.sh
```
