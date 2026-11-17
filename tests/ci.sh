#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out=${1:?Pass a new output directory}
mkdir -p "$out/source" "$out/build"
out=$(CDPATH= cd -- "$out" && pwd)
cp -a "$src/." "$out/source/"
cd "$out/source"
./autogen.sh
cd "$out/build"
"$out/source/configure" --disable-sensors --disable-hwloc --disable-capabilities CFLAGS="${CFLAGS:--Os -g}"
make -j"${JOBS:-4}"
./htop-plus --version
"$src/tests/run.sh" "$out/tests"
"$src/tests/meters.sh" "$out/build" "$out/tests"
python3 "$src/tests/pty_smoke.py" ./htop-plus "$out/pty"
