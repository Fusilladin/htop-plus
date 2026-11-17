#!/usr/bin/env python3
"""Create/resume a draft for this exact source commit; never publish or push tags."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def gh(*args, payload=None):
    result = subprocess.run(["gh", *args], input=json.dumps(payload) if payload is not None else None,
                            text=True, stdout=subprocess.PIPE, check=True)
    return result.stdout.strip()


def select_release(releases, tags, version, sha):
    pattern = re.compile(r"v" + re.escape(version) + r"-plus\.([1-9]\d*)")
    for release in releases:
        if pattern.fullmatch(release["tag_name"]) and release["target_commitish"] == sha:
            return release
    numbers = [int(match[1]) for name in tags + [r["tag_name"] for r in releases]
               if (match := pattern.fullmatch(name))]
    return {"tag_name": "v" + version + "-plus." + str(max(numbers, default=0) + 1)}


def verify_assets(directory):
    expected = {"htop-plus-linux-x86_64-glibc", "htop-plus-linux-x86_64-musl",
                "htop-plus-source.tar.gz", "COPYING", "COPYING.ncurses"}
    actual = {path.name for path in directory.iterdir()}
    if actual != expected | {"SHA256SUMS"}:
        raise RuntimeError("Release assets do not match the expected set")
    checksums = {}
    for line in (directory / "SHA256SUMS").read_text().splitlines():
        digest, name = line.split("  ", 1)
        if name not in expected or name in checksums:
            raise RuntimeError("Unexpected or duplicate checksum entry")
        if hashlib.sha256((directory / name).read_bytes()).hexdigest() != digest:
            raise RuntimeError("Checksum mismatch: " + name)
        checksums[name] = digest
    if set(checksums) != expected:
        raise RuntimeError("Missing checksum entries")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("assets", type=Path)
    args = parser.parse_args()
    verify_assets(args.assets)
    version = Path(".htop-plus/upstream-version").read_text().strip()
    if not re.fullmatch(r"(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)", version):
        raise RuntimeError("Invalid upstream version")
    sha = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
    repo = os.environ["GITHUB_REPOSITORY"]
    releases = sum(json.loads(gh("api", "--paginate", "--slurp", "repos/" + repo + "/releases?per_page=100")), [])
    tags = [tag["name"] for page in json.loads(gh("api", "--paginate", "--slurp", "repos/" + repo + "/tags?per_page=100")) for tag in page]
    release = select_release(releases, tags, version, sha)
    tag = release["tag_name"]
    if "id" in release and not release["draft"]:
        print("This source already has a published release: " + tag)
        return
    if "id" not in release:
        notes = ("Source commit: `" + sha + "`\n\n"
                 "Linux x86_64 glibc binary: optional NVIDIA NVML loaded at runtime, plus AMD/sysfs meters. "
                 "The glibc build uses Debian trixie; older distributions should build from source.\n\n"
                 "Static musl binary: AMD, disk and temperature meters; NVIDIA NVML is unavailable in static musl.\n\n"
                 "Verify SHA256SUMS. GPL-2.0-or-later source and ncurses license are included.\n\n"
                 "Owner: validate the binaries, create/push `" + tag + "` at the source commit, "
                 "then publish this draft. Automation never publishes or pushes a tag.")
        release = json.loads(gh("api", "repos/" + repo + "/releases", "--method", "POST", "--input", "-",
                                payload={"tag_name": tag, "target_commitish": sha, "name": tag,
                                         "body": notes, "draft": True, "prerelease": False}))
    if not release["draft"] or release["target_commitish"] != sha:
        raise RuntimeError("Release is not a draft for the tested source")
    # Missing assets can be resumed, but never replace an owner's existing asset.
    existing = {asset["name"]: asset for asset in release.get("assets", [])}
    for path in sorted(args.assets.iterdir()):
        if path.name in existing:
            digest = "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()
            if existing[path.name].get("digest") != digest:
                raise RuntimeError("Existing draft asset differs: " + path.name)
            continue
        gh("release", "upload", tag, str(path))
    print("Draft ready for owner review: " + tag + " (" + sha + ")")


if __name__ == "__main__":
    main()
