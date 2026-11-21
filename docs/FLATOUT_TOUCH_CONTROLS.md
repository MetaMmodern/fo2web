# FlatOut Touch Controls

## Status

The web page contains a touch-control overlay inherited from the NFSU2 web
project and redesigned for FlatOut. FlatOut now has a separate `drive` status
classifier in `recomp/runtime/web/status_web.c`; it does not consult NFSU2
game-flow, HUD packages, or memory layouts. Runtime verification on a touch
device is still required.

## Future feature

Add a small FlatOut-owned status hook that publishes enough state for the page
to choose a layout:

```ts
mode: 'menu' | 'loading' | 'race' | 'paused' | 'results'
manualGearbox: boolean
```

The page should use this state to show only controls FlatOut supports:

- **Menu:** touch-as-mouse plus navigation / confirm / back where needed.
- **Race:** steering, throttle, brake/reverse, handbrake, camera, reset,
  pause, and manual gear controls only when applicable.

Phone tilt is not supplied by a touch-control library.  It should use
`DeviceOrientationEvent`, normalize phone roll to a `-1..1` steering axis,
and pass that to FlatOut's virtual controller input.  iOS requires permission
to be requested from a user gesture.

## Investigation approach

This should require narrow state discovery, not broad reverse engineering:

1. Inspect existing FlatOut runtime hooks for a game-flow, race-manager, HUD,
   pause, or scene-state signal.
2. `FO2_TOUCH_PROBE=1` enables the bounded FlatOut-only execution probe in
   `recomp/runtime/com.c`. It reports changing per-frame DirectInput polling
   and Direct3D draw call-site signatures as `[fo2-touch]`, and input lifecycle
   transitions as `[fo2-touch-input]`. The lifted return addresses identify
   FlatOut code paths without guessing memory layouts. Escape open/close cycles
   additionally produce `[fo2-touch-key]` and `[fo2-touch-toggle]`: successive
   snapshots intersect only small game-owned fields that reverse consistently
   as the in-race menu opens and closes. The probe forces `drive=0`; no
   inherited NFSU2 game-flow, HUD, package, pause, or gearbox classifier is
   consulted.

The resulting FlatOut classifier combines the race-only world draw path with
FlatOut's in-race-menu condition at `0x008dbe34`, which the three-toggle trace
observed reversing `0 -> 1 -> 0 -> 1`. That condition also changes during
look-back, so it is sampled and latched only after FlatOut consumes Escape or
an in-menu Enter. The classifier publishes `drive=1` only while the world is
active and the latched in-race menu is closed. Race entry requires its unique
world draw caller. After entry, race state follows FlatOut's local-player
control callback, which continues while stationary and across camera changes.

FlatOut enumerates DirectInput controllers before the page's touch-only virtual
pad exists. The web build therefore wraps `sub_0046fa50`, FlatOut's confirmed
player-to-vehicle control writer, and supplies the touch pad's proportional LX
at the shaped steering channel `player+0x684` immediately before it is
propagated and consumed by simulation. Thumbstick and tilt steering remain
analog rather than being converted to left/right key presses.
3. Have a player perform one sequence: menu, loading, countdown/race, pause,
   results, menu; collect the `[fo2-touch]` lines and use them to select stable
   signatures.
4. Implement the classifier in FlatOut runtime code and export it through the
   existing shared web status mechanism.
5. Wire touch layouts to that FlatOut status, then verify on a real touch
   device.

Do not infer the state from the canvas or DOM: those are not reliable game
state signals.

## Optional UI dependency

`@rbuljan/gamepad` (MIT) is a possible widget layer for customizable
multitouch buttons, D-pads, and horizontal joysticks.  It does not supply
device-orientation steering or FlatOut state detection, so it would only
replace some overlay UI; the FlatOut input adapter and status hook remain
necessary.
