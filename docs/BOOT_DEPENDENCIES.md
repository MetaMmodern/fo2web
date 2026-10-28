# FlatOut 2 browser boot dependency audit

## What the executable imports

The lifted FlatOut 2 executable has 241 static Win32 imports. The import table is in `recomp/gen/all/impargs.c`.

| DLL family | Count | Role |
|---|---:|---|
| Kernel32 | 116 | Threads, memory, files, time, locale, and loader APIs |
| User32 + GDI32 | 41 | Window creation, messages, cursor, and basic drawing handles |
| Winsock | 36 | Multiplayer and network discovery |
| WinMM + MSACM32 | 30 | Audio output, capture, timing, and format conversion |
| Advapi32 | 7 | Registry and saved configuration |
| Ole32 | 5 | COM initialization and object activation |
| D3DX9 | 3 | Effect and texture helpers |
| D3D9 | 1 | Graphics device creation |
| IP Helper + Shell32 | 2 | Adapter discovery and shell launch |

The game also calls `GetProcAddress` for APIs that are not in that static table. In the latest smoke log it requests six extra Kernel32 APIs (now resolved), `DirectInput8Create` (now resolved), and these multimedia exports:

- `dsound.dll`: `DirectSoundCreate8`, `DirectSoundCreate`, `DirectSoundEnumerateA`, `DirectSoundCaptureCreate`, and `DirectSoundCaptureEnumerateA`.
- `ddraw.dll`: `DirectDrawCreateEx`.
- An earlier startup branch probes `DirectSoundCaptureCreate8` as well.

The runtime returns a placeholder handle for arbitrary DLLs. A placeholder does not provide that DLL's exports, so dynamic exports need explicit mappings. The observed DirectSound and DirectDraw exports are now mapped as a batch: playback creation uses the existing DirectSound COM shim, enumeration reports no capture devices, capture creation returns `DSERR_NODRIVER`, and DirectDraw reports unsupported.

## What the browser runtime already supplies

- Guest x86 execution and Win32 import dispatch in WASM.
- WebGPU-backed D3D9 COM objects and D3DX texture/effect shims.
- Browser filesystem mapping for the game `data/` tree and persistent `/save` data.
- Keyboard, mouse, and gamepad input, including a DirectInput8 object.
- Browser audio output plumbing, plus several DirectSound and wave APIs.

## What the latest boot trace says

- The runtime loaded FlatOut's executable and assets, initialized D3D9, created a 1280x1024 device, and rendered four frames. The gray screen is real renderer output; it is not a failure to start WebGPU.
- Dynamic `DirectInput8Create` resolves to the existing DirectInput COM implementation. The observed DirectSound and DirectDraw exports also resolve through explicit shims now.
- Calls reached without an implementation during this run: `ole32!CoInitializeEx`, `gdi32!GetStockObject`, and `user32!EnableWindow`. The game continues past them, but each needs a deliberate return value/behavior for a faithful window and COM setup.
- The current fatal trace is a JavaScript/WASM call-stack overflow in the game's recursive expression parser (`sub_00578d66` / `sub_00578cfe`), reached while loading a `Menu1` resource from the `data/global` group. At the last observed dispatch, parser token index 0 targets `0x00578cfe`; the parser cycles without returning. This remains the immediate blocker after resolving the dynamic API probes.

## How to read this inventory

The 241 static imports are the complete declared Win32 dependency surface, not a promise that all 241 APIs are needed to draw the first frame. The dynamic list comes from observed `GetProcAddress` requests. The boot path is being tracked separately from optional audio, capture, DirectDraw fallback, networking, and later game modes. A missing API is considered boot-blocking only after the trace shows that the game calls it and depends on its result.
