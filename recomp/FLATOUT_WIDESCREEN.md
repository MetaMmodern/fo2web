# FlatOut 2 widescreen experiments (postponed)

FlatOut 2 currently uses the original 4:3 presentation. The experimental
widescreen runtime changes were removed after repeated menu and HUD regressions.
This file records the work so a future attempt can start from the evidence
rather than repeating the same approaches.

## Constraints and reference behavior

- The browser drawable should eventually follow the window aspect ratio.
- 3D scenes should use a wider view without stretching or offsetting the car.
- HUD, text, icons, controls, and intro artwork must retain their proportions.
- Menu backdrops may fill the width, while interface groups need deliberate
  left, center, or right anchoring.
- The installed original `ZPatchFO2.asi` is the useful reference. Its source is
  unavailable, and an unrelated open-source reimplementation was not trusted.
- The inherited lift is unstamped, so experimental WASM builds used
  `SKIP_GENCHECK=1` and remain unverified builds.

## Static findings

- The installed `FlatOut2.exe` remained unchanged; ZPatch was loaded through
  `winmm.dll` and `ZPatchFO2.asi`.
- ZPatch projection hooks include `0x004cbdef`, `0x004c9f63`, `0x0059964a`,
  and `0x004c9e27`. Those exact hook sites are absent from the current lift.
- ZPatch hooks `0x00533683` and `0x00533690` inside lifted
  `sub_0053366e`. That function converts source UI rectangles before FlatOut
  emits their draw geometry and is reached through a jump-table tail dispatch
  from `sub_00533590`.
- ZPatch also redirects the dimension loads at `0x005aad12` and `0x005aad22`.
  Those changes assume the game itself is operating with the corresponding
  patched widescreen dimensions; copying isolated formulas into the current
  renderer-side mapping is not equivalent.
- A car-selection capture showed separate draws for left statistics, centered
  controls, and the right-side car name. Individual draw calls are not reliable
  UI-element boundaries.

## Experiments and outcomes

1. **Browser-sized drawable with renderer aspect compensation**
   - Rendered to the browser dimensions while preserving FlatOut's mode list.
   - A full-scene `StretchRect` incorrectly cropped from x=551 and stretched
     2521 pixels across a 3072-pixel target. Correcting that copy was verified
     to center the race car and remove horizontal stretching.
   - Static menu backdrops and the animated pause backdrop were made full width
     and were verified by the user.
   - The HUD and interactive menu content remained in the centered legacy area.

2. **Per-draw edge anchoring**
   - Shifted qualifying left/right orthographic draw batches without stretching.
   - Failed because one logical widget can use several draws. It split profile
     rows, car and track carousels, car names from class labels, dialog borders
     from panels, button prompts, and song text from its notification backing.

3. **ZPatch rectangle-rule transplant**
   - Intercepted `sub_0053366e` and reproduced the recovered coordinate tests.
   - Kept geometry from an individual rectangle together, but the formulas
     depended on ZPatch's other resolution/layout changes. Menus and HUD stayed
     inset, the race-selection `NAME` label detached, and the garage car widget
     appeared to change composition.

4. **Virtual layout width and left-origin mapping**
   - Wrapped `sub_005aad10` to expose a browser-aspect virtual width and moved
     orthographic coordinates to the drawable's left origin.
   - Left-side content aligned better, but center and right content were all
     squeezed toward the left.

5. **Continuous per-batch horizontal anchoring**
   - Added a 0..2 side-gap translation based on each draw batch's original
     horizontal center to restore left, center, and right placement.
   - Still did not produce a coherent layout and retained the fundamental
     problem that draw batches are not complete UI groups.

## Other observations

- Intro artwork was stretched by broad full-width classification and needs an
  explicit aspect-fit path in any future implementation.
- The loading bar appeared horizontally offset and needs its own identification.
- The GPU Inspector capture work was separate from widescreen rendering. Capture
  pools were split to avoid a nearly 2 GB single allocation, but browser capture
  downloads still had an unresolved connection-error report.

## Recommended future direction

Do not restore any of the removed draw-level or isolated rectangle heuristics.
A future implementation should reproduce ZPatch's widescreen coordinate system
as a complete set: resolution/layout dimensions, scale globals, projection, and
the relevant element-specific exceptions. Validate menu composition and HUD
anchors together before enabling widescreen by default.
