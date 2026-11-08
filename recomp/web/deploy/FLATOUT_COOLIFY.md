# FlatOut 2: GitHub Actions, GHCR and Coolify

The image contains the Rust server and frontend, **not** the game or WASM.
GitHub Actions publishes `ghcr.io/<owner>/<repo>-rs:sha-<full-commit>` on
`master` pushes. It never triggers a Coolify deployment. GHCR package visibility
is a GitHub setting: confirm the package is **private** (an existing package can
already be public). Game assets and the locally generated module stay off GitHub.

## Before the first release

1. Push the workflow and wait for the image job. Verify the image tag and OCI
   revision label correspond to the intended commit. The build context is
   `recomp/`; the Dockerfile is `recomp/web/server-rs/Dockerfile`.
2. On the VPS create `/srv/flatout/game`, `/srv/flatout/module/releases`, and
   `/srv/flatout/state`. Copy the tested GOG installation's `FlatOut2.exe`,
   `fo2a.bfs`, `fo2b.bfs`, `fo2c.bfs`, `patch1.bfs`, `filesystem`, and `patch`
   into `/srv/flatout/game`. Do not copy saves, uninstall logs, or unrelated
   files. Ensure the game and module directories are readable and searchable by
   container UID 1000; `/srv/flatout/state` must be writable by UID 1000. Check
   existing ownership before changing it. Allow enough disk space for the game,
   modules, image layers, and server state.
3. Create a Coolify **Docker Image** application for the private GHCR image;
   do not use the inherited NFS Compose stack or start another proxy on 80/443.
   Configure GHCR credentials in Coolify and Docker registry access for the VPS
   SSH account used by the release script: its `docker pull` must work without
   interactive login. Use a read-only package token, not a repository secret in
   the image. Configure the same destination server for Coolify and SSH.
4. Configure these persistent bind mounts in Coolify:
   `/srv/flatout/game:/game:ro`, `/srv/flatout/module:/module:ro`, and
   `/srv/flatout/state:/state` (writable). Do not mount over either individual
   module file. Set `NFSU2_DATA=/game`, `NFSU2_STATE=/state`,
   `NFSU2_HTTP_BIND=0.0.0.0`, `NFSU2_HTTP_PORT=8080`,
   `NFSU2_HOST=<public-hostname>`, `NFSU2_ONLINE=on`, `NFSU2_ROAM_HUB=off`,
   and create a runtime `NFSU2_WASM` variable (initially
   `/module/releases/not-yet-released/nfsu2.wasm`). The release script updates
   this value. The image's default command is `--behind-proxy`, required for
   `NFSU2_HTTP_BIND=0.0.0.0`; do not expose port 8080 directly to the Internet.
   Local non-container use defaults to loopback.

   The OpenSpy relay is a separate switch: enable it with `--spy-relay` on the
   container command (the bare flag uses the default table:
   `29900=gpcm.openspy.net:29900`, `29901=gpsp.openspy.net:29901`,
   `28900=available.openspy.net:28900`, `28910=flatout2pc.master.openspy.net:28910`),
   or with `NFSU2_SPY_RELAY` naming an explicit `port=host:port` table (an empty or
   unset value is off). **`--spy-relay` needs `NFSU2_ONLINE=on`** — the server
   refuses to start with the relay on and online off. See "OpenSpy relay and host
   networking" below for the UDP ports the relay's hosting/guesting needs.
5. Configure Coolify's existing HTTPS proxy for the hostname and container
   port 8080, with WebSockets supported. Disable automatic deployments and
   previews for this application. Do not manually deploy the placeholder
   module. Create a scoped Coolify API token with application read/update and
   deploy access; enable API access if necessary. Keep it outside Git, and
   never use a token with an untrusted URL. Authentication must protect the
   entire app, including `/data`, `/data-blocks`, and WebSockets: without it
   anyone who can access the server can download the copyrighted game files.

## Release from your local checkout

Build `recomp/out/web/nfsu2.wasm` and `nfsu2.json` locally using the existing
FlatOut build and your own executable. The metadata must report a **clean**
`flatout2-<git revision>` and its recorded size must match. The script resolves
the revision in your local Git repository, checks the full-commit image's OCI
revision label using `docker pull`/`inspect` on the VPS, and verifies SHA-256
for both files after uploading. It uses an immutable release directory per
commit and content hashes. A revision in a JSON file is a claim, not a
cryptographic proof of how the WASM was built: use a trusted build and verified
lift. The previously tested module with `+` and `lift: unknown` is not a clean,
reproducible release artifact; make a clean build for a normal release.

Install Python 3, Git, SSH/SCP locally. The VPS needs Bash, Docker, `sha256sum`,
and permission for the SSH user to create directories under
`/srv/flatout/module/releases`. The script never uploads the executable.

```sh
export FLATOUT_IMAGE='ghcr.io/<lowercase-owner>/<lowercase-repo>-rs'
export COOLIFY_URL='https://<your-coolify-host>'
export COOLIFY_APP_UUID='<docker-image-application-uuid>'
export COOLIFY_TOKEN='<scoped-api-token>'
recomp/web/deploy/release.sh -n user@your-vps recomp/out/web
recomp/web/deploy/release.sh user@your-vps recomp/out/web
```

### Explicit unverified development release

To use the existing working `flatout2-2fee04b+` module with the published
server image without rebuilding, first run the local-only check, then, only when
ready to deploy, run the second command with your configured environment and VPS:

```sh
recomp/web/deploy/release.sh -n --dev-release --image-commit 07cd9945381cb21b24cc22cba158243e0849f3da user@your-vps recomp/out/web
recomp/web/deploy/release.sh --dev-release --image-commit 07cd9945381cb21b24cc22cba158243e0849f3da user@your-vps recomp/out/web
```

Both flags are required on every development invocation. This combination is
**unverified**: the metadata revision is a claim, not proof of WASM provenance
or compatibility with the image. The original JSON is unchanged. Each staged
release includes a hash-verified `release.json` recording the WASM revision
claim and hash, metadata hash, image commit and verification status.

`-n` checks only local metadata and prints the image/release; it does not check
GHCR, SSH or Coolify. The real command reads Coolify configuration, verifies
the image on the VPS, uploads to a unique staging directory, verifies both
SHA-256 hashes, and publishes the directory by rename. Only then does it update
Coolify's existing `NFSU2_WASM` and image settings and explicitly trigger a
deployment. It waits for Coolify to report `finished`, not merely webhook
acceptance. It does **not** independently verify the actual running image or
public WASM: check Coolify logs, `/game.json`, image identity, HTTPS headers
(`Cross-Origin-Opener-Policy: same-origin`,
`Cross-Origin-Embedder-Policy: require-corp`) and functionality before calling
the release successful. A failed/ambiguous API call after the two configuration
updates may leave saved Coolify settings changed even if the running container
was not changed: review them before a later deployment. Interrupted uploads can
leave `.incoming-*` directories for manual inspection/cleanup.

No automatic rollback is implemented. To roll back, select a previously retained
image tag and its matching `/module/releases/<id>/nfsu2.wasm` in Coolify, then
explicitly deploy. Never change or delete `/game` or `/state` to roll back.

## OpenSpy relay and host networking

The OpenSpy relay has two halves that behave differently on the Docker bridge
network (which the Coolify Docker Image resource uses):

- **Search** (the game's TCP connections to 28900/28910/29900/29901, and the
  *solicited* UDP to OpenSpy's 27900 master / 27901 natneg) works on the bridge:
  those are outbound, and Docker's NAT forwards the responses back.
- **Hosting and guesting** (a remote player's join/race datagrams, *unsolicited*
  inbound UDP to the address OpenSpy advertised) does **not** work on the bridge:
  the inbound source never matches Docker's outbound conntrack, so the packets
  are dropped.

The fix is `network_mode: host`, so the relay's UDP sockets bind directly to the
VPS's public IP. The relay binds ephemeral UDP ports by default; the new
`NFSU2_SPY_UDP_PORTS=28100-28355` bounds them to a known range (one per concurrent
session, matching the relay's session cap) so the firewall can allow exactly
that range.

### Hetzner firewall

- Allow inbound **UDP 28100–28355** only (not all UDP). Widen the range and the
  rule together if you later need more concurrent sessions.
- TCP 80/443 is already open for Coolify's Traefik.

### Coolify and host networking

- Coolify's **Custom Docker Options does not support `--network=host`**
  (confirmed), so the existing Docker Image resource cannot be switched to host
  networking by editing its options.
- The workable path is to run the server from a **Compose** file with
  `network_mode: host` (see `compose.flatout.staging.yml`).
- Coolify's proxy (`coolify-proxy`) resolves `host.docker.internal` to
  `10.0.0.1` (confirmed), and can reach an HTTP service on the VPS through that
  hostname (confirmed). So Traefik can route a hostname to
  `http://host.docker.internal:18081`.

### Staging (do not run in production)

`compose.flatout.staging.yml` is a throwaway host-networked instance to prove the
path: relay OFF, HTTP on 18081, and its own state folder, so the live production
container and Graphito stay untouched.

1. Make a separate state folder (never the production one):
   `mkdir -p /srv/flatout/state-staging && chown 1000:1000 /srv/flatout/state-staging`.
2. Next to the compose file, write a `.env`:
   `FLATOUT_IMAGE=ghcr.io/<owner>/<repo>-rs:sha-<commit>`,
   `STAGING_HOST=staging.example.com`, `FLATOUT_GAME=/srv/flatout/game`,
   `FLATOUT_MODULE=/srv/flatout/module`, `FLATOUT_STATE_STAGING=/srv/flatout/state-staging`,
   `NFSU2_WASM=/module/releases/<id>/nfsu2.wasm`.
3. `docker compose -f compose.flatout.staging.yml up -d`.
4. Verify the network mode (below), then route the staging hostname through
   Traefik to `host.docker.internal:18081`, and open `https://staging.example.com/`.
5. Tear down with `docker compose -f compose.flatout.staging.yml down` (keep the
   staging `.env` for the next test).

Block 18081/tcp inbound for the duration of the test: with host networking it is
otherwise reachable from the Internet.

### Verify the network mode

```sh
docker inspect flatout-staging-server-1 --format '{{.HostConfig.NetworkMode}}'   # must print: host
docker inspect flatout-staging-server-1 --format '{{json .NetworkSettings.Networks}}'  # must print: {}
```

### Traefik routing for the staging hostname

Whether Coolify's UI can point a hostname directly at `host.docker.internal:18081`
depends on the Coolify version. Two cases:

- **Coolify manages it directly** (a "Custom domain"/hostname with an
  address/port upstream, or a proxy "target" set to `http://host.docker.internal:18081`):
  use that, no extra file.
- **Coolify needs a Traefik dynamic config** (a `file` provider): add a router
  and service for the staging hostname, e.g.

  ```yaml
  http:
    routers:
      flatout-staging:
        rule: "Host(`staging.example.com`)"
        service: flatout-staging
        tls:
          certResolver: <coolify's resolver>
    services:
      flatout-staging:
        loadBalancer:
          servers:
            - url: "http://host.docker.internal:18081"
  ```

Confirm which case applies against your Coolify version before touching the
live proxy, and make sure the new route does not overlap Graphito's or the
production FO2 route (a distinct `Host` rule is enough).

### Rolling back the staging test

`docker compose -f compose.flatout.staging.yml down` removes only the staging
container. It shares no network, state or volume with production, so Graphito and
the live FO2 app are unaffected. There is nothing to undo on the VPS beyond the
container and the staging state folder.

### Release script, if migrating to Compose

The release script (`release.sh`) today updates the Coolify Docker Image resource
through the Coolify API (set `NFSU2_WASM` and the image, then trigger a deploy).
If production moves to a Compose deployment, the equivalent would be: update the
Compose `.env`'s `FLATOUT_IMAGE` and `NFSU2_WASM`, then `docker compose up -d`
(or the deploy step the script triggers). That is a later change; the staging
compose is not wired into the release script yet.
