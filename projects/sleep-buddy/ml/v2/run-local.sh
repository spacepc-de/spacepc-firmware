#!/usr/bin/env bash
set -euo pipefail

SCRIPT_ROOT="$(cd "$(dirname "$0")" && pwd)"
DATA_ROOT="${SLEEP_AI_DATA_ROOT:-$SCRIPT_ROOT/data}"

docker run --rm \
  -v "$SCRIPT_ROOT:/pipeline:ro" \
  -v "$DATA_ROOT:/sleepai:rw" \
  -w /pipeline \
  sleepai-ml:v2 "$@"
