#!/usr/bin/env bash
set -euo pipefail

# Build and deploy the production game module. Releases are immutable and the
# stable host-side "current" symlink is changed atomically.
#
# Usage:
#   ./deploy-fo2-wasm-simple.sh
#   ./deploy-fo2-wasm-simple.sh --no-build
#   ./deploy-fo2-wasm-simple.sh --no-restart
#
# Overrides:
#   FO2_SSH_HOST       SSH alias (default: hetzner-coolify)
#   FO2_CONTAINER      production container (default: flatout-prod-server-1)
#   FO2_PUBLIC_URL     public origin (default: https://flatout2.online)
#   FO2_REMOTE_MODULE  host module directory (default: /srv/flatout/module)

SSH_HOST="${FO2_SSH_HOST:-hetzner-coolify}"
CONTAINER="${FO2_CONTAINER:-flatout-prod-server-1}"
PUBLIC_URL="${FO2_PUBLIC_URL:-https://flatout2.online}"
REMOTE_BASE="${FO2_REMOTE_MODULE:-/srv/flatout/module}"
BUILD=true
RESTART=true

usage() {
  echo "Usage: $0 [--no-build] [--no-restart]" >&2
  exit 2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-build) BUILD=false ;;
    --no-restart) RESTART=false ;;
    -h|--help)
      sed -n '3,16p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *) usage ;;
  esac
  shift
done

ROOT="$(git rev-parse --show-toplevel)"
WASM="$ROOT/recomp/out/web/nfsu2.wasm"
JSON="$ROOT/recomp/out/web/nfsu2.json"

if "$BUILD"; then
  # The embedded revision must describe exactly what was built. Untracked files
  # are allowed because Git's build revision intentionally ignores them.
  if ! git -C "$ROOT" diff --quiet || ! git -C "$ROOT" diff --cached --quiet; then
    echo "Refusing production build with uncommitted tracked changes" >&2
    exit 1
  fi

  echo "Checking that HEAD is published on origin/master"
  git -C "$ROOT" fetch --quiet origin master
  HEAD_SHA="$(git -C "$ROOT" rev-parse HEAD)"
  REMOTE_SHA="$(git -C "$ROOT" rev-parse FETCH_HEAD)"
  [[ "$HEAD_SHA" == "$REMOTE_SHA" ]] || {
    echo "Refusing production deploy: HEAD is not origin/master" >&2
    echo "  HEAD:          $HEAD_SHA" >&2
    echo "  origin/master: $REMOTE_SHA" >&2
    exit 1
  }

  echo "Building a clean production WASM"
  "$ROOT/recomp/web/build.sh"
fi

[[ -s "$WASM" && -s "$JSON" ]] || {
  echo "Missing WASM or JSON in recomp/out/web" >&2
  exit 1
}

WASM_SIZE="$(wc -c < "$WASM" | tr -d ' ')"
read -r REV JSON_SIZE LIFT < <(
  python3 - "$JSON" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as f:
    metadata = json.load(f)
print(metadata.get("rev", ""), metadata.get("size", ""), metadata.get("lift", ""))
PY
)
EXPECTED_REV="flatout2-$(git -C "$ROOT" rev-parse --short HEAD)"
[[ "$REV" == "$EXPECTED_REV" ]] || {
  echo "Metadata revision is not a clean build of HEAD: $REV (expected $EXPECTED_REV)" >&2
  exit 1
}
[[ "$JSON_SIZE" == "$WASM_SIZE" ]] || {
  echo "Metadata size mismatch: $JSON_SIZE (actual $WASM_SIZE)" >&2
  exit 1
}
[[ "$LIFT" != "unknown" && -n "$LIFT" ]] || {
  echo "Refusing release with unknown lift provenance" >&2
  exit 1
}

WASM_HASH="$(shasum -a 256 "$WASM" | awk '{print $1}')"
JSON_HASH="$(shasum -a 256 "$JSON" | awk '{print $1}')"
RELEASE="wasm-${REV#flatout2-}-${WASM_HASH:0:16}-${JSON_HASH:0:12}"

echo "Uploading $REV to $SSH_HOST:$REMOTE_BASE/releases/$RELEASE"
TMP="$(ssh "$SSH_HOST" "mktemp -d '$REMOTE_BASE/releases/.upload-XXXXXXXX'")"

# Clean up an interrupted upload; never remove a published release.
cleanup() { ssh "$SSH_HOST" "rm -rf -- '$TMP'" >/dev/null 2>&1 || true; }
trap cleanup EXIT

scp "$WASM" "$JSON" "$SSH_HOST:$TMP/"

ssh "$SSH_HOST" bash -s -- "$TMP" "$REMOTE_BASE" "$RELEASE" "$WASM_HASH" "$JSON_HASH" <<'REMOTE'
set -euo pipefail
TMP="$1"; BASE="$2"; RELEASE="$3"; WASM_HASH="$4"; JSON_HASH="$5"
TARGET="$BASE/releases/$RELEASE"

verify() {
  [[ "$(sha256sum "$1/nfsu2.wasm" | awk '{print $1}')" == "$WASM_HASH" ]]
  [[ "$(sha256sum "$1/nfsu2.json" | awk '{print $1}')" == "$JSON_HASH" ]]
}

verify "$TMP" || { echo 'Upload checksum mismatch' >&2; exit 1; }
chmod 755 "$TMP"
chmod 644 "$TMP/nfsu2.wasm" "$TMP/nfsu2.json"

if [[ -e "$TARGET" ]]; then
  verify "$TARGET" || { echo "Existing release differs: $TARGET" >&2; exit 1; }
  rm -rf -- "$TMP"
else
  mv -- "$TMP" "$TARGET"
fi

cd "$BASE"
ln -s "releases/$RELEASE" .current-next
mv -Tf .current-next current
echo "Current: $(readlink current)"
REMOTE

trap - EXIT

if "$RESTART"; then
  echo "Restarting $CONTAINER and waiting for health"
  ssh "$SSH_HOST" bash -s -- "$CONTAINER" "$REMOTE_BASE" "$WASM_HASH" "$JSON_HASH" <<'REMOTE_RESTART'
set -euo pipefail
CONTAINER="$1"; BASE="$2"; WASM_HASH="$3"; JSON_HASH="$4"

docker inspect "$CONTAINER" >/dev/null
docker inspect "$CONTAINER" --format '{{range .Config.Env}}{{println .}}{{end}}' |
  grep -Fx 'NFSU2_WASM=/module/current/nfsu2.wasm' >/dev/null || {
    echo 'Container is not configured for /module/current/nfsu2.wasm' >&2
    exit 1
  }
docker inspect "$CONTAINER" --format '{{range .Mounts}}{{println .Source "|" .Destination}}{{end}}' |
  grep -Fx "$BASE | /module" >/dev/null || {
    echo "Container does not mount $BASE at /module" >&2
    exit 1
  }

docker restart "$CONTAINER" >/dev/null
status=""
for _ in $(seq 1 60); do
  status="$(docker inspect "$CONTAINER" \
    --format '{{if .State.Health}}{{.State.Health.Status}}{{else}}{{.State.Status}}{{end}}')"
  [[ "$status" == "healthy" ]] && break
  [[ "$status" == "unhealthy" || "$status" == "exited" || "$status" == "dead" ]] && break
  sleep 1
done
[[ "$status" == "healthy" ]] || {
  echo "Container failed to become healthy: $status" >&2
  docker logs --tail 80 "$CONTAINER" >&2
  exit 1
}

[[ "$(docker exec "$CONTAINER" sha256sum /module/current/nfsu2.wasm | awk '{print $1}')" == "$WASM_HASH" ]]
[[ "$(docker exec "$CONTAINER" sha256sum /module/current/nfsu2.json | awk '{print $1}')" == "$JSON_HASH" ]]
echo "Container healthy; mounted WASM and metadata hashes verified"
REMOTE_RESTART
else
  echo "Upload complete; container restart and public verification skipped"
fi

if "$RESTART"; then
  echo "Verifying public WASM"
  HEADERS="$(curl -fsSI -H 'Accept-Encoding: identity' "$PUBLIC_URL/game.wasm")"
  HTTP_STATUS="$(printf '%s\n' "$HEADERS" | awk 'toupper($1) ~ /^HTTP\// {status=$2} END {print status}')"
  PUBLIC_SIZE="$(printf '%s\n' "$HEADERS" | awk 'tolower($1) == "content-length:" {gsub("\r", "", $2); size=$2} END {print size}')"
  [[ "$HTTP_STATUS" == "200" ]] || {
    echo "Public WASM returned HTTP $HTTP_STATUS" >&2
    exit 1
  }
  [[ "$PUBLIC_SIZE" == "$WASM_SIZE" ]] || {
    echo "Public WASM size mismatch: $PUBLIC_SIZE (expected $WASM_SIZE)" >&2
    exit 1
  }

  echo "Deployed $REV successfully"
  echo "  release: $REMOTE_BASE/releases/$RELEASE"
  echo "  sha256:  $WASM_HASH"
  echo "  public:  $PUBLIC_URL/game.wasm ($PUBLIC_SIZE bytes)"
fi
