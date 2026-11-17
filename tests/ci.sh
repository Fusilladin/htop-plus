#!/bin/sh
set -eu
src=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out=${1:?Pass a new output directory}
mkdir -p "$out/source" "$out/build"
out=$(CDPATH= cd -- "$out" && pwd)
cp -a "$src/." "$out/source/"
# Keep configuration writes inside this run, including container and valgrind runs.
export HOME="$out/home" XDG_CONFIG_HOME="$out/home/.config" HTOPRC="$out/home/htoprc"
mkdir -p "$XDG_CONFIG_HOME"
cd "$out/source"
./autogen.sh
cd "$out/build"
"$out/source/configure" --disable-sensors --disable-hwloc --disable-capabilities CFLAGS="${CFLAGS:--Os -g}"
make -j"${JOBS:-4}"
./htop-plus --version
"$src/tests/run.sh" "$out/tests"
"$src/tests/meters.sh" "$out/build" "$out/tests"
python3 "$src/tests/pty_smoke.py" ./htop-plus "$out/pty"
