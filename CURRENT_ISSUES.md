# Current issues

- Parked rendering-parity follow-up: the post combination pass derives its half-texel UV offset from logical 1280×1024 dimensions rather than the physical 3072×1920 texture, leaving a 0.7 × 0.4375 source-texel sampling discrepancy. See docs/PIX_RENDERING_ANALYSIS.md.
- Parked GPU validation follow-up: the final Fields capture records 12 submissions referencing destroyed `blit rect 3072x1920` temporary textures. Determine whether these originate in game-frame submission or GPU Inspector capture before changing resource lifetimes. See docs/PIX_RENDERING_ANALYSIS.md.
- Audit production server load and security; address any risks found.
- Multiplayer search, joining, hosting, and racing have been verified for browser guests, remote players joining a browser host, and same-LAN players joining a browser host. However, players occasionally see “no players left” just before a race starts; investigate this intermittent failure.
- Bloom extraction now works, but comparison with the original still shows weaker highlight whitening and glow spreading, especially on the sunlit road and grass. Investigate the remaining bloom, colour, and sunbeam differences through the game's effect/API path rather than adding a replacement shader.
- Compare any remaining ground-detail harshness against the original at matched resolution and camera position. Full mip chains and mip uploads are now runtime-verified, and the supplied screenshot shows smoother distant road detail; overall visual parity remains unconfirmed.
- Add the FlatOut 2 launcher's video controls to the website, including anti-aliasing, anisotropy, buffering, post-processing, refresh rate, and aspect ratio; make each control affect the renderer or game configuration.
- Low graphics settings previously produced incorrect visuals; thoroughly test and fix every visual quality level.
- On low graphics settings, menu text is sometimes misaligned.
- On low graphics settings, text looks jagged; compare it with the original game.
- On low graphics settings, the audio popup text overflows its container; compare it with the original game.
- The touchscreen Nitrous button also pressed Gas; verify that it now activates nitrous independently.
- Verify the compact touchscreen menu action cluster (Del above Back/OK) and the combined Start/Pause race icon.
- Verify that button steering works alongside the analog FlatOut steering hook and that tilt mode places NoS with the left-side controls.
- The start page once failed to load after the language picker was removed; the cause is unclear, and the issue is believed to be fixed. Confirm before launch.
- Online car selection occasionally showed rapid jumps between rotation angles, but this has not been reproducible recently; monitor for recurrence.
- Intro video frame-rate drops are no longer observed; monitor for recurrence.
- GPU Inspector capture download is fixed, as far as currently known.
- GPU capture must freeze one submitted frame, read its texture/buffer payloads in bounded sequential chunks, and include CPU D3D draw provenance correlated through `GwDraw` to WebGPU commands; Inspector's all-at-once readbacks lose the device, and a GPU-only trace is insufficient to diagnose stunt visual artifacts.
- Widescreen support is postponed; the game intentionally uses its original 4:3 presentation for now. Previous experiments are recorded in `recomp/FLATOUT_WIDESCREEN.md`.
- Online mode and replacing NFS branding/names and restyling the website UI are done.
- The black square that intermittently appeared in the menu is no longer observed.

## Deferred touch-control follow-ups

- Define when the page switches between menu navigation controls and driving controls.
- Define the on-screen controller action mapping so touch controls follow player-rebound game inputs rather than hard-coded keys.
