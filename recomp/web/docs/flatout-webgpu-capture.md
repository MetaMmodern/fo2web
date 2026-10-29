# Capture a FlatOut WebGPU frame

This debug mode loads WebGPU Inspector **before** the app starts. It does not depend on the Chrome extension attaching to the page or on the game submitting frames from `requestAnimationFrame`.

1. Build the web app (`cd recomp/web && npm run build`). Restart the web server only if it does not serve updated files from disk.
2. Open the usual game URL with `?gpuInspector=1` appended, for example `http://localhost:8080/?gpuInspector=1`, then press Play.
3. When the broken car is visible, open DevTools Console and run `fo2CaptureGpuFrame()`. Keep the game running until the promise resolves and the console says the frame was saved.
4. Find `fo2-frame-<timestamp>.wgpuc` in Downloads. It can be loaded through WebGPU Inspector's Capture panel menu. Share the file path for analysis. This build saves a binary capture with buffer and texture payloads.

To get a compact text summary locally, run `node tools/inspect-wgpuc.mjs <path-to-capture.wgpuc>`. It reports draw calls, pipeline and texture bindings, and the captured transformed vertex ranges. Use `--min-vertices N` to filter small draws.

The command captures the next game frame actually submitted to WebGPU. It can make that frame slow and produce a large file. Use a single capture at car selection first. The normal URL does not load Inspector.

The same-origin script `src/static/webgpu-inspector.js` is WebGPU Inspector 1.8.0 from [brendan-duncan/webgpu_inspector](https://github.com/brendan-duncan/webgpu_inspector), pinned to commit `afc6bbe6291e18904d145a988ec757a31d6ad935`, SHA-256 `62541690484f94757c81dcf08af6d10313e7499932621c68fdb7bed60429cd7b`. The adjacent license file is its MIT license. Its [manual capture API](https://github.com/brendan-duncan/webgpu_inspector/blob/main/docs/manual_injection.md) requires `initialize()` before any GPU objects are created, and the renderer brackets exactly one submitted frame with `beginFrameCapture({ maxBufferSize: 128 * 1024 * 1024 })` and `endFrameCapture()`.
