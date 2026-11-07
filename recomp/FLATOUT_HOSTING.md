# FlatOut 2 browser hosting — relay-owned NAT negotiation

> **Status (final):** implemented and verified for same-LAN and cross-network hosting and
> guesting. The implementation diverged from the "terminate natneg" plan below in one
> important way: the relay **does not terminate** NAT negotiation. Terminating it stopped the
> host's INIT from reaching OpenSpy, so OpenSpy could never match host and guest — and the
> host's game also sends its natneg from a *second* socket (the GameSpy natneg library's own),
> which the relay cannot own anyway. The working approach is: pass natneg through untouched;
> present each hosted client under a consistent identity (a private/LAN client under
> `HOSTED_PEER_VIP:<syn>` with the join's reported address rewritten, a public client under its
> own public address with the reported address mapped back); and route `HOSTED_PEER_VIP` to the
> relay. The phases below are kept for the protocol facts and the reasoning that led here.

## Objective

Make a **browser-hosted** FlatOut 2 session indistinguishable from a desktop-hosted one:
a remote player (real desktop, or another browser) can search → join → race, on the same
LAN or across networks, exactly as two desktop clients do today.

Success = the browser host's game never learns an address it cannot reach. Every address the
game sees is a **relay-controlled** address that the relay can route to the real player.

## What "seamless" looks like (the reference)

Captures `fo2_host_session.pcapng` / `fo2_guest_session.pcapng` show two **desktop** clients
(`192.168.100.30` ↔ `192.168.100.32`) on one LAN:

1. Host reports its session to the GameSpy **master** (`134.122.16.249:27900`) — a QR report
   whose `localip0` is the host's real LAN address.
2. Guest queries the master, reads `localip0`, connects **directly** to it over UDP.
3. They exchange ~3000 datagrams on the game port `23756` (the race), direct, no relay.

The desktop path is `game → master (find) → peer (direct UDP)`. OpenSpy's only role is the
directory and, cross-network, NAT negotiation (natneg). The browser host cannot do the "direct
UDP" part, so the relay must stand in for it — and it must stand in *convincingly*.

## Protocol facts (decoded from captures + server logs)

### QR2 / master (UDP 27900, or TCP 28910)

- Query: `09 00 00 00 00 "flatout2pc" 00` (16 bytes).
- Empty query reply: `FE FD 09 00 00 00 00` (7 bytes).
- Host report: `03 <instance-id:4> <key 00 value 00 ...>` with `localip0`, `localport`,
  `natneg`, `statechanged`, `gamename`, `publicip`, `publicport`, `hostkey`, `hostname`,
  `gamever=FO14`, `gametype`, `mapname`, … `datachecksum`.
- Report ack: `FE FD 01 <instance-id:4> <session-key> 00` (28 bytes).
- Keepalive: `01 <instance-id:4> <session-key>`.

### NAT negotiation (UDP 27901 — `natneg1/2.openspy.net`)

Header is 8 bytes: `FD FC` (magic) `1E 66 6A B2` (protocol id) `02` (version) `<type>`.

| type | name | direction | body after header |
|---|---|---|---|
| `00` | NN_INIT | client → server | cookie(4) porttype(1) clientindex(1) usegameport(1) localip(4) localport(2) gamename\0 |
| `01` | NN_INITACK | server → client | cookie(4) flag(1) ?(1) ?(1) peerip(4) peerport(2) [pad] |
| `05` | NN_CONNECT | server → client | cookie(4) peerip(4) peerport(2) pad(2) |
| `07` | NN_CONNECT | client → peer | cookie(4) peerip(4) peerport(2) pad(2) |

Observed real bytes (from a `FO2_SPY_UDP_TRACE=1` host run):

- INIT: `FD FC 1E 66 6A B2 02 00 0C 5B 1B B9 00 01 01 0A 4D 01 02 00 00 flatout2pc 00`
  (cookie `0C5B1BB9`, localip `10.77.1.2`, localport `0`).
- INITACK (no peer yet): `FD FC 1E 66 6A B2 02 01 <cookie> 00 00 01 00 00 00 00 00 00`.
- CONNECT (peer): `FD FC 1E 66 6A B2 02 05 <cookie> <peerip> <peerport> 00 00`.
- Connect-to-peer: `FD FC 1E 66 6A B2 02 07 <cookie> <peerip> <peerport> 00 00`.

### Join / peer-connect packet (UDP game port `23756`, `0x6b`-length-prefixed)

- `[0]` = `0x6b` (length).
- `[2..6]` = cookie (4 bytes).
- `[10..14]` = `"FO14"`.
- `[28..32]` = the player's **reported** IP (its own view of itself — normally its LAN IP).
- `[44..46]` = reported port (normally `0x5ccc` = 23756).
- `[62..]` = `datachecksum` (a hash of game data — see note below).

The host's game sends the join/peer traffic to **both** the natneg-provided public address
*and* the reported address (see `desktop-connecting-to-browser.pcapng`: the guest fires the
`0x6b` join at `212.47.146.210:3355` and `10.77.0.2:23756` alternately). The relay must route
*both* to the real player.

### Race traffic

UDP `3658`, a star around the race's creator. No separate protocol to touch — it just needs the
same address translation as the join traffic.

## Why the current relay is wrong

`spy_udp.rs` is a pass-through: it forwards the host's natneg to OpenSpy and the game's peer
traffic to whatever address the game learned. Those addresses are unreachable:

- **Same LAN**: the natneg address is the shared router's WAN → hairpin.
- **Cross network / browser guest**: the "public address" is the *relay's own* address (every
  browser player collapses onto the bridge), so it cannot name a specific peer.
- **Reported address**: the host answers the guest's self-reported private IP (e.g.
  `192.168.100.30`); until recently the relay dropped it outright (`host reply to unknown player
  … dropped`). A partial fix (`reported_endpoint`) now maps it back — but the natneg leg and the
  *presentation* leg are still wrong.

## The fix: relay owns the host's NAT negotiation

The relay terminates the host's natneg and hands the game **one consistent, relay-routable
address per guest**, and rewrites the game's view so every address it sees (natneg result, join
content, packet source) is that same relay address.

Concretely, for a browser host with a joined guest whose real endpoint is `E`:

1. **Terminate natneg.** When the host sends `NN_INIT` to `natneg*.openspy.net:27901`, do not
   forward it. Reply `NN_INITACK` ourselves (echo the cookie). When the guest actually connects,
   send the host a forged `NN_CONNECT` naming `HOSTED_PEER_VIP:<syn>` (a relay VIP + a synthetic
   per-client port) instead of OpenSpy's unreachable public address.
2. **Rewrite the join content.** In the guest's `0x6b` join packet forwarded to the host, rewrite
   the reported IP/port (`[28..32]` / `[44..46]`) to `HOSTED_PEER_VIP:<syn>`, so the game's peer
   table equals the natneg address equals the packet source.
3. **Translate both ways.** Host→`HOSTED_PEER_VIP:<syn>` ⇒ send to `E`. `E`→host ⇒ present as
   `HOSTED_PEER_VIP:<syn>`. (The `HOSTED_PEER_VIP` machinery already exists; it must be extended
   to public peers and driven by a single per-guest identity.)
4. **Keep the LAN listener** for the same-LAN fast path, but present that client under the same
   relay identity so the host never sees two different addresses for one player.

The guest's own natneg (a real desktop talking to OpenSpy directly, or another browser's relay)
is unchanged: it learns the host's relay address from the QR/natneg it already receives, and its
connection into the relay is accepted as "unsolicited" (already implemented).

## Scope decisions (established, do not re-litigate)

- **Host side only.** The relay owns natneg for sessions *this browser hosts*. Guest-side
  search/join/race already works and must not regress.
- **Identity = `HOSTED_PEER_VIP:<syn>`.** One synthetic relay VIP + a per-client port, reused for
  natneg, join content, and packet source. No per-player fake IPs (avoids RFC1918 collisions).
- **Do not rewrite `datachecksum`.** The join packet's trailing checksum covers game data, not
  the address fields; only `[28..32]`/`[44..46]` are rewritten. If a live test shows the game
  rejects the rewritten packet, revisit (fallback: present the client under its *reported* IP and
  translate, which touches no bytes but risks address collisions — a documented tradeoff).
- **Backward compatible with `--spy-relay off` and guest mode.** All new logic is gated on the
  session being a *host* (the existing `hosting` flag), never on global config.

## Implementation phases (each independently testable)

### Phase 1 — natneg termination + forged NN_INITACK/NN_CONNECT

In `spy_udp.rs`'s worker, when `hosting` is set: intercept the `Target::Nat(0|1)` (natneg 27901)
outbound INIT, parse the cookie, and instead of forwarding, queue a forged `NN_INITACK`
(cookie echoed, peer zeroed). When a guest is accepted (hosted client created), queue a forged
`NN_CONNECT` for it naming `HOSTED_PEER_VIP:<syn>`. Add a bounded "natneg ownership" state per
session (cookie, whether a connect was already announced per client).

Unit tests: pure builders for `NN_INITACK`/`NN_CONNECT` bytes (round-trip the known captures'
bytes byte-for-byte); a worker-level test that a host INIT yields an INITACK from the relay and
no packet to `natneg*.openspy.net`.

### Phase 2 — rewrite the join packet's reported address

Extend the inbound path: when forwarding a hosted client's `0x6b` join to the host, rewrite
`[28..32]`/`[44..46]` to the client's `HOSTED_PEER_VIP` + `syn`. Add `rewrite_join(packet, vip,
syn)`. Unit test: feed the captured 109-byte join, assert only those 6 bytes change and the
`datachecksum` tail is untouched.

### Phase 3 — unify presentation + translation under the one identity

Make every hosted client (public *or* LAN-listener) resolve to a single `(syn, real_endpoint,
via_lan)` record used for: natneg connect target, join rewrite, inbound source rewrite, and
outbound `HostedAddr`/`HostedClient` translation. Retire the ad-hoc split between
`hosted_clients` / `public_reported` / `lan_by_ip` where it double-books one player. Unit tests:
one public client + one LAN client → both reachable, each under one distinct `syn`; the host's
reply to the rewritten address arrives at the right socket.

### Phase 4 — verification harness (no manual play)

Add an end-to-end test that drives two `Core` instances (host + guest) with the real UDP sockets,
asserting: QR report → guest query sees the relay address → guest join reaches host → host's
`0x6b` reply reaches guest → a stream of `3658` datagrams flows both ways. This is the "it must
work like two desktops" acceptance test, run headless.

## Test coverage summary

- Natneg byte builders (golden bytes from captures).
- `rewrite_join` (golden join packet, only address fields touched).
- Worker natneg-ownership state machine (INIT→INITACK, connect→CONNECT, no OpenSpy leak).
- Identity unification (public + LAN clients, distinct syns, both directions translated).
- Phase-4 headless two-Core race (the acceptance criterion).

## Open questions (to resolve during Phase 1, not blockers)

1. Exact `NN_INITACK` flag byte semantics (`00`/`01`/`02` seen). Mirror whatever OpenSpy sends;
   the forged INITACK should match the observed `00 00 01` shape first.
2. Whether the game requires a natneg *success* signal before allowing the race to start (if so,
   the forged `NN_CONNECT` must satisfy it — test empirically).
3. `localport` in NN_INIT is `0` from the browser; confirm a desktop sends its real port and
   whether the relay should echo it.

## Files

- `recomp/web/server-rs/crates/online/src/spy_udp.rs` — all relay work (natneg owner, join
  rewrite, identity unification).
- `recomp/runtime/web/net_lan.c` — page-side gateway (unchanged unless a new control frame is
  needed; currently none is).
- `recomp/web/test/lan-wasm.test.mjs` + `recomp/web/test/lan_unit.c` — Winsock unit tests
  (reference for how the existing harness drives the relay in tests).
