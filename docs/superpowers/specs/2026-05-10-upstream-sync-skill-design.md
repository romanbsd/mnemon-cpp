# Upstream-sync skill — design

**Date:** 2026-05-10
**Status:** Approved (awaiting implementation plan)
**Audience:** future Claude instances and human contributors implementing this skill

## Goal

Provide a project-scoped skill that keeps `mnemon-cpp` in lockstep with upstream
`https://github.com/mnemon-dev/mnemon.git`. For each new upstream commit, the
skill classifies it, ports the change to C++ (with a test), opens a PR, and
mirrors upstream tags onto this repo after the corresponding port-PRs merge.

The port is a drop-in replacement of the Go binary, so this skill must respect
the parity contract enforced by `mnemon-spec.md` and `scripts/e2e_test.sh`.

## Non-goals

- Auto-merging port-PRs. The human merges in order; the skill only opens PRs.
- Owning C++ release versioning. Tags mirror upstream verbatim.
- Replacing `scripts/sync_setup_assets_from_monorepo.sh`. Asset-only upstream
  commits delegate to that script.

## Decisions (locked)

| Decision | Choice |
|---|---|
| Sync tracking | Committed file `.upstream-sync` (key=value) |
| Interaction model | Fully autonomous batch |
| PR sequencing | Stacked PRs, each branched off the previous; human merges in order |
| Non-applicable commits | Skip silently, advance tracker |
| Setup-asset commits | Delegate to existing `sync_setup_assets_from_monorepo.sh` |
| Skill location | `.claude/skills/upstream-sync/SKILL.md` (in-repo) |
| Upstream source | Add `upstream` git remote on this repo |
| Tag naming | Identical to upstream (`v0.7.3` → `v0.7.3`) |
| Merge gating | Skill opens PRs and stops; tag mirroring is a separate step |
| Implementation shape | SKILL.md + `scripts/upstream_sync.sh` + `scripts/upstream_release_tags.sh` + `/upstream-sync` slash command |

## Architecture

### Files added

- `.claude/skills/upstream-sync/SKILL.md` — judgment loop (the skill itself).
- `.claude/commands/upstream-sync.md` — slash command that invokes the skill.
- `scripts/upstream_sync.sh` — deterministic helper. Subcommands:
  - `init` — first-run baseline; writes `.upstream-sync` from current `upstream/main` HEAD.
  - `classify` — fetches upstream and emits JSON for the commit range.
  - `advance <sha>` — bumps `.upstream-sync` to a SHA (used by the skill).
  - `release-tags` — convenience pass-through to `upstream_release_tags.sh`.
- `scripts/upstream_release_tags.sh` — mirrors upstream tags after their port-PRs merge. Single-purpose script; no subcommands.
- `.upstream-sync` — committed tracker file (key=value).
- `.upstream-tag-pending` — committed sidecar mapping `<tag>=<upstream-sha>`.
- `tests/fixtures/upstream-sync/upstream.tar.gz` — synthetic upstream history fixture.
- `tests/upstream_sync_helper_test.sh` — runs the helper against the fixture and asserts JSON.

### Boundary

The helper script never creates branches, commits, or PRs. It reads upstream,
classifies, and writes only to `.upstream-sync` / `.upstream-tag-pending` on
disk. The skill (Claude) is the only thing that creates branches, commits, and
PRs. A human can run `bash scripts/upstream_sync.sh classify` to inspect the
current sync plan with no side effects.

## Data model

### `.upstream-sync` (committed at repo root)

```
# Last upstream commit evaluated by /upstream-sync.
# Do not hand-edit unless reseeding via `scripts/upstream_sync.sh init`.
upstream_url=https://github.com/mnemon-dev/mnemon.git
last_sha=e6f3a91b...
last_sha_subject=Bump cli11 to 2.5.0
last_sha_date=2026-05-08T12:34:56Z
```

Plain `key=value`, line-oriented. The helper reads/writes only the four keys;
unknown lines are preserved.

### `.upstream-tag-pending` (committed at repo root)

```
# Upstream tags awaiting mirror to mnemon-cpp after their port-PR is merged.
# Maintained by /upstream-sync; cleared by scripts/upstream_release_tags.sh.
v0.7.3=abc123def456...
v0.7.4=789ghi012jkl...
```

`<upstream-tag>=<upstream-sha>`. Tag mirroring runs after merges; we cannot
record a local SHA at PR-open time because squash-merge rewrites it. Instead,
the port-PR body carries `Upstream-Commit: <sha>` as a trailer, and
`upstream_release_tags.sh` greps `master` history for that trailer to find the
matching local commit.

## The autonomous batch flow

### Entry point

User types `/upstream-sync`. The slash command invokes the skill. The skill
runs:

```bash
bash scripts/upstream_sync.sh classify
```

### Helper output

```json
{
  "baseline_sha": "e6f3a91...",
  "head_sha":     "ab12cd3...",
  "commits": [
    { "sha": "...", "subject": "...", "tag": null,    "class": "doc-meta-only", "files": [...] },
    { "sha": "...", "subject": "...", "tag": null,    "class": "asset-only",    "files": [...] },
    { "sha": "...", "subject": "...", "tag": "v0.7.3","class": "relevant",      "files": [...] }
  ]
}
```

### Classifier rules (deterministic, in helper)

- `asset-only` — every changed path begins with `internal/setup/assets/`.
- `doc-meta-only` — every path matches `*.md`, `LICENSE`,
  `.github/workflows/*.yml`, `.gitignore`, `Makefile`, `go.mod`, or `go.sum`.
  (Go test files are deliberately excluded — a `_test.go` change can encode
  behavior, so we let the skill decide.)
- `relevant` — anything else. The skill decides at port-time whether the
  diff actually translates to a C++ change.

The script never tries to predict what's "Go-only logic worth porting" — that's
judgment. It only quarantines obvious no-ops.

### Per-commit loop in the skill

For each commit in classification order (oldest first), the skill picks a
branch base:

- First commit: `master`.
- Subsequent: previous commit's branch tip (regardless of class).

Then it dispatches by class:

#### `doc-meta-only`

No branch. Accumulate the SHAs. Advance `.upstream-sync` to the latest no-op
SHA inside whatever branch comes next. If the batch ends on no-ops, open one
final housekeeping PR carrying only the tracker bump.

#### `asset-only`

Branch `upstream-sync/<short-sha>` off previous. Run
`bash scripts/sync_setup_assets_from_monorepo.sh`. Build (`make build`) — this
regenerates `build/embedded_assets.{hpp,cpp}` automatically per the existing
CMake `ASSET_DEPS` wiring. Run `make test`. Bump `.upstream-sync` to this SHA.
Commit, push, `gh pr create --base <previous-branch>` with the standard PR body.

#### `relevant`

Branch `upstream-sync/<short-sha>` off previous. The skill:

1. Reads the upstream diff via `git show upstream/<sha>` (the `upstream` remote
   is fetched on this repo).
2. Reads `mnemon-spec.md` and the C++ files the diff implicates. The
   cross-language mapping is judgment; the spec is the contract.
3. Decides whether this needs a port. If no (e.g., upstream refactored Go
   internals with no observable behavior change), treats as no-op-with-reason
   and follows `doc-meta-only` handling.
4. If yes: writes/modifies a test first. E2E section in `scripts/e2e_test.sh`
   for behavioral changes; Catch2 in `tests/smoke_test.cpp` for unit-scope
   logic. Test must fail before the impl change.
5. Implements the change in `src/`.
6. `make build && make test`. Loops until green or escalates.
7. Bumps `.upstream-sync` to this SHA. If the commit has an upstream tag, also
   writes the entry into `.upstream-tag-pending`.
8. Commits with a message that includes `Upstream-Commit: <full-sha>` as a
   trailer (and `Upstream-Tag:` if applicable). Pushes. Opens stacked PR with
   `--base <previous-branch>` and the body shape below.

### PR title and body

Title: `Port upstream <short-sha>: <upstream subject>`

Body:

```
Ports upstream commit <full-sha> from mnemon-dev/mnemon.

Upstream subject: <subject>
Upstream message:
<body>

## Changes
- tests/...: <what test was added/modified>
- src/...: <what was changed>

## Why this commit applies
<one paragraph justification>

## Verification
- make build: PASS
- make test: PASS

Upstream-Commit: <full-sha>
Upstream-Tag: <tag>   # only when tagged
```

### End of batch

The skill prints a summary: PRs opened (URLs), no-op SHAs collapsed, pending
tags. Does **not** auto-merge. Does **not** push tags.

### Idempotence checkpoints

After every successful PR open, the skill stores a recallium memory recording
`<upstream-sha> -> <PR url>`. If re-invoked mid-batch (network blip, kill), the
skill cross-references those notes against `gh pr list` to detect already-opened
PRs and skip them.

## Tag mirroring (post-merge)

Triggered by `/upstream-sync release-tags` or
`bash scripts/upstream_release_tags.sh`.

1. Reads `.upstream-tag-pending` from `master`.
2. For each `<tag>=<upstream-sha>` entry:
   - `git log master --grep="^Upstream-Commit: <upstream-sha>"` to find the
     local commit. If not found, skip — PR not merged yet.
   - If found: `git tag <tag> <local-sha>`, `git push origin <tag>`, remove the
     entry from `.upstream-tag-pending`.
   - `gh release create <tag> --generate-notes --target <local-sha>` (skippable
     with `--no-release`).
3. If any entries were cleared, open a housekeeping PR carrying the cleared
   sidecar.

Idempotent: re-running with no merges since last invocation is a no-op.

### Squash-merge / trailer-strip fallback

GitHub may strip trailers from the squash-merge commit message under some
configurations (the PR body itself is preserved). If `git log --grep` finds no
match for an `Upstream-Commit` trailer, the script falls back to:

```
gh pr list --state merged --search "Upstream-Commit: <sha> in:body" \
  --json mergeCommit -q '.[0].mergeCommit.oid'
```

This returns the local squash-commit SHA on `master`, which the script then
tags. Only if both lookups fail does the script leave the entry pending and
log a warning.

## Failure modes

| Scenario | Skill behavior |
|---|---|
| `git fetch upstream` fails | Helper exits non-zero. Skill aborts with no side effects. |
| Upstream history rewritten — recorded `last_sha` not in `upstream/main` | Helper detects via `git merge-base --is-ancestor`. Skill aborts; tells user to re-seed via `init`. |
| `make build` or `make test` fails on a port commit | Skill stops. Branch unpushed, tracker un-advanced. Reports failing commit + diff of what it tried. |
| `gh pr create` fails | Skill stops, surfaces the gh error. User cleans up; re-invocation resumes. |
| Mid-batch resume after kill | Recallium notes + `gh pr list` cross-check skip already-opened PRs. |
| Trailer stripped by squash-merge | Fall back to `gh pr list --state merged --search`. |

## Setup and idempotence

The helper script is idempotent:

- `init` — only writes `.upstream-sync` if missing or if `--force` is passed.
- `classify` — pure read; never mutates working tree.
- `advance <sha>` — verifies `<sha>` is in `upstream`'s history before bumping.
- Adds the `upstream` remote on first invocation:
  `git remote get-url upstream || git remote add upstream <url>`.
- Fetches with `--tags`; unshallows if needed.

## Testing

### Helper script (deterministic)

Fixture at `tests/fixtures/upstream-sync/upstream.tar.gz` is a synthetic
upstream repo with five commits covering each class:

1. Asset-only.
2. Doc-only.
3. Go-only relevant (e.g., a CLI flag change).
4. Mixed relevant (CLI + setup asset).
5. Tagged relevant (carries `v0.0.1`).

Test runner at `tests/upstream_sync_helper_test.sh` extracts the fixture into a
temp dir, points `MNEMON_REPO_URL` at the local path, runs
`scripts/upstream_sync.sh classify`, and asserts the JSON shape with `jq`.
Wired into `make test-helper` (a new make target) and run from CI.

### Skill (pressure tests with subagents)

Following the writing-skills TDD discipline:

- **RED**: spawn a baseline subagent (no skill loaded) against a scratch
  checkout with the fixture in place. Prompt: *"You have these five upstream
  commits, port them per CLAUDE.md."* Record verbatim rationalizations and
  failures. Likely failures: forget to advance tracker, process out of order,
  skip test-first, open all PRs off `master`, miss the tag.
- **GREEN**: write SKILL.md addressing each observed failure with explicit
  rules ("Branch off the previous commit's branch, NOT master, even when the
  previous was asset-only"). Re-run; verify compliance.
- **REFACTOR**: run pressure variants — *"the test is flaky, just skip it"*,
  *"the build is slow, skip verification"*, *"this commit looks trivial, batch
  it with the next one"*. Plug each loophole inline in SKILL.md.

The fixture and helper test ship with the same PR that introduces the skill.
The pressure tests are documented in the implementation plan and executed in a
follow-up session.

## Open questions

None at design time. If reality shifts (upstream changes default branch, asset
sync script gets retired, CI changes squash policy), revisit this spec.
