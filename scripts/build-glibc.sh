#!/bin/sh
set -eu
# Run on Linux x86_64 with the development packages used by tests/ci.sh.
src=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out=${1:?Pass a new output directory}
export LC_ALL=C.UTF-8
[ "$(uname -m)" = x86_64 ] || { echo 'x86_64 builder required' >&2; exit 1; }
"$src/tests/ci.sh" "$out"
mkdir -p "$out/artifacts"
cp "$out/build/htop-plus" "$out/artifacts/htop-plus-linux-x86_64-glibc"
strip "$out/artifacts/htop-plus-linux-x86_64-glibc"
cp "$src/COPYING" "$out/artifacts/COPYING"
# Confirm this is a dynamic glibc binary. NVML remains optional at runtime;
# tests/ci.sh exercises its dlopen backend using the driver-library stub.
readelf -l "$out/artifacts/htop-plus-linux-x86_64-glibc" | grep 'ld-linux-x86-64.so.2'
cd "$out/artifacts"
sha256sum htop-plus-linux-x86_64-glibc > SHA256SUMS
