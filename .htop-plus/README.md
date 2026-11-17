# Maintaining upstream sync and release drafts

`.htop-plus/upstream-version` records the upstream stable tag integrated into
`main`. Keep it accurate when resolving sync conflicts manually.

## One-time owner setup

1. Review and push the workflow changes to `main`, then run **Build and test**
   once. **Report CI status** must complete too, so the `CI` status is available
   in branch protection's selector.
2. Under **Settings → General → Pull Requests**, enable squash merging and
   **Allow auto-merge**.
3. Under **Settings → Actions → General → Workflow permissions**, enable
   **Allow GitHub Actions to create and approve pull requests**. Leave the
   default token permissions restricted; each job declares its own scopes.
4. Under **Settings → Branches**, edit the `main` protection rule. Check
   **Require status checks to pass before merging**, select **CI**, and check
   **Require branches to be up to date before merging**. Do not require the
   `Build verdict` workflow-job check: dispatched job checks are not eligible
   for branch rulesets. Existing required reviews still apply.
5. Protect `v*-plus.*` tags with an owner-only tag ruleset, without an Actions
   bypass. Automation only proposes a tag name in an unpublished draft; the
   owner creates/pushes the actual tag at the draft's exact source commit.

No extra secrets, personal token or GitHub App is needed. Repository/account
policies must permit the requested Actions scopes and auto-merge. Branch
protection availability depends on the repository's visibility and GitHub plan.

## Sync behavior

**Sync upstream** runs daily at 06:23 UTC or manually on `main`. Stable tags
match exactly `X.Y.Z` and sort numerically; prereleases are ignored. Upstream
tags are fetched into `refs/htop-upstream/`, separate from release tags.

The initial import and every squash merge omit upstream's parent history.
Before merging a newer tag, the script attaches the recorded old upstream tag
to the sync branch with an `ours` strategy merge that preserves the fork's
entire tree. The subsequent normal three-way merge applies the upstream delta.
This ancestry repair is local to the sync branch and is repeated after squash.

A clean merge updates the marker, creates `sync/htop-X.Y.Z`, opens a PR and
queues squash auto-merge. A conflicting merge is aborted and an issue lists
the conflicting files; no branch is pushed. Existing branches are inspected
and never force-pushed. If `main` advances, the strict branch requirement holds
the PR until the owner updates/resolves it. The sync workflow waits for its
explicitly dispatched CI run; failures leave the PR open for review.

To dry-run without any GitHub writes, use a disposable clean clone containing
the committed version marker:

```sh
python3 scripts/sync-upstream.py
# A local upstream clone can be supplied with --upstream /path/to/clone.
```

The default mode only creates a local sync branch. `--github` enables pushes,
issue/PR creation, CI dispatch and auto-merge, and requires the official upstream.
All generated commits use the repository owner's specified public Git identity.

## CI and event handling

The original distro, sanitizer, Valgrind and static jobs remain. The automation
job tests sync/release behavior offline. `Build verdict` succeeds only if all
jobs succeed; skipped or failed prerequisites fail the verdict.

`GITHUB_TOKEN`-created PR workflow runs can require owner approval. Sync therefore
dispatches CI explicitly on the exact sync branch head. GitHub does not accept
workflow-dispatch job checks as PR branch-ruleset requirements. A separate
**Report CI status** workflow verifies the completed build run's identity and
posts its conclusion as the `CI` commit status on the exact tested SHA. It never
checks out code or consumes artifacts from the triggering run. Approval-gated
bot PR runs are ignored by the reporter so their later timeout cannot replace
the explicit dispatch result. The normal PR and push runs are still supported.

After auto-merge, the sync workflow dispatches **Draft release** explicitly,
because token-originated pushes do not start another workflow. If auto-merge is
still waiting for other rules after five minutes, the PR stays queued. A later
owner merge changing the marker starts release builds through the push trigger.
If a delayed bot merge occurs after the wait ends, the daily 07:53 UTC release
catch-up builds the newest marker-changing main commit. Complete drafts/releases
for that exact commit are skipped; unrelated main changes do not create releases.

GitHub's relevant behavior is documented here:

https://docs.github.com/en/actions/concepts/security/github_token

https://docs.github.com/en/pull-requests/how-tos/merge-and-close-pull-requests/troubleshooting-required-status-checks

## Release owner gate

**Draft release** starts when an owner push to `main` changes the marker or
when manually dispatched on `main`, with daily catch-up as described above. Its optional `source_sha` must be a full
commit SHA reachable from `main`; otherwise it builds the event's main commit.
Both build jobs and the draft use this one pinned SHA.

The glibc job builds on Debian trixie and runs the full device, fake-NVML,
meter/layout and terminal suites. NVML remains optional and loaded at runtime.
The Alpine job builds static musl, checks its ELF dependencies and size budget,
runs terminal smoke tests, runs the fixture suite in a separate dynamic musl
build, and runs the static candidate in a scratch container without terminfo.
Static musl cannot load NVIDIA NVML; its AMD/sysfs meters remain available.

Only after both jobs succeed, the draft job combines binaries, matching source,
GPL/ncurses licenses and checksums. It proposes `vX.Y.Z-plus.N`, with `N` one above
existing tags and all draft/published releases for that upstream version.
The same source resumes its existing draft; a published source is a no-op.
Missing draft assets can be resumed; differing existing assets cause a safe
failure and are never overwritten. Published releases are never modified.

The owner must validate the candidate on a fresh machine, complete the project's
fresh-VM install/update release gate, create and push the proposed release tag
at the exact source commit, then publish the draft. The workflow contains no
tag push and never publishes a release. Manual dispatch can prepare an initial
`3.4.1` draft before any upstream update.

The owner can resolve conflicts manually on a sync branch, retain the meters,
update `.htop-plus/upstream-version`, and run CI before merging. Automation never
claims that an unresolved upstream release is integrated.
