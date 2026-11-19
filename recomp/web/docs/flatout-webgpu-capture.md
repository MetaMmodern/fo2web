# Capture a FlatOut WebGPU frame

This debug mode loads WebGPU Inspector **before** the app starts. It does not depend on the Chrome extension attaching to the page or on the game submitting frames from `requestAnimationFrame`.

1. Build the web app (`cd recomp/web && npm run build`). Restart the web server only if it does not serve updated files from disk.
2. Open the usual game URL with `?gpuInspector=1` appended, for example `http://localhost:8080/?gpuInspector=1`, then press Play.
3. When the broken car is visible, open DevTools Console and run `fo2CaptureGpuFrame()`. Keep the game running until the promise resolves and the console says the frame was saved.
4. Find `fo2-frame-<timestamp>.wgpuc` in Downloads. It can be loaded through WebGPU Inspector's Capture panel menu. Share the file path for analysis. This build saves a binary capture with buffer and texture payloads plus a top-level `d3dDraws` array containing CPU D3D state, source vertex bytes, generated `TVert` bytes, and CPU → `GwDraw` → WebGPU command correlation.

To get a compact text summary locally, run `node tools/inspect-wgpuc.mjs <path-to-capture.wgpuc>`. It reports draw calls, pipeline and texture bindings, and the captured transformed vertex ranges. Use `--min-vertices N` to filter small draws.

The command captures the next game frame actually submitted to WebGPU. Once that frame is submitted, rendering pauses while the Inspector copies and maps each buffer and texture in 4 MiB-or-smaller staging chunks, keeping the complete payload in JavaScript memory until it writes one `.wgpuc` file. This preserves the full trace without allocating all GPU readbacks at once. A large trace can take time to finish; keep the tab open until the console says it was saved. The normal URL does not load Inspector.

The same-origin script `src/static/webgpu-inspector.js` is WebGPU Inspector 1.8.0 from [brendan-duncan/webgpu_inspector](https://github.com/brendan-duncan/webgpu_inspector), pinned to commit `afc6bbe6291e18904d145a988ec757a31d6ad935` and locally patched for deferred bounded readback/export. The adjacent license file is its MIT license. Its [manual capture API](https://github.com/brendan-duncan/webgpu_inspector/blob/main/docs/manual_injection.md) requires `initialize()` before any GPU objects are created, and the renderer brackets exactly one submitted frame with `beginFrameCapture({ deferredPayloadReadback: true })` and `endFrameCapture()`.

CPU provenance includes every fixed-function CPU `TVert` draw. To bound trace growth, ordinary draws include their first six source/generated vertices. Draws using texture 14 with PS 14 or texture 82/83 with PS 28/29 include the complete source/generated vertex ranges. Texture 82 is included because runtime captures identify the originally expected TV texture 83 as 82.
