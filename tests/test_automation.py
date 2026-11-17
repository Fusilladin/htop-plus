"""Offline integration tests for sync ancestry, conflicts and release safety."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import textwrap
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


sync = load("sync_upstream", "sync-upstream.py")
draft = load("draft_release", "draft-release.py")


class SyncTests(unittest.TestCase):
    def setUp(self):
        self.original = Path.cwd()
        # Retain each fixture for diagnosis; no cleanup removes evidence.
        self.temp = Path(tempfile.mkdtemp(prefix="htop-plus-automation-"))
        self.upstream = self.temp / "upstream"
        self.fork = self.temp / "fork"
        self.identity = patch.dict(os.environ, {
            "GIT_AUTHOR_NAME": sync.IDENTITY, "GIT_AUTHOR_EMAIL": sync.EMAIL,
            "GIT_COMMITTER_NAME": sync.IDENTITY, "GIT_COMMITTER_EMAIL": sync.EMAIL,
            "GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_SYSTEM": os.devnull,
        })
        self.identity.start()
        self.init(self.upstream)
        (self.upstream / "shared.c").write_text("upstream base\n")
        self.commit("base")
        sync.git("update-ref", "refs/tags/3.4.1", "HEAD")
        self.init(self.fork)
        (self.fork / ".htop-plus").mkdir()
        (self.fork / ".htop-plus/upstream-version").write_text("3.4.1\n")
        (self.fork / "shared.c").write_text("upstream base\n")
        (self.fork / "meters.c").write_text("fork meters\n")
        self.commit("independent import")
        sync.git("remote", "add", "origin", str(self.upstream))

    def tearDown(self):
        os.chdir(self.original)
        self.identity.stop()

    def init(self, path):
        path.mkdir()
        os.chdir(path)
        sync.git("init", "-b", "main")

    def commit(self, message):
        sync.git("add", ".")
        sync.git("commit", "-m", message)

    def advance(self, version, shared="upstream base\n"):
        os.chdir(self.upstream)
        (self.upstream / "shared.c").write_text(shared)
        (self.upstream / ("release-" + version)).write_text(version)
        self.commit(version)
        sync.git("update-ref", "refs/tags/" + version, "HEAD")
        os.chdir(self.fork)

    def test_current_is_noop(self):
        before = sync.git("rev-parse", "HEAD").stdout
        self.assertEqual(sync.prepare(str(self.upstream))["status"], "current")
        self.assertEqual(sync.git("rev-parse", "HEAD").stdout, before)

    def test_stable_numeric_selection_and_independent_import(self):
        self.advance("3.9.0")
        self.advance("3.10.0")
        self.advance("4.0.0rc1")
        result = sync.prepare(str(self.upstream))
        self.assertEqual((result["status"], result["version"]), ("prepared", "3.10.0"))
        self.assertEqual(Path("meters.c").read_text(), "fork meters\n")
        self.assertEqual(Path(".htop-plus/upstream-version").read_text(), "3.10.0\n")
        self.assertEqual(sync.git("tag").stdout, "")
        self.assertEqual(sync.git("status", "--porcelain").stdout, "")
        # The default dry run must never push a branch.
        self.assertEqual(sync.git("ls-remote", "origin", "refs/heads/sync/*").stdout, "")

    def test_repeated_sync_after_squash_keeps_correct_base(self):
        self.advance("3.5.0", "new upstream line\n")
        sync.prepare(str(self.upstream))
        sync.git("switch", "main")
        sync.git("merge", "--squash", "sync/htop-3.5.0")
        self.commit("squashed first sync")
        self.advance("3.6.0", "next upstream line\n")
        self.assertEqual(sync.prepare(str(self.upstream))["status"], "prepared")
        self.assertEqual(Path("shared.c").read_text(), "next upstream line\n")
        self.assertEqual(Path("meters.c").read_text(), "fork meters\n")

    def test_conflict_lists_files_aborts_and_keeps_marker(self):
        Path("shared.c").write_text("fork customization\n")
        self.commit("customize")
        self.advance("3.5.0", "upstream customization\n")
        result = sync.prepare(str(self.upstream))
        self.assertEqual(result["status"], "conflict")
        self.assertEqual(result["conflicts"], ["shared.c"])
        self.assertEqual(Path("shared.c").read_text(), "fork customization\n")
        self.assertEqual(Path(".htop-plus/upstream-version").read_text(), "3.4.1\n")
        self.assertFalse(Path(".git/MERGE_HEAD").exists())
        self.assertEqual(sync.git("ls-remote", "origin", "refs/heads/sync/*").stdout, "")

    def test_existing_remote_branch_is_never_overwritten(self):
        self.advance("3.5.0")
        result = sync.prepare(str(self.upstream))
        sync.git("push", "origin", "HEAD:refs/heads/" + result["branch"])
        sync.git("switch", "main")
        before = sync.git("rev-parse", "HEAD").stdout
        result = sync.prepare(str(self.upstream))
        self.assertEqual(result["status"], "existing")
        self.assertEqual(sync.git("rev-parse", "HEAD").stdout, before)

    def test_existing_remote_branch_with_wrong_marker_is_rejected(self):
        self.advance("3.5.0")
        sync.git("push", "origin", "HEAD:refs/heads/sync/htop-3.5.0")
        with self.assertRaisesRegex(RuntimeError, "wrong version"):
            sync.prepare(str(self.upstream))

    def test_dirty_checkout_rejected(self):
        Path("meters.c").write_text("uncommitted work\n")
        with self.assertRaisesRegex(RuntimeError, "clean checkout"):
            sync.prepare(str(self.upstream))

    def test_missing_base_rejected(self):
        Path(".htop-plus/upstream-version").write_text("3.0.0\n")
        self.commit("invalid base")
        with self.assertRaisesRegex(RuntimeError, "missing from upstream"):
            sync.prepare(str(self.upstream))

    def test_conflict_issue_is_deduplicated(self):
        result = {"status": "conflict", "version": "3.5.0", "conflicts": ["shared.c"]}
        with patch.object(sync, "gh", return_value=json.dumps([
                {"title": "Upstream htop 3.5.0 merge conflicts", "url": "existing"}])) as api:
            sync.publish(result)
        self.assertEqual(result["issue"], "existing")
        self.assertEqual(api.call_count, 1)

    def test_publish_dispatches_ci_then_queues_squash_without_force(self):
        self.advance("3.5.0")
        result = sync.prepare(str(self.upstream))
        pr = {"number": 1, "url": "pr-url", "id": "node-id", "headRefOid": result["head"]}
        with patch.object(sync, "gh", side_effect=["[]", "pr-url", json.dumps([pr]), "", ""]) as api:
            sync.publish(result)
        calls = [call.args for call in api.call_args_list]
        self.assertEqual(calls[-2], ("workflow", "run", "ci.yml", "--ref", "sync/htop-3.5.0"))
        self.assertTrue(any("enablePullRequestAutoMerge" in arg for arg in calls[-1]))
        self.assertTrue(any("mergeMethod:SQUASH" in arg for arg in calls[-1]))
        self.assertEqual(result["pr"], "1")
        self.assertEqual(sync.git("ls-remote", "origin", "refs/tags/*").stdout.count("3.5.0"), 1)


class DraftTests(unittest.TestCase):
    def test_next_revision_counts_tags_and_all_releases(self):
        releases = [{"tag_name": "v3.5.0-plus.2", "target_commitish": "old"},
                    {"tag_name": "v3.5.0-plus.10", "target_commitish": "other"}]
        result = draft.select_release(releases, ["v3.5.0-plus.11", "v9.0.0-plus.100"], "3.5.0", "new")
        self.assertEqual(result["tag_name"], "v3.5.0-plus.12")

    def test_same_source_is_idempotent_for_draft_and_published(self):
        for is_draft in (True, False):
            release = {"id": 1, "tag_name": "v3.5.0-plus.1", "target_commitish": "same", "draft": is_draft}
            self.assertEqual(draft.select_release([release], [], "3.5.0", "same"), release)

    def test_checksums_reject_corruption_and_missing_assets(self):
        directory = Path(tempfile.mkdtemp(prefix="htop-plus-assets-"))
        names = ["htop-plus-linux-x86_64-glibc", "htop-plus-linux-x86_64-musl",
                 "htop-plus-source.tar.gz", "COPYING", "COPYING.ncurses"]
        for name in names:
            (directory / name).write_text(name)
        (directory / "SHA256SUMS").write_text("".join(
            hashlib.sha256(name.encode()).hexdigest() + "  " + name + "\n" for name in names))
        draft.verify_assets(directory)
        (directory / names[0]).write_text("corrupted")
        with self.assertRaisesRegex(RuntimeError, "Checksum mismatch"):
            draft.verify_assets(directory)
        (directory / "unexpected").write_text("unexpected")
        with self.assertRaisesRegex(RuntimeError, "expected set"):
            draft.verify_assets(directory)

    def test_creation_remains_draft_and_never_calls_a_tag_api(self):
        version = (ROOT / ".htop-plus/upstream-version").read_text().strip()
        tag = "v" + version + "-plus.1"
        release = {"id": 1, "tag_name": tag, "target_commitish": "a" * 40,
                   "draft": True, "assets": []}
        directory = Path(tempfile.mkdtemp(prefix="htop-plus-draft-api-"))
        (directory / "candidate").write_text("candidate")
        original = Path.cwd()
        try:
            os.chdir(ROOT)
            with patch.object(draft, "verify_assets"), patch.object(draft.subprocess, "check_output", return_value="a" * 40), \
                    patch.dict(os.environ, {"GITHUB_REPOSITORY": "owner/repo"}), \
                    patch("sys.argv", ["draft-release.py", str(directory)]), \
                    patch.object(draft, "gh", side_effect=["[[]]", "[[]]", json.dumps(release), ""]) as api:
                draft.main()
            calls = api.call_args_list
            payload = calls[2].kwargs["payload"]
            self.assertIs(payload["draft"], True)
            self.assertEqual(payload["target_commitish"], "a" * 40)
            self.assertEqual(calls[-1].args, ("release", "upload", tag, str(directory / "candidate")))
            self.assertFalse(any("/git/" in arg for call in calls for arg in call.args))
        finally:
            os.chdir(original)


class StatusReporterTests(unittest.TestCase):
    def report(self, conclusion="success", event="workflow_dispatch", actor="github-actions[bot]", workflow_id=123):
        directory = Path(tempfile.mkdtemp(prefix="htop-plus-status-"))
        log = directory / "calls.jsonl"
        executable = directory / "gh"
        executable.write_text('''#!/usr/bin/env python3
import json, os, sys
with open(os.environ['API_CALL_LOG'], 'a') as f:
    f.write(json.dumps(sys.argv[1:]) + '\\n')
if '/actions/runs/' in sys.argv[2]:
    print(os.environ['RUN_METADATA'])
elif '/actions/workflows/' in sys.argv[2]:
    print('123')
''')
        executable.chmod(0o755)
        workflow = (ROOT / ".github/workflows/ci-status.yml").read_text()
        script = textwrap.dedent(workflow.split("        run: |\n", 1)[1])
        metadata = {"workflow_id": workflow_id, "path": ".github/workflows/ci.yml", "status": "completed",
                    "event": event, "actor": {"login": actor}, "head_sha": "a" * 40,
                    "html_url": "https://github.com/owner/repo/actions/runs/99", "conclusion": conclusion}
        env = dict(os.environ, PATH=str(directory) + os.pathsep + os.environ["PATH"],
                   GH_REPO="owner/repo", RUN_ID="99", API_CALL_LOG=str(log), RUN_METADATA=json.dumps(metadata))
        result = subprocess.run(["bash", "-c", script], env=env, text=True, capture_output=True)
        calls = [json.loads(line) for line in log.read_text().splitlines()]
        return result, [call for call in calls if "/statuses/" in call[1]]

    def test_dispatch_success_posts_required_status_on_exact_sha(self):
        result, posts = self.report()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(posts), 1)
        self.assertEqual(posts[0][1], "repos/owner/repo/statuses/" + "a" * 40)
        self.assertIn("state=success", posts[0])
        self.assertIn("context=CI", posts[0])

    def test_failed_build_reports_failure(self):
        result, posts = self.report(conclusion="failure")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("state=failure", posts[0])

    def test_wrong_workflow_cannot_post_success(self):
        result, posts = self.report(workflow_id=456)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(posts, [])

    def test_bot_pr_approval_timeout_does_not_replace_dispatched_result(self):
        result, posts = self.report(conclusion="action_required", event="pull_request")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(posts, [])


class ReleaseSourceTests(unittest.TestCase):
    def select(self, complete=False, requested="", event="schedule"):
        original = Path.cwd()
        directory = Path(tempfile.mkdtemp(prefix="htop-plus-release-source-"))
        try:
            os.chdir(directory)
            with patch.dict(os.environ, {"GIT_AUTHOR_NAME": sync.IDENTITY, "GIT_AUTHOR_EMAIL": sync.EMAIL,
                                         "GIT_COMMITTER_NAME": sync.IDENTITY, "GIT_COMMITTER_EMAIL": sync.EMAIL}):
                sync.git("init", "-b", "main")
                Path(".htop-plus").mkdir()
                Path(".htop-plus/upstream-version").write_text("3.4.1\n")
                sync.git("add", ".")
                sync.git("commit", "-m", "record upstream version")
                marker = sync.git("rev-parse", "HEAD").stdout.strip()
                Path("README.md").write_text("unrelated documentation change\n")
                sync.git("add", ".")
                sync.git("commit", "-m", "unrelated change")
                head = sync.git("rev-parse", "HEAD").stdout.strip()
                sync.git("update-ref", "refs/remotes/origin/main", head)
            names = ["COPYING", "COPYING.ncurses", "SHA256SUMS", "htop-plus-linux-x86_64-glibc",
                     "htop-plus-linux-x86_64-musl", "htop-plus-source.tar.gz"]
            releases = [[{"target_commitish": marker, "assets": [
                {"name": name, "state": "uploaded", "size": 1} for name in names]}]] if complete else [[]]
            executable = directory / "gh"
            executable.write_text("#!/bin/sh\nprintf '%s\\n' \"$RELEASE_METADATA\"\n")
            executable.chmod(0o755)
            workflow = (ROOT / ".github/workflows/release.yml").read_text()
            script = textwrap.dedent(workflow.split("        run: |\n", 1)[1].split("  glibc:\n", 1)[0])
            output = directory / "output"
            env = dict(os.environ, PATH=str(directory) + os.pathsep + os.environ["PATH"],
                       GH_REPO="owner/repo", GITHUB_OUTPUT=str(output), GITHUB_SHA=head,
                       REQUESTED_SHA=requested, GITHUB_EVENT_NAME=event, RELEASE_METADATA=json.dumps(releases))
            result = subprocess.run(["bash", "-c", script], env=env, text=True, capture_output=True)
            values = dict(line.split("=", 1) for line in output.read_text().splitlines()) if output.exists() else {}
            return result, values, marker, head
        finally:
            os.chdir(original)

    def test_daily_catchup_pins_marker_commit_and_skips_completed_draft(self):
        result, values, marker, head = self.select(complete=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotEqual(marker, head)
        self.assertEqual(values, {"sha": marker, "build": "false"})

    def test_daily_catchup_builds_missing_draft(self):
        result, values, marker, _ = self.select()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(values, {"sha": marker, "build": "true"})

    def test_manual_dispatch_rejects_invalid_source(self):
        result, values, _, _ = self.select(requested="main;echo unsafe", event="workflow_dispatch")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(values, {})


if __name__ == "__main__":
    unittest.main()
