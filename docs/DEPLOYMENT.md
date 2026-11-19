# Deployment

FlatOut 2 Web is deployed using GitHub Actions, GitHub Container Registry
(GHCR), Docker Compose, and Coolify.

**Production:** <https://flatout2.online>

## Architecture

- **GitHub Actions** builds the Rust server and web frontend into a
  multi-platform Docker image.
- **GHCR** hosts images at `ghcr.io/metammodern/fo2web-rs`, tagged with
  `latest` and `sha-<commit>`.
- **Coolify** manages production using the Git-backed Compose configuration at
  `recomp/web/deploy/compose.flatout.prod.yml`.
- **Traefik** provides HTTPS termination and reverse proxying.
- **Docker host networking** supports UDP communication required for OpenSpy
  multiplayer.
- Game assets and WASM are stored separately from the Docker image and mounted
  into the container.

## Automatic deployment

Server and frontend changes are deployed automatically:

1. Push changes to `master`.
2. GitHub Actions builds and publishes the Docker image to GHCR.
3. After publishing succeeds, GitHub Actions triggers the Coolify deployment
   webhook.
4. Coolify pulls the updated `latest` image using `pull_policy: always` and
   redeploys the application.

No manual server access or image-tag updates are required.

## WASM deployment

The WebAssembly game module is built locally and is not included in the Docker
image.

1. Build and verify `recomp/out/web/nfsu2.wasm` and `nfsu2.json`.
2. Upload both files using the WASM deployment script.
3. Preserve the previous module version for rollback.
4. Update the active module path if required.
5. Manually redeploy the application through Coolify.

Deploy server and WASM changes together when they require compatibility updates.

## Configuration and persistent data

The Compose configuration is version-controlled, while environment-specific
settings are managed through Coolify.

Game assets and WASM modules are mounted read-only. Persistent application state
is stored separately and survives container redeployments.

Production HTTPS routing is configured through Traefik. Multiplayer requires
an explicitly configured UDP port range.

Staging and production are maintained separately. Their multiplayer UDP
configurations must not conflict.

## Rollback

- **Server/frontend:** Deploy a previous immutable `sha-<commit>` Docker image.
- **WASM:** Restore a previously verified module release and redeploy.
- **Game data:** Restore from retained backups when necessary.

## Security

- Credentials and API tokens are stored outside the repository.
- GitHub Actions secrets are used for deployment authentication.
- Production secrets must not be embedded in Docker image layers or build
  history.
- Only required network ports should be publicly accessible.
