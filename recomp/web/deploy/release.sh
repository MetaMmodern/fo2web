#!/usr/bin/env bash
# Explicit FlatOut release: see FLATOUT_COOLIFY.md. Does not build the WASM.
set -euo pipefail
exec python3 "$(dirname "$0")/release_flatout.py" "$@"
