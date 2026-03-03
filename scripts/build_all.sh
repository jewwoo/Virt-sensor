#!/usr/bin/env bash
set -euo pipefail

pushd kernel >/dev/null
make clean && make
popd >/dev/null

pushd daemon >/dev/null
make clean && make
popd >/dev/null

echo "Build complete."