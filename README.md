# htop-plus

A light Linux fork of **htop 3.4.1**, adding per-disk activity, filesystem space,
and per-GPU VRAM meters, with temperatures beside meter bars. No new runtime
packages are required beyond htop's existing curses dependency. NVIDIA support
uses the optional driver-provided NVML library, loaded at runtime; AMD uses sysfs.

## Build and install

Install a C compiler, make, autoconf, automake, pkg-config and ncurses development
headers. On Debian/Ubuntu:

```sh
sudo apt-get install build-essential autoconf automake pkg-config libncurses-dev
./autogen.sh
./configure --disable-sensors --disable-hwloc --disable-capabilities
make -j4
./htop-plus --version
sudo make install
```

On Arch, install `base-devel autoconf automake pkgconf ncurses` first.
The executable is `htop-plus`. Configuration is separate from stock htop, under
`$XDG_CONFIG_HOME/htop-plus/htoprc` (or `~/.config/htop-plus/htoprc`).
`HTOPRC` can select a different config file. The manual and desktop entry are installed as `htop-plus`, alongside stock htop;
run `htop-plus --help` for command options.

## Meters

Press **F2 → Meters**. Each disk and each mount has its own available entry.
GPU choices include each NVIDIA/AMD device. The default full-width layout adds
one VRAM row per GPU (none on GPU-less systems). A single GPU is captioned `VRM`;
multiple GPUs use `VRM1`, `VRM2`, etc., in device order. Add a meter
to the desired column and select its bar, text, graph or LED mode.

- **SSD**: busy percentage, read/write throughput, optional controller temperature.
  Includes NVMe, SATA and other block disks; the name is retained for config compatibility.
  Loop, RAM and compressed RAM devices are excluded. Rates use 512-byte kernel sectors.
- **Mount space**: whole percentage used and available space, following `df`'s
  treatment of blocks reserved for root. Device-backed mounts and ZFS datasets
  are listed; pseudo filesystems, squashfs, overlay and network mounts are omitted.
- **VRM**: compact used/total video memory for one GPU. The saved meter name remains
  `VRAM` for configuration compatibility. Legacy unparameterized VRAM
  selections expand into individual device rows when loaded.
  NVIDIA reads NVML; AMD reads `mem_info_vram_used` and `mem_info_vram_total`.
  Per-NVIDIA meters also show core temperature when the driver provides it.
  Unavailable device readings say **n/a**; sleeping devices say **asleep**.
- **Temperature column**: the imported CPU, RAM, disk and NVIDIA temperature
  readings show current, recent peak and reported limit when available. Sensor
  availability depends on hardware/kernel support; missing sensors leave it blank.
  Sysfs temperatures are sampled every five seconds; missing sensors are retried
  once a minute to keep idle overhead low.

A missing resource shows **n/a**, including machines without a GPU. Sleeping
GPUs stay asleep: if any NVIDIA GPU is suspended or has an unreadable runtime
power state, all NVML queries are deferred. Active AMD devices remain readable.
This deliberately conservative policy also prevents peer-device initialization.
See NVIDIA's API notes:
https://docs.nvidia.com/deploy/nvml-api/latest/api/group__nvmlDeviceQueries.html

Discovery is a sorted snapshot at startup. Restart after device/mount hotplug
and check saved selections when hardware changes; numeric selection indices can
change with the resource list. Mounted paths with spaces are decoded correctly.

## Environment variables

| Variable | Effect |
| --- | --- |
| `HTOP_PLUS_DISK` | Restrict SSD choices to one block name, e.g. `sda` or `nvme2n1`. No path prefix. |
| `HTOP_PLUS_MOUNT` | Restrict space choices to one absolute mount path. |
| `HTOP_PLUS_SYSROOT` | Fake filesystem root **only in test builds** compiled with `HTOP_PLUS_TESTING`. Ignored by release builds. |

No disk-guard integration, download control or fixed personal mount paths are included.

## Static candidate

`scripts/build-static.sh /absolute/new-build-dir` builds an x86_64 musl candidate
inside Alpine (see the CI workflow for development packages). It embeds ncurses
and fallback terminal descriptions for xterm, xterm-256color, Linux console,
screen, screen-256color, tmux, tmux-256color and vt100. The output directory holds
`htop-plus-linux-x86_64-musl`, `SHA256SUMS` and both license notices. Verify with
`sha256sum -c SHA256SUMS`, then install the executable as `htop-plus`.

**NVIDIA requires the normal source build.** A fully static musl executable cannot
load NVML with `dlopen`; its NVIDIA meters therefore show n/a. AMD, disk and
sysfs temperature meters remain supported. This limitation avoids adding a helper
process or runtime dependency. Release builds include both glibc and static musl
binaries. The glibc binary is built on Debian trixie; older distributions may need
to build from source. Publishing release assets and tags requires the owner's gate.

## Staying up to date

The daily (or manual) **Sync upstream** action merges the newest stable htop tag
on `sync/htop-X.Y.Z` and opens a PR with squash auto-merge after required CI.
Conflicts open an issue listing files and leave `main` unchanged.

The **Draft release** action builds and tests glibc (with optional NVIDIA NVML)
and static musl binaries, then prepares `vX.Y.Z-plus.N` with source, licenses and
checksums. The owner validates the candidates, creates/pushes the release tag at
the draft's source commit, and publishes the draft. No workflow publishes or
pushes release tags. Only `GITHUB_TOKEN` is needed.

Before enabling the schedule, follow the [owner setup and workflow notes](.htop-plus/README.md),
including making **CI** a required status check for `main`.

## Tests and CI

```sh
tests/ci.sh /absolute/new-test-dir
```

Build outputs and synthetic proc/sys trees stay in that directory. The suite
covers 0/1/3 disks, NVMe/SATA, mount filtering, overrides, malformed/missing files,
0/1/2 NVIDIA devices via a tiny NVML stub, AMD, mixed GPUs, suspended peers,
optional driver symbols, meter independence, counter resets, GPU totals and config
round-trips. Three terminal sizes exercise F2 and two seconds of live drawing.

CI builds Ubuntu, Debian and Arch containers with GCC and Clang, plus separate
ASan/UBSan, Valgrind and static candidate jobs. All sanitizer and Valgrind checks run without leak suppressions.
The CI workflow builds artifacts; the separate release workflow creates drafts.
No workflow publishes releases or pushes release tags.

## License and credit

Based on htop 3.4.1 by Hisham H. Muhammad and the htop development team:
https://htop.dev/

htop and these additions are **GPL version 2 or later**, preserving the upstream
notices. See `COPYING` and `AUTHORS`. Static distributions also include ncurses'
permissive license as `COPYING.ncurses`; its source is pinned by checksum in the
static build script. There is no association with or endorsement by upstream htop.
