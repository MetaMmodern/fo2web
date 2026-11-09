# Current issues

- Public launch priority: add online mode.
- Public launch priority: replace NFS branding and names throughout the website UI and restyle it.
- Public launch priority: configure a better domain.
- Public launch priority: automate deployment.
- Widescreen support is postponed; the game intentionally uses its original 4:3 presentation for now. Previous experiments are recorded in `recomp/FLATOUT_WIDESCREEN.md`.
- Multiplayer online verification:

  | role | search | join | race |
  |---|---|---|---|
  | browser as guest (any network) | done | done | done |
  | browser as host, remote players | done | done | done |
  | browser as host, same-LAN player | done | done | done |

  Browser hosting (same-LAN and cross-network) is fixed, documented in `recomp/FLATOUT_HOSTING.md`. The relay presents each hosted client under a consistent identity — a LAN-listener (private) client under `HOSTED_PEER_VIP:<syn>` with its join packet's reported address rewritten to that identity, a public client under its own public address with its reported private address mapped back to the real endpoint — and translates the host's replies both ways. The switch routes `HOSTED_PEER_VIP` to the relay, and the page forwards every non-subnet UDP to the gateway for the server to validate. NAT negotiation itself passes through to OpenSpy untouched (terminating it would stop the host and guest from ever matching).
- Multiplayer online, separate user observation (not proven): after a guest disconnects, it can appear to be the host, and the stale session remains listed but cannot be joined.
- Deployed multiplayer races are intermittent: a browser guest and desktop host succeeded on the same LAN, but a cross-network attempt and subsequent retries failed. Search, joining, and racing require separate verification; the cause remains unconfirmed.
- GPU Inspector can generate a capture but its download fails immediately with a browser connection error.
- Daytime scenes have flatter lighting than the original; the remaining difference after the glow fixes needs confirmation.
- A black square intermittently appears in the menu; it does not bother the user for now.
- Painted road markings appear faded in sunlight compared with shade; compare with the original test track to determine whether this is expected.
