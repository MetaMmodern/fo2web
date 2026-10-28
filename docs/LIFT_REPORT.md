# FlatOut 2 static-lift feasibility report

Date: 2026-10-25

## Target

| Field | Result |
| --- | --- |
| Executable | `FlatOut2.exe` |
| SHA-256 | `612c6c45d6e675b11751fedb3a11d7674fa381cda1921ec852073464c3d5ba20` |
| Image base | `0x00400000` |
| Entry point | `0x00602638` |
| PE relocation entries | 53,228 `HIGHLOW` slots |
| Generated functions | 11,584 |
| Generated C | 73.0 MB, 128 shards |
| Missing referenced generated functions | 0 |
| Unsupported-instruction trap sites | 213 |
| Wasm compilation smoke test | passed for a generated shard |

## Interpretation

FlatOut has a conventional, intact PE32 relocation directory. This is better
than the NFSU2 source executable, where the same information had to be manually
recovered from a hidden section. The generic x86-to-C lifting pipeline works on
FlatOut after replacing only that NFS-specific relocation recovery step.

The 213 traps are not yet a boot blocker: static discovery can enter data or
cold/error paths. The port must log a guest program counter on every trap, then
implement only instructions reached while booting. Two generated functions have
generic `lift-error` stubs (`0x00406f94`, `0x00532b2c`); neither is currently
known to be on the boot path.

## Runtime surface discovered

The generated import table has 241 imports. The early browser runtime needs a
small but real subset of:

- Win32 file, heap, virtual-memory, timing, TLS, synchronization, window and
  registry APIs.
- D3D9's `Direct3DCreate9` plus `D3DXCreateEffect` and texture/surface helpers.
- WinMM timing/audio APIs; FlatOut's FMOD implementation is present in the EXE.
- DirectInput and other DLLs loaded dynamically, to be confirmed by boot traces.

Networking, GameSpy and nonessential audio paths remain deferred. A browser
implementation must never pretend those imports succeeded unless their behavior
is sufficient for the code path that called them.

## Next engineering slice

Adapt only the generic portions of the NFS runtime into a FlatOut host:

1. Guest PE image mapping, CPU dispatch, imports and trace stubs.
2. Filesystem, heap, clock, threads and minimal window/message behavior.
3. A D3D9 call tracer, then the boot/menu-rendering subset over WebGPU.

NFS game-specific hooks, overrides, multiplayer, UI additions and fixed memory
addresses are excluded rather than carried into FlatOut.
