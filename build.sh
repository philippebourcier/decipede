#!/usr/bin/env bash
# One-step build: fetch dependencies (first run only), configure, compile.
#   ./build.sh            -> build/rtkbase.uf2 (+ build/partitions.uf2)
#   JOBS=3 ./build.sh     limit parallel jobs (default: all CPUs)
# Configure your deployment first: cp provision/device.env.example provision/device.env
set -euo pipefail
cd "$(dirname "$0")"
JOBS=${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)}
export JOBS
tools/setup.sh
[ -f provision/device.env ] || echo "Note: no provision/device.env, so the firmware has no CONFIG_URL (see README, Provisioning)"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$JOBS"
echo
echo "Firmware:        build/rtkbase.uf2"
echo "Partition table: build/partitions.uf2 (first install only, see README)"
