# Milestones

## 0 — Binary preflight

- Freeze the `FlatOut2.exe` SHA-256.
- Record PE sections, relocations, imports, TLS callbacks and data layout.
- Generate a first lift and report unsupported x86 instructions.

**Exit:** the binary can be deterministically analyzed and all dependencies are
named.

## 1 — Wasm entry-point smoke test

- Build generated C plus minimal CPU/guest-memory runtime with `wasi-sdk`.
- Map the PE image into Wasm memory.
- Replace imports with trace stubs and execute from the real entry point.

**Exit:** browser console shows the original executable entering Win32 startup
and reports the first missing API call.

## 2 — Original boot / menu

- Implement the Win32, filesystem, clock, threading and input calls reached at
  startup.
- Implement the observed D3D9 and D3DX subset on WebGPU.
- Serve the existing uncompressed `data/` directory locally.

**Exit:** an original FlatOut boot or menu state is visible in a browser tab.

## 3 — One local race

- Support all rendering, audio and data paths used by one known quick-race path.
- Verify input, fixed/real timing and car handling against the Windows build.

**Exit:** a local quick race is playable with the original simulation.

## Deferred

Videos, controller/wheel support, saves, settings UI, packaging, public hosting,
and online multiplayer are not allowed to expand milestones 0–2.
