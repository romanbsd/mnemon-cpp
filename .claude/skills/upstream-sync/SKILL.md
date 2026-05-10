---
name: upstream-sync
description: Use when porting recent commits from mnemon-dev/mnemon (the Go reference for this C++ port) into mnemon-cpp. Triggered by /upstream-sync. Opens stacked PRs — does not auto-merge.
---

# upstream-sync

## When to use

Invoke via `/upstream-sync` when you want to bring this repo up to date with new commits on `https://github.com/mnemon-dev/mnemon` (the `upstream` git remote). The skill processes ALL new upstream commits in one autonomous batch, opening one stacked port-PR per commit that needs porting.

The skill does NOT merge PRs. The human merges in order. After merges, run `/upstream-sync release-tags` (or `bash scripts/upstream_release_tags.sh`) to mirror upstream tags onto this repo.

## Pre-flight (one-time)

1. `bash scripts/upstream_sync.sh init` — adds `upstream` remote, seeds `.upstream-sync`.
2. Commit `.upstream-sync` and an empty `.upstream-tag-pending` to master.

If `.upstream-sync` is missing when you invoke the skill, stop and tell the user to run pre-flight.

## The autonomous batch loop

### Step 1 — Classify

Run:

```bash
bash scripts/upstream_sync.sh classify
```

The helper emits JSON:

```json
{
  "baseline_sha": "...",
  "head_sha": "...",
  "commits": [
    { "sha": "...", "subject": "...", "tag": null,
      "class": "asset-only|doc-meta-only|relevant", "files": [...] }
  ]
}
```

If `commits` is empty: print "Up to date with upstream." and stop.

### Step 2 — Resume check

Search recallium for memories tagged `upstream-sync` referencing any SHA in the classify output. Cross-check `gh pr list --search "head:upstream-sync/" --state open --json headRefName -q '.[].headRefName'`. Skip any commit whose port-branch already exists upstream.

### Step 3 — Per-commit dispatch

Process commits in the order returned (oldest first). For each commit `C`:

**Branch base:**
- First commit processed in this batch: `master`.
- Subsequent: the previous processed commit's branch tip (regardless of class).

**Branch name:** `upstream-sync/<short-sha-of-C>`.

**Dispatch by class:**

#### `doc-meta-only`

No branch. Accumulate the SHA. When the next branch is created, run `bash scripts/upstream_sync.sh advance <sha>` for each accumulated SHA before doing other work, and include the resulting `.upstream-sync` change in that branch's commit. If the batch ends on `doc-meta-only` commits with no relevant follow-up, open a final PR off the last branch tip (or master) titled `Housekeeping: advance upstream-sync tracker through N no-op commits` carrying only the tracker bump.

#### `asset-only`

1. Create branch off previous tip.
2. Run `bash scripts/sync_setup_assets_from_monorepo.sh`.
3. `make build && make test`. If either fails, STOP. Do not push. Do not advance the tracker.
4. `bash scripts/upstream_sync.sh advance <full-sha>`.
5. If `tag` is non-null in the classify output, append `<tag>=<full-sha>` to `.upstream-tag-pending`.
6. `git add setup_assets/ .upstream-sync` (and `.upstream-tag-pending` if applicable).
7. Commit with the standard trailer (see "Commit and PR shape" below).
8. `git push -u origin upstream-sync/<short-sha>`.
9. `gh pr create --base <previous-branch-or-master> --title "Port upstream <short-sha>: <subject>" --body-file <body>`.
10. Store recallium memory: type=feature, content includes upstream SHA, PR URL, files changed; tags=["upstream-sync"]; related_files=[setup_assets/...].

#### `relevant`

1. Create branch off previous tip.
2. Read upstream diff: `git show <full-sha>` (the `upstream` remote is fetched after `classify`; the SHA is locally resolvable).
3. Decide: does this commit need a C++ port? If clearly no-op (e.g., upstream refactored a private Go helper with no observable behavior change), treat as `doc-meta-only` — accumulate and move on. State your reasoning in a recallium note (type=decision).
4. If a port is needed: write or modify a TEST FIRST.
   - Behavioral changes (CLI output, JSON keys, error wording, exit codes, schema): edit `scripts/e2e_test.sh`. Follow the existing `assert_jq` and milestone-banner patterns.
   - Pure in-process logic (engine helpers, math, parsing): add a Catch2 test in `tests/smoke_test.cpp`.
   - The test must FAIL before you implement.
5. Implement the change in `src/`. Match `mnemon-spec.md` parity over idiomatic C++ — JSON keys, error wording, exit codes, file layout are part of the contract enforced by `scripts/e2e_test.sh`.
6. `make build && make test`. Loop until green. If you can't reach green after three reasonable iterations, STOP and report.
7. `bash scripts/upstream_sync.sh advance <full-sha>`.
8. If `tag` is non-null, append `<tag>=<full-sha>` to `.upstream-tag-pending`.
9. `git add` impacted source/test files plus `.upstream-sync` (and `.upstream-tag-pending` if applicable).
10. Commit with trailer.
11. Push and `gh pr create --base <previous-branch-or-master> --title "Port upstream <short-sha>: <subject>" --body-file <body>`.
12. Store recallium memory: type=feature or code-snippet; content covers upstream SHA, the C++ files changed, the rationale; related_files=[the modified files]; tags=["upstream-sync"].

### Step 4 — Summary

After the batch, print:

- PRs opened, with URLs.
- Number of `doc-meta-only` SHAs collapsed.
- Pending tags: count from `.upstream-tag-pending`.
- Anything that failed (commit SHA + reason).

## Commit and PR shape

Commit message:

```
Port upstream <short-sha>: <subject>

<one-paragraph rationale>

Upstream-Commit: <full-sha>
[Upstream-Tag: <tag>]
```

PR body:

```
Ports upstream commit <full-sha> from mnemon-dev/mnemon.

Upstream subject: <subject>
Upstream message:
<body>

## Changes
- tests/...: <one-line summary>
- src/...: <one-line summary>

## Why this commit applies
<one paragraph>

## Verification
- make build: PASS
- make test: PASS

Upstream-Commit: <full-sha>
[Upstream-Tag: <tag>]
```

The `Upstream-Commit:` trailer is load-bearing: `scripts/upstream_release_tags.sh` greps for it after merge to find the local commit for tag mirroring. Verbatim format. Don't reformat. Don't drop. The trailer goes in BOTH the commit message AND the PR body — squash-merge can strip the commit-message version, but the PR body is preserved.

## Tag mirroring (post-merge)

When the user runs `/upstream-sync release-tags` (or invokes the script directly):

```bash
bash scripts/upstream_release_tags.sh
```

The script reads `.upstream-tag-pending`, finds local commits via the trailer (with a `gh pr list --search "... in:body"` fallback), tags them with the upstream tag verbatim, pushes the tags, and creates GitHub releases. It removes cleared entries from `.upstream-tag-pending`. If any entries cleared, open a small housekeeping PR carrying the cleared sidecar.

## Failure modes

| Scenario | What you do |
|---|---|
| Helper exits non-zero (network, history rewrite) | STOP. Print the error. Do not open PRs. Do not advance tracker. |
| `make build` or `make test` fails on a port | STOP. Don't push. Don't advance tracker. Report which commit, which test/build output. |
| `gh pr create` fails | STOP. Surface the gh error. Local branch and tracker bump exist locally; user can re-invoke after cleanup. |
| Mid-batch resume after a kill | Use the Step 2 resume check — recallium notes + `gh pr list --search "head:upstream-sync/"`. |
| Squash-merge stripped the trailer | The release-tags script handles the fallback automatically via `gh pr list --search "... in:body"`. |

## Red flags — STOP and re-read

| Thought | Reality |
|---|---|
| "I'll skip the test for this small change" | Test-first is the contract. Write the test. |
| "Branching off master is simpler than chasing the previous branch" | Stacked PRs require the previous branch as base. Master-base will conflict. |
| "Let me batch these two commits into one PR" | One commit, one PR. Trailer machinery and tag mirroring depend on it. |
| "The classifier got it wrong, I'll re-classify mentally" | Trust the deterministic classifier. If it's wrong, fix `classify_paths` in `scripts/upstream_sync.sh`, not the skill loop. |
| "The trailer is just metadata, I'll shorten it" | The `Upstream-Commit:` trailer is load-bearing for tag mirroring. Verbatim. |
| "I'll auto-merge to speed things up" | The user merges. The skill stops at PR open. |

## Common mistakes

- Forgetting to bump `.upstream-sync` inside the same commit that opens the PR. Result: next batch replays the same commit.
- Including unrelated changes in a port-PR. Each PR contains only the C++ diff for its upstream commit (plus tracker bump and optional tag-pending entry).
- Force-pushing to a stacked branch after later branches have been built off it. If you must rewrite, rebuild the entire stack from that point forward.
