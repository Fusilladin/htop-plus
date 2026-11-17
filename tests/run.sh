#!/bin/sh
set -eu
# Usage: tests/run.sh /absolute/unique-output-directory
src=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out=${1:?Pass a new output directory}
mkdir -p "$out/stub" "$out/no-temp" "$out/missing-library"
out=$(CDPATH= cd -- "$out" && pwd)
printf '' > "$out/config.h"
cc=${CC:-cc}
flags=${TEST_CFLAGS:--O2 -g}
$cc $flags -Wall -Wextra -Werror -D_GNU_SOURCE -DHTOP_PLUS_TESTING -I"$out" -I"$src" \
   "$src/linux/PlusDevices.c" "$src/tests/probe.c" -ldl -lm -o "$out/probe"
$cc $flags -I"$src" -fPIC -shared "$src/tests/nvml-stub.c" -o "$out/stub/libnvidia-ml.so.1"
$cc $flags -I"$src" -fPIC -shared -DNVML_TEST_NO_TEMPERATURE "$src/tests/nvml-stub.c" -o "$out/no-temp/libnvidia-ml.so.1"
python3 "$src/tests/test_devices.py" "$out/probe" "$out/stub" "$out/fixtures"
# The same NVIDIA fixture must tolerate missing optional symbols and missing libraries.
HTOP_PLUS_SYSROOT="$out/fixtures/test_one_nvidia" NVML_TEST_COUNT=1 LD_LIBRARY_PATH="$out/no-temp" \
   "$out/probe" > "$out/no-temperature.txt"
HTOP_PLUS_SYSROOT="$out/fixtures/test_one_nvidia" LD_LIBRARY_PATH="$out/missing-library" \
   "$out/probe" > "$out/no-library.txt"
# Prove production ignores the fixture-root switch (using a deliberately nonexistent root).
$cc $flags -D_GNU_SOURCE -I"$out" -I"$src" "$src/linux/PlusDevices.c" "$src/tests/probe.c" -ldl -lm -o "$out/production-probe"
HTOP_PLUS_SYSROOT="$out/nonexistent" "$out/production-probe" > "$out/production.txt"
python3 - "$out" <<'PYTHON'
from pathlib import Path
import sys
out = Path(sys.argv[1])
assert out.joinpath('no-temperature.txt').read_text().strip() == 'gpu NVIDIA 0000:01:00.0 1 2048 8192 0'
assert out.joinpath('no-library.txt').read_text().strip() == 'gpu NVIDIA 0000:01:00.0 0 0 0 0'
PYTHON
"$out/production-probe" > "$out/production-unset.txt"
cmp "$out/production.txt" "$out/production-unset.txt"
echo 'Device tests, optional symbols, absent library, production root isolation: PASS'
