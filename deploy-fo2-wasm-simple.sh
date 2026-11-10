#!/usr/bin/env bash
set -euo pipefail

# Upload local, prebuilt WASM + JSON to the VPS. Keep all previous releases.
# One-time Coolify NFSU2_WASM setting: /module/current/nfsu2.wasm
# Usage: FO2_SSH_HOST=your-ssh-alias ./deploy-fo2-wasm-simple.sh

SSH_HOST="${FO2_SSH_HOST:-fo2-vps}"
ROOT="$(git rev-parse --show-toplevel)"
WASM="$ROOT/recomp/out/web/nfsu2.wasm"
JSON="$ROOT/recomp/out/web/nfsu2.json"
REMOTE_BASE="/srv/flatout/module"

[[ -s "$WASM" && -s "$JSON" ]] || {
  echo "Missing WASM or JSON in recomp/out/web" >&2
  exit 1
}

WASM_HASH="$(shasum -a 256 "$WASM" | awk '{print $1}')"
JSON_HASH="$(shasum -a 256 "$JSON" | awk '{print $1}')"
RELEASE="wasm-${WASM_HASH:0:16}-${JSON_HASH:0:12}"

echo "Uploading to $SSH_HOST: $RELEASE"
TMP="$(ssh "$SSH_HOST" "mktemp -d '$REMOTE_BASE/releases/.upload-XXXXXXXX'")"

# Clean up a failed upload; never touch an existing release.
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

# Atomic update of the stable symlink; prior releases stay as backups.
cd "$BASE"
ln -s "releases/$RELEASE" .current-next
mv -Tf .current-next current

echo "Current: $(readlink current)"
echo "Previous release directories remain in $BASE/releases/"
REMOTE

trap - EXIT
# Successful upload's temp dir has been moved (or deleted if it already existed).
echo "Ready. In Coolify set NFSU2_WASM=/module/current/nfsu2.wasm (once), then click Redeploy."
