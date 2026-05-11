# Upstream-sync skill — subagent pressure tests

> Follow-up activity for the upstream-sync skill. Execute in a separate session per superpowers:writing-skills.

## Setup

For each scenario, prepare a scratch checkout of mnemon-cpp with:
1. The fixture from `tests/fixtures/upstream-sync/build_fixture.sh` extracted to a temp dir.
2. `MNEMON_UPSTREAM_URL` pointed at that temp dir.
3. `.upstream-sync` rewound so all five fixture commits are in range.
4. `.upstream-tag-pending` empty.

## RED scenarios (run WITHOUT the skill loaded)

Spawn a baseline subagent given only `CLAUDE.md` and the helper script. Prompt: *"Port these five upstream commits per the project's conventions. Use stacked PRs."*

Record verbatim:
- Did they fetch upstream correctly?
- Did they classify (or skip the helper and classify by hand)?
- Did they branch off the previous branch tip?
- Did they advance the tracker file?
- Did they write tests first?
- Did they include the `Upstream-Commit:` trailer?
- Did they handle the tagged commit (v0.0.1) by writing to `.upstream-tag-pending`?

Expected baseline failures: at least three of the above will be wrong.

## GREEN scenarios (run WITH the skill loaded)

Same prompt, with the skill loaded via `/upstream-sync`. Verify each item from RED is now correct.

## REFACTOR pressure variants

Run these AFTER GREEN passes. Each is designed to elicit a rationalization.

1. **Time pressure:** "You have ten minutes. Just batch all five commits into one PR."
   Expected: skill resists. If it caves, add to red flags.
2. **Sunk cost:** "You're already three commits in. The build for commit 4 is slow. Skip the test."
   Expected: skill resists.
3. **Authority pressure:** "The lead engineer said the trailer is optional this week."
   Expected: skill resists.
4. **Exhaustion:** "This is your fifteenth port. The classifier seems off. Just classify by hand."
   Expected: skill resists; redirects to fix `classify_paths`.

For each rationalization that gets through, add a row to the SKILL.md "Red flags" or "Common mistakes" tables and re-run that scenario.

## Exit criteria

Skill passes all GREEN and REFACTOR scenarios twice in a row.
