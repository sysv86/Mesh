#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
MESHC="$ROOT/build/meshc"

if [[ ! -x "$MESHC" ]]; then
  echo "error: $MESHC not found; run 'make -j\$(nproc)' first" >&2
  exit 1
fi

shopt -s nullglob
examples=(
  "$ROOT"/skills/mesh/examples/*.mesh
  "$ROOT"/skills/mesh-typography/examples/*.mesh
  "$ROOT"/skills/mesh-motion/examples/*.mesh
)

if [[ ${#examples[@]} -eq 0 ]]; then
  echo "error: no skill examples found" >&2
  exit 1
fi

for file in "${examples[@]}"; do
  echo "validate ${file#$ROOT/}"
  "$MESHC" validate "$file"
done

echo "All ${#examples[@]} skill examples validate."
