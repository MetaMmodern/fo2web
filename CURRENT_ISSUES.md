# Current issues

- Public launch priority: add online mode.
- Public launch priority: replace NFS branding and names throughout the website UI and restyle it.
- Public launch priority: configure a better domain.
- Public launch priority: automate deployment.
- Widescreen support is postponed; the game intentionally uses its original 4:3 presentation for now. Previous experiments are recorded in `recomp/FLATOUT_WIDESCREEN.md`.
- Multiplayer online: OpenSpy race search works after correcting the Winsock import argument counts, but joining any listed race reports "this race is not available anymore". The cause of the join failure is not yet established. Hosting and racing with community players remain unverified. Public peers cannot initiate UDP to a browser that has not contacted them.
- GPU Inspector can generate a capture but its download fails immediately with a browser connection error.
- Daytime scenes have flatter lighting than the original; the remaining difference after the glow fixes needs confirmation.
- A black square intermittently appears in the menu; it does not bother the user for now.
- Painted road markings appear faded in sunlight compared with shade; compare with the original test track to determine whether this is expected.
- The game always starts with the same song; the original reportedly starts with a different one each time.
