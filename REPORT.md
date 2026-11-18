# DSK format and stable mount selection

Completed on 2026-11-18 for Fusilladin/htop-plus, based on commit
44210d2b0ea96742e578f0d7ba0fb0372e9a550a. Local branch:
`fix/dsk-format-stable-mounts`. Author and committer:
`Fusilladin <91526142+Fusilladin@users.noreply.github.com>`.

The source and commit are in:
`~/dev/projects/plans/86-htop-plus-public/_build/dsk-format-20261118/repo`.
The requested original checkout is read-only in this sandbox, so a local clone
was used. The original checkout, installed binary, live watcher state and remote
repository were not modified. Nothing was pushed or installed.

## Changes

- DSK now uses the old local build's `Meter_humanUnit` format:
  `<used>/<size> <pct>% <avail> free`, with one decimal in the percentage.
  Example: `576G/1.00T 60.0% 384G free`.
- Sampling retains df-style accounting: used = blocks - all free blocks;
  available excludes root-reserved blocks; percent = used / (used + available).
  The percentage is highlighted at or above **80%**, including bar and text modes.
  The existing bar renderer may draw fill marks in spaces within its text.
- Each DSK instance retains its selected absolute mount path independently.
  htoprc entries now look like `DiskSpace(/home)` rather than `DiskSpace(4)`.
  Space, percent, parentheses, equals and other special bytes are percent-escaped.
- Valid old numeric and unparameterized selections still load and become path
  selections when the meter layout is saved. SSD/GPU serialization is unchanged.
- A saved mount absent from startup discovery remains selected and shows `n/a`,
  even if its mount-point directory still exists. Its path survives another save.
- `HTOP_PLUS_MOUNT` remains a single absolute-path override: all DSK rows use that
  path for the run, while previously saved choices remain intact.
- No disk-guard state file or `DL paused` integration was added.
- README explains format, selection, compatibility and restart behavior. The new
  test helper is included in Makefile.am's source-distribution list.

## Pick partitions in F2

1. Run htop-plus without `HTOP_PLUS_MOUNT` to expose all discovered partitions:
   `env -u HTOP_PLUS_MOUNT ~/.local/bin/htop-plus`.
2. Press **F2**, select **Meters**, then move to **Available meters**.
3. Select **Mount space used/free: /**, **/home**, **/data**, **/models**, or
   **/boot/efi**. Add each wanted entry using **Enter** (last column), or
   **F5** (first column) / **F6** (last column), as shown by the picker.
4. Move the added row where desired; repeat for the remaining mounts. Remove old
   unwanted DSK rows with **Delete** in the selected meter column. Use **Space**
   to switch a selected meter's mode if desired.
5. Leave Setup with **F10** or **Esc**, then quit normally with **q/F10** to save.

Default configuration: `$XDG_CONFIG_HOME/htop-plus/htoprc`, otherwise
`~/.config/htop-plus/htoprc`. `HTOPRC` can override the filename.
A saved line can contain, for example:

```text
column_meters_0=DiskSpace(/) DiskSpace(/home) DiskSpace(/data) DiskSpace(/models) DiskSpace(/boot/efi)
column_meter_modes_0=1 1 1 1 1
```

## Validation

PASS: `scripts/build-glibc.sh /work/out` in the existing Debian trixie Docker
image `htop-plus-ci-fix-debian:20261117`. The script runs `tests/ci.sh /work/out`
before producing and stripping the binary. The final artifact is rebuilt from
the committed source; the committed version is visible with `--version`.

PASS: an additional full `tests/ci.sh /work/sanitize` with Clang, ASan, UBSan and
leak detection enabled, without suppressions.

Coverage includes:
- 17 device-discovery/backend tests and optional/absent NVML cases.
- Real meter callbacks with deterministic reserved-block samples; exact DSK text,
  80% boundary, 100% with zero available, invalid samples and highlight reset.
- Actual htoprc write/read for all five owner mount paths and a path containing
  space, parentheses, percent and equals; adding/removing earlier mounts;
  missing-but-existing mount directory; overrides; legacy migration; malformed
  path escapes; independent modes and unchanged SSD/GPU settings.
- Five GPU scenarios and ncurses rendering in bar/text/graph/LED modes,
  ASCII/UTF-8 and full/half columns, including percent-only highlighting.
- PTY smoke tests at 24x80, 12x40 and 40x160, entering F2 Meters and exiting cleanly.

Evidence beside this report: `build-glibc.log`, `sanitize.log`, `out/tests/`,
`out/pty/`, `sanitize/`. The first rendering assertion assumed spaces could never
contain bar fill; the test was corrected to match the existing renderer. Raw
first-attempt logs and core were preserved under `_archive/20261118-first-build/`.

## Binary and exact install command (not executed)

Binary:
`~/dev/projects/plans/86-htop-plus-public/_build/dsk-format-20261118/out/artifacts/htop-plus-linux-x86_64-glibc`.
This is an x86_64 dynamically linked glibc build, with optional NVML dlopen
support. It requires compatible glibc/ncurses libraries; Debian trixie uses
glibc 2.41, so older distributions may need a source build. The license and exact
binary checksum are in `out/artifacts/COPYING` and `out/artifacts/SHA256SUMS`.

Run these commands only when ready to install; the current binary is moved into
`~/.local/bin/_archive` before installing the candidate:

```sh
set -eu
artifacts="$HOME/dev/projects/plans/86-htop-plus-public/_build/dsk-format-20261118/out/artifacts"
(cd "$artifacts" && sha256sum -c SHA256SUMS)
archive_dir="$HOME/.local/bin/_archive/$(date +%Y%m%d-%H%M%S)-dsk-format"
mkdir -p "$archive_dir"
if [ -e "$HOME/.local/bin/htop-plus" ] || [ -L "$HOME/.local/bin/htop-plus" ]; then
    mv "$HOME/.local/bin/htop-plus" "$archive_dir/htop-plus"
fi
install -m 755 "$artifacts/htop-plus-linux-x86_64-glibc" "$HOME/.local/bin/htop-plus"
"$HOME/.local/bin/htop-plus" --version
```

## Assumptions and limits

- Followed the headless instruction: made routine decisions without questions.
- Used a writable clone based on the exact requested commit; no permissions
  escalation or live watcher takeover was attempted from this scoped job.
- The old local limit means 80% of used + available, using the unrounded value.
  Download-pausing text was intentionally omitted, as permitted by the request.
- Mount PATH is the identity, rather than a filesystem UUID/device. A different
  filesystem mounted at the same path is consequently the selected partition.
- Discovery remains a startup snapshot. Restart after mounting/unmounting;
  this change does not add hotplug polling. Legacy indices can only resolve
  against the mount list available when loaded; historical index identity is
  unrecoverable until a path selection has been saved.
- An active HTOP_PLUS_MOUNT temporarily overrides every saved DSK row, preserving
  the public build's single-mount restriction and allowing normal choices after
  the override is removed. No additional numbered environment variables exist.
- Kept the existing caption `DSK`; the F2 row label identifies the selected path.
- Used the already available Debian Docker toolchain with four CPUs. Build
  products and failed attempts were preserved/archived; no manual deletion.
