#!/bin/sh
set -eu
# Use objects from a configured htop-plus build, replacing its backend with a fixture build.
src=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=${1:?configured build}; tests=${2:?test output from run.sh}
# These integration binaries also run directly, outside ci.sh.
mkdir -p "$tests/home/.config"
tests=$(CDPATH= cd -- "$tests" && pwd)
export HOME="$tests/home" XDG_CONFIG_HOME="$tests/home/.config" HTOPRC="$tests/home/htoprc"
cc=${CC:-cc}
mkdir -p "$tests/fixtures/test_mixed/sys/block/sda" "$tests/fixtures/test_mixed/sys/block/sdb"
sensor="$tests/fixtures/test_mixed/sys/block/sda/device/hwmon0"
mkdir -p "$sensor"
printf 'nvme\n' > "$sensor/name"
printf 'Composite\n' > "$sensor/temp1_label"
printf '31000\n' > "$sensor/temp1_input"
printf '100000\n' > "$sensor/temp1_crit"
$cc ${TEST_CFLAGS:--O2 -g} -D_GNU_SOURCE -DHTOP_PLUS_TESTING -I"$build" -I"$src" \
  -c "$src/linux/PlusDevices.c" -o "$tests/fixture-backend.o"
objects=$(find "$build" -name '*.o' ! -name 'htop.o' ! -name 'PlusDevices.o')
$cc ${TEST_CFLAGS:--O2 -g} -UNDEBUG -I"$build" -I"$src" -I"$src/linux" \
  "$src/tests/meters.c" "$tests/fixture-backend.o" $objects -lncursesw -ltinfo -lm -ldl -o "$tests/meters"
HTOP_PLUS_SYSROOT="$tests/fixtures/test_mixed" NVML_TEST_COUNT=2 LD_LIBRARY_PATH="$tests/stub" "$tests/meters"

$cc ${TEST_CFLAGS:--O2 -g} -UNDEBUG -I"$build" -I"$src" -I"$src/linux" \
  "$src/tests/layout.c" "$tests/fixture-backend.o" $objects -lncursesw -ltinfo -lm -ldl -o "$tests/layout"
for scenario in zero_devices one_nvidia two_nvidia amd mixed; do
  case "$scenario" in
    zero_devices) count=0; nv=0;;
    one_nvidia) count=1; nv=1;;
    two_nvidia) count=2; nv=2;;
    amd) count=1; nv=0;;
    mixed) count=3; nv=2;;
  esac
  HTOPRC="$tests/layout-$scenario.rc" HTOP_PLUS_SYSROOT="$tests/fixtures/test_$scenario" \
    NVML_TEST_COUNT="$nv" LD_LIBRARY_PATH="$tests/stub" TERM=xterm-256color \
    "$tests/layout" "$count" "$tests/layout-$scenario.terminal"
done
