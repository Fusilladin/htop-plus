#!/usr/bin/env python3
"""Prepare an upstream sync locally; --github also pushes and opens its PR."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess

UPSTREAM = "https://github.com/htop-dev/htop.git"
VERSION = re.compile(r"(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)")
IDENTITY = "Fusilladin"
EMAIL = "91526142+Fusilladin@users.noreply.github.com"


def run(*args, check=True):
    return subprocess.run(args, text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, check=check)


def git(*args, check=True):
    return run("git", *args, check=check)


def gh(*args):
    return run("gh", *args).stdout.strip()


def key(version):
    return tuple(map(int, version.split(".")))


def prepare(upstream):
    if git("status", "--porcelain").stdout:
        raise RuntimeError("Sync requires a clean checkout")
    current = Path(".htop-plus/upstream-version").read_text().strip()
    if not VERSION.fullmatch(current):
        raise RuntimeError("Invalid upstream-version marker")
    # Keep upstream tags in a private ref namespace: release tags remain owner-only.
    git("fetch", "--no-tags", upstream, "refs/tags/*:refs/htop-upstream/*")
    tags = git("for-each-ref", "--format=%(refname:strip=2)", "refs/htop-upstream/").stdout.splitlines()
    stable = [tag for tag in tags if VERSION.fullmatch(tag)]
    if current not in stable:
        raise RuntimeError("Recorded upstream version is missing from upstream")
    latest = max(stable, key=key)
    result = {"version": latest, "previous": current, "status": "current"}
    if key(latest) <= key(current):
        return result
    branch = "sync/htop-" + latest
    result["branch"] = branch
    remote = git("ls-remote", "--heads", "origin", "refs/heads/" + branch).stdout.strip()
    if remote:
        git("fetch", "--no-tags", "origin", "refs/heads/" + branch + ":refs/htop-sync/candidate")
        marker = git("show", "refs/htop-sync/candidate:.htop-plus/upstream-version").stdout.strip()
        if marker != latest:
            raise RuntimeError("Existing sync branch has the wrong version; owner must inspect it")
        result["status"] = "existing"
        result["head"] = remote.split()[0]
        return result
    git("switch", "-c", branch)
    old_ref = "refs/htop-upstream/" + current
    new_ref = "refs/htop-upstream/" + latest
    # The initial import and subsequent squash merges lack upstream parents.
    # Preserve our entire tree while attaching the recorded upstream base.
    git("merge", "--strategy=ours", "--no-ff", "--allow-unrelated-histories",
        old_ref, "-m", "Record upstream htop " + current + " merge base")
    merge = git("merge", "--no-commit", "--no-ff", new_ref, check=False)
    if merge.returncode:
        conflicts = git("diff", "--name-only", "--diff-filter=U").stdout.splitlines()
        if not conflicts:
            raise RuntimeError(merge.stderr or merge.stdout)
        git("merge", "--abort")
        result.update(status="conflict", conflicts=conflicts)
        return result
    Path(".htop-plus/upstream-version").write_text(latest + "\n")
    git("add", ".htop-plus/upstream-version")
    git("commit", "-m", "Sync upstream htop " + latest)
    result.update(status="prepared", head=git("rev-parse", "HEAD").stdout.strip())
    return result


def publish(result):
    version = result["version"]
    if result["status"] == "conflict":
        title = "Upstream htop " + version + " merge conflicts"
        issues = json.loads(gh("issue", "list", "--state", "open", "--json", "title,url", "--limit", "100"))
        existing = next((issue for issue in issues if issue["title"] == title), None)
        if existing:
            result["issue"] = existing["url"]
        else:
            body = "Automatic sync stopped without pushing a branch. Resolve these files manually:\n\n"
            body += "\n".join("- `" + file + "`" for file in result["conflicts"])
            result["issue"] = gh("issue", "create", "--title", title, "--body", body)
        return
    if result["status"] == "current":
        return
    branch = result["branch"]
    if result["status"] == "prepared":
        git("push", "origin", "HEAD:refs/heads/" + branch)
    prs = json.loads(gh("pr", "list", "--head", branch, "--base", "main", "--state", "open",
                        "--json", "number,url,id,headRefOid", "--limit", "100"))
    if not prs:
        body = ("Merge upstream htop " + version + ". The recorded upstream base is " + result["previous"]
                + ".\n\nCI is explicitly dispatched with GITHUB_TOKEN. Squash auto-merge obeys the required CI check."
                + "\n\nAfter merging, the sync workflow dispatches draft release builds. The owner publishes releases.")
        gh("pr", "create", "--base", "main", "--head", branch,
           "--title", "Sync upstream htop " + version, "--body", body)
        prs = json.loads(gh("pr", "list", "--head", branch, "--base", "main", "--state", "open",
                            "--json", "number,url,id,headRefOid", "--limit", "100"))
    pr = prs[0]
    if pr["headRefOid"] != result["head"]:
        raise RuntimeError("Sync PR head changed; refusing to arm auto-merge")
    result.update(pr=str(pr["number"]), url=pr["url"])
    gh("workflow", "run", "ci.yml", "--ref", branch)
    # Queue auto-merge directly; do not use a command that can immediately merge
    # an unprotected branch. GitHub requires branch merge requirements here.
    gh("api", "graphql", "-f", "query=mutation($id:ID!){enablePullRequestAutoMerge(input:{pullRequestId:$id,mergeMethod:SQUASH}){pullRequest{id}}}",
       "-f", "id=" + pr["id"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--upstream", default=UPSTREAM, help="Local mirror allowed for dry runs")
    parser.add_argument("--github", action="store_true", help="Push and open a PR using GITHUB_TOKEN")
    args = parser.parse_args()
    if args.github and args.upstream != UPSTREAM:
        parser.error("GitHub mode requires the official upstream")
    for role in ("AUTHOR", "COMMITTER"):
        os.environ["GIT_" + role + "_NAME"] = IDENTITY
        os.environ["GIT_" + role + "_EMAIL"] = EMAIL
    result = prepare(args.upstream)
    if args.github:
        publish(result)
    print(json.dumps(result, indent=2))
    if os.environ.get("GITHUB_OUTPUT"):
        with open(os.environ["GITHUB_OUTPUT"], "a") as output:
            for name in ("status", "version", "branch", "head", "pr"):
                if name in result:
                    output.write(name + "=" + result[name] + "\n")


if __name__ == "__main__":
    main()
