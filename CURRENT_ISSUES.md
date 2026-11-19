# Current issues

- Audit production server load and security; address any risks found.
- Multiplayer search, joining, hosting, and racing have been verified for browser guests, remote players joining a browser host, and same-LAN players joining a browser host. However, players occasionally see “no players left” just before a race starts; investigate this intermittent failure.
- Compared with the original at the same settings, the overall image looks somewhat bleached, as if some shaders or overall glow and sunbeams are missing. Investigate shader/effect coverage against the original.
- Low graphics settings previously produced incorrect visuals; thoroughly test and fix every visual quality level.
- Touchscreen driving controls are incomplete, and changing the steering type has no apparent effect; verify and fix the controls for FlatOut.
- The start page once failed to load after the language picker was removed; the cause is unclear, and the issue is believed to be fixed. Confirm before launch.
- Online car selection occasionally showed rapid jumps between rotation angles, but this has not been reproducible recently; monitor for recurrence.
- Intro video frame-rate drops are no longer observed; monitor for recurrence.
- GPU Inspector capture download is fixed, as far as currently known.
- Widescreen support is postponed; the game intentionally uses its original 4:3 presentation for now. Previous experiments are recorded in `recomp/FLATOUT_WIDESCREEN.md`.
- Online mode and replacing NFS branding/names and restyling the website UI are done.
- The black square that intermittently appeared in the menu is no longer observed.
