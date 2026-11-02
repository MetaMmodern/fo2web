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
   `NFSU2_HOST=<public-hostname>`, `NFSU2_ONLINE=off`, `NFSU2_ROAM_HUB=off`,
   and create a runtime `NFSU2_WASM` variable (initially
   `/module/releases/not-yet-released/nfsu2.wasm`). The release script updates
   this value. The image's default command is `--behind-proxy`, required for
   `NFSU2_HTTP_BIND=0.0.0.0`; do not expose port 8080 directly to the Internet.
   Local non-container use defaults to loopback.
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
reproducible release artifact; make a clean build before release.

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
