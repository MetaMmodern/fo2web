#!/bin/bash
# Local FlatOut 2 browser smoke test.  The large GOG archives stay in place and
# are streamed by the Rust server; only the page and wasm module live here.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
WEB="$ROOT/recomp/web"
DATA="${FLATOUT2_DATA:-/Volumes/transferbuf/FlatOut 2}"
PORT="${FLATOUT2_PORT:-8080}"
SERVER="$WEB/../out/web/server-rs/nfsu2-server"

if [ ! -f "$DATA/FlatOut2.exe" ]; then
  echo "FlatOut2.exe not found in: $DATA" >&2
  echo "Set FLATOUT2_DATA to the GOG install folder with FlatOut2.exe and the .bfs archives." >&2
  exit 1
fi
if [ ! -x "$SERVER" ]; then
  echo "Missing server: $SERVER" >&2
  echo "Run: cd recomp/web/server-rs && cargo build --release" >&2
  exit 1
fi
if [ ! -f "$WEB/dist/main.js" ]; then
  echo "Missing web/dist/main.js; run: cd recomp/web && npm ci && node build.mjs" >&2
  exit 1
fi
exec "$SERVER" --data "$DATA" --http-port "$PORT" --online off --roam-hub off
