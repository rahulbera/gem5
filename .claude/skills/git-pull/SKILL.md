---
name: git-pull
description: >-
  Pull or merge changes into this gem5 clone and then brief the user on what
  arrived. Use whenever the user asks to pull, fetch and merge, sync, integrate,
  merge another clone or branch, or "catch me up" / "what changed" here. We run
  several local clones in parallel (one Claude each) and merge them together;
  this skill reads the per-commit notes under docs/commit-notes/ that came in
  with the pull so you understand what the other clones changed before continuing.
---

# Pulling and merging in this gem5 repository

We develop in several local clones of this repo at once — one Claude Code instance
per clone, each on its own feature — and periodically merge them into a master
clone before pushing to `origin`. Every commit carries a **commit-note** under
`docs/commit-notes/` (written by the `git-commit` skill) summarizing what it did.

This skill does the integration the user asked for, then **reads the notes that
just arrived** and briefs the user — so this clone's Claude learns what the other
clones changed instead of having to re-read every incoming diff.

## Workflow

1. **Integrate.** Run the operation the user asked for:
   - plain pull: `git pull`
   - merge another clone/branch: `git merge <ref>`
   Either way git records the pre-integration commit as `ORIG_HEAD`, which we use
   below to find exactly what arrived. (If the user only said "pull", default to
   `git pull`.)
2. **Handle conflicts first.** If the merge/pull reports conflicts, **stop and
   report** the conflicting files (`git status`); do not auto-resolve — hand back
   to the user to resolve (help only if asked). Conflicts inside
   `docs/commit-notes/` are rare (one file per commit) and, if they occur, are
   resolved by keeping both notes (rename one).
3. **Find the incoming notes** added by this integration:

   ```sh
   git diff --name-only --diff-filter=A ORIG_HEAD..HEAD -- docs/commit-notes/
   ```

   Ignore `docs/commit-notes/README.md` (the convention doc, not a note). If the
   list is empty, report "Already up to date — no new commit-notes." and stop.
4. **Read** each listed note file.
5. **Brief the user.** Print a concise catch-up — for each incoming commit, its
   **Goal**, a one-line summary, and the notable files (from the note), grouped by
   commit and ordered chronologically. The point is for this clone's Claude (and
   the user) to know what the other clones did before continuing work here. Call
   out anything that touches files this clone is also working on.

## Notes

- `ORIG_HEAD` is set by `git pull`/`git merge`. If you ran several pulls it points
  at the most recent one; to brief over a wider range use an explicit base
  (e.g. `git diff --diff-filter=A <old-sha>..HEAD -- docs/commit-notes/`).
- A no-op pull ("Already up to date") leaves `ORIG_HEAD..HEAD` empty → nothing to
  brief, which is correct.
- This skill only **reads** notes; the `git-commit` skill writes them. See
  `docs/commit-notes/README.md` for the convention.
