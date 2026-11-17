#!/bin/sh
set -eu
# Run under x86_64 Alpine with build-base, linux-headers, autotools, ncurses-dev,
# ncurses and ncurses-static (tic/infocmp), curl and Python 3. Never publishes anything.
command -v tic >/dev/null
command -v infocmp >/dev/null
src=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out=${1:?Pass a new output directory}
[ "$(uname -m)" = x86_64 ] || { echo 'x86_64 builder required' >&2; exit 1; }
mkdir -p "$out/ncurses-source" "$out/ncurses-build" "$out/source" "$out/build" "$out/artifacts"
out=$(CDPATH= cd -- "$out" && pwd)
curl -fL --retry 3 https://ftp.gnu.org/gnu/ncurses/ncurses-6.5.tar.gz -o "$out/ncurses-6.5.tar.gz"
printf '%s  %s\n' 136d91bc269a9a5785e5f9e980bc76ab57428f604ce3e5a5a90cebc767971cc6 "$out/ncurses-6.5.tar.gz" | sha256sum -c -
tar -xf "$out/ncurses-6.5.tar.gz" --strip-components=1 -C "$out/ncurses-source"
cd "$out/ncurses-build"
"$out/ncurses-source/configure" --prefix="$out/ncurses" --without-shared --without-debug --without-ada --without-cxx --without-cxx-binding --without-tests --without-progs --enable-widec --with-normal --without-termlib --with-fallbacks=xterm,xterm-256color,linux,screen,screen-256color,tmux,tmux-256color,vt100 CFLAGS='-Os -ffunction-sections -fdata-sections'
make -j"${JOBS:-4}"
make install.libs install.includes
cp -a "$src/." "$out/source/"
cd "$out/source"
./autogen.sh
cd "$out/build"
# musl static executables cannot load NVML. Sysfs AMD, disk and thermal meters
# remain available. The README makes this limitation explicit.
CURSES_CFLAGS="-I$out/ncurses/include -I$out/ncurses/include/ncursesw" CURSES_LIBS="$out/ncurses/lib/libncursesw.a" \
  "$out/source/configure" --disable-sensors --disable-hwloc --disable-capabilities CFLAGS='-Os -ffunction-sections -fdata-sections' LDFLAGS='-static -Wl,--gc-sections'
make -j"${JOBS:-4}"
strip htop-plus
cp htop-plus "$out/artifacts/htop-plus-linux-x86_64-musl"
cp "$src/COPYING" "$out/artifacts/COPYING"
cp "$out/ncurses-source/COPYING" "$out/artifacts/COPYING.ncurses"
size=$(wc -c < htop-plus)
[ "$size" -lt 1500000 ] || { echo "Size budget exceeded: $size" >&2; exit 1; }
! readelf -l htop-plus | grep -q INTERP
! readelf -d htop-plus | grep -q NEEDED
./htop-plus --version
# Force fallback terminfo use in a tiny isolated chroot with no shared libraries.
mkdir -p "$out/empty-root/bin"
cp htop-plus "$out/empty-root/bin/htop-plus"
chroot "$out/empty-root" /bin/htop-plus --version
SMOKE_TERM=xterm-256color python3 "$src/tests/pty_smoke.py" ./htop-plus "$out/pty"
cd "$out/artifacts"
sha256sum htop-plus-linux-x86_64-musl > SHA256SUMS
printf 'Static binary: %s bytes; no ELF interpreter or shared dependencies\n' "$size"
