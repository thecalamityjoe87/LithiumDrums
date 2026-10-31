#!/usr/bin/env bash
# Usage: scripts/build.sh [debug|release] [extra cmake --build args...]
set -euo pipefail

preset="${1:-debug}"
shift || true

cd "$(dirname "$0")/.."
cmake --preset "$preset"
cmake --build --preset "$preset" "$@"

echo
echo "Built into build/$preset/LithiumDrums_artefacts/"
