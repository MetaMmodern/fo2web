#!/usr/bin/env bash
# Read-only report for the frozen FlatOut 2 executable.
set -euo pipefail

game_dir="${FO2_GAME_DIR:?Set FO2_GAME_DIR to the FlatOut 2 installation directory}"
exe="$game_dir/FlatOut2.exe"
[[ -f "$exe" ]] || { echo "Not found: $exe" >&2; exit 1; }
command -v objdump >/dev/null || { echo "objdump is required" >&2; exit 1; }

out_dir="build/diagnostics"
mkdir -p "$out_dir"

{
  echo '# FlatOut 2 binary preflight'
  echo
  echo '## Identity'
  echo
  echo '```text'
  file "$exe"
  shasum -a 256 "$exe"
  echo '```'
  echo
  echo '## PE image'
  echo
  echo '```text'
  objdump -p "$exe" | sed -n '1,/The Import Tables:/p'
  echo '```'
  echo
  echo '## Imported DLLs'
  echo
  echo '```text'
  objdump -p "$exe" | sed -n '/The Import Tables:/,/The Export Tables:/p'
  echo '```'
  echo
  echo '## Sections'
  echo
  echo '```text'
  objdump -x "$exe" | sed -n '/Sections:/,/SYMBOL TABLE:/p'
  echo '```'
} > "$out_dir/preflight.md"

echo "Wrote $out_dir/preflight.md"
