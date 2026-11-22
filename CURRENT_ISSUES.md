# Current issues

- Audit production server load and security; address any risks found.
- Multiplayer search, joining, hosting, and racing have been verified for browser guests, remote players joining a browser host, and same-LAN players joining a browser host. However, players occasionally see “no players left” just before a race starts; investigate this intermittent failure.
- Compared with the original at the same settings, the overall image looks somewhat bleached, as if some shaders or overall glow and sunbeams are missing. Investigate shader/effect coverage against the original.
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
