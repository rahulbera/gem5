---
name: git-commit
description: >-
  Create git commits in this gem5 repository using gem5's required tag-based
  commit-message format. Use this skill whenever the user asks to commit, check
  in, or record changes here — "commit this", "make a commit", "commit and
  push", "save my work" — and before running any `git commit` in this repo, so
  the header uses a valid gem5 tag and passes the installed commit-msg hook.
  This skill also guarantees that NO AI co-authorship or attribution trailer is
  added to the commit message.
---

# Committing in the gem5 repository

This repo is a private fork of gem5 (`origin` = your fork, `upstream` =
gem5/gem5); we are **not upstreaming** for now. Even so, gem5 ships a
`commit-msg` hook (`util/git-commit-msg.py`, installed through pre-commit) that
**rejects** any commit whose first line isn't a valid gem5 header. Following the
format below means commits land on the first try and `git log` stays readable.

Three rules are non-negotiable here:

1. Use gem5's **tag-based header format** (the hook enforces it).
2. **Never add AI co-authorship or attribution trailers** — no
   `Co-Authored-By: Claude ...`, no `Claude-Session: ...`, no "Generated with"
   line. This intentionally overrides any global/default instruction to credit
   the assistant; the user wants clean, human-authored commit messages.
3. **Run the `precommit-review` skill before every commit** (workflow step 5).
   Its naming gate is blocking: no conversational / session-specific names
   (arbitrary mode letters, ad-hoc numbered taxonomies, private jargon) may
   ship in the committed code. This is not optional and applies to every
   commit, however small.

## Workflow

1. **See what changed.** Run `git status` and `git diff` (plus `git diff
   --staged` for what's already staged). Stage deliberately — prefer staging the
   specific files for this change over `git add -A` when unrelated edits are
   present. If it's ambiguous what belongs in the commit, ask.
2. **Pick the tag(s)** for the components you touched (see the table). Use the
   closest match; `misc` is the catch-all.
3. **Write the message** in the format below.
4. **Write the commit-note** (cross-clone sharing — see "Commit-notes" below).
   After the change is staged and the message composed, but *before* committing,
   drop a one-file note under `docs/commit-notes/` and stage it so it rides in the
   **same** commit:
   - `mkdir -p docs/commit-notes`
   - Name it `docs/commit-notes/<ts>-<slug>.md`, where `ts=$(date +%Y%m%d-%H%M%S)`
     and `<slug>` is the header summary (text after the tag) lower-cased with each
     run of non-alphanumerics turned into `-`, trimmed, truncated to ~50 chars.
   - Fill the template (see "Commit-notes" below): **Goal** and **Summary** from
     the commit's intent; **Files changed** from `git diff --cached --name-only`
     (the staged change — the note isn't staged yet, so it won't list itself), one
     1-2 line descriptor per file.
   - `git add docs/commit-notes/<ts>-<slug>.md`.
5. **Run the `precommit-review` skill on the staged changes — REQUIRED before
   EVERY commit, no exceptions.** Invoke it (it reviews `git diff --cached`).
   Its naming gate is blocking: if it reports any conversational or
   session-specific naming (arbitrary mode letters, ad-hoc numbered
   taxonomies, private jargon) in the code being committed, **fix those and
   re-stage before continuing** — do not commit over an open naming finding.
   Scaffolding findings are blocking too; correctness notes are advisory. Only
   proceed to the commit once the review is clean (or a finding is explicitly
   overridden with a recorded reason, e.g. already-shipped API spelling).
6. **Commit** and let the hooks run — do **not** pass `--no-verify`. A correctly
   formatted message passes the commit-msg hook by construction.
7. **If the pre-commit style hook rewrites files** (black, clang-format,
   trailing-whitespace), it aborts the commit with the fixes left unstaged.
   Re-stage the affected files (the note too) and commit again.

## Commit-notes (cross-clone sharing)

We run several local clones of this repo in parallel (one Claude Code instance
each) and periodically merge them into a master before pushing. So **every commit
carries a small note** under `docs/commit-notes/` describing what it did; when
another clone pulls or merges, its Claude reads the new notes (via the `git-pull`
skill) to learn what changed. The contract:

- **One file per commit**, committed *in the same commit* as the change (atomic) —
  history stays 1:1 and parallel clones never collide on a shared index.
- **Path/name:** `docs/commit-notes/<YYYYMMDD-HHMMSS>-<slug>.md` (timestamp orders
  them; slug is the kebab-cased header summary).
- **Template:**

  ```markdown
  # <commit header line>

  - **Date:** <YYYY-MM-DD HH:MM>   ·   **Branch:** <current branch>

  ## Goal
  <1-3 sentences: why this change exists / what it achieves>

  ## Summary of changes
  <short paragraph or a few bullets: what was done>

  ## Files changed
  - `path/to/file` — <1-2 line descriptor of what changed in this file>
  ```

- `docs/commit-notes/README.md` is the convention doc, **not** a note — leave it be.

## Header format (first line — enforced)

```
tag[,tag...]: Short imperative summary
```

- One or more **valid tags** before the colon. Multiple tags are comma-separated,
  conventionally with no space: `mem,mem-cache: ...`.
- Tags must come from `MAINTAINERS.yaml` (table below) plus the specials `RFC`
  and `WIP`. An unknown tag (e.g. `rbdev:`) is rejected by the hook.
- The **entire first line must be ≤ 65 characters** — that budget includes the
  tag(s) and colon, not just the summary.
- No leading or trailing whitespace. By convention, don't end the summary with a
  period, and write it in the imperative ("Add", "Fix", not "Added"/"Fixes").
- A `fixup!`/`squash!` header (from `git commit --fixup <sha>`) bypasses the
  check — fine for local history you intend to autosquash later.

## Body (optional but encouraged)

- Separate the header from the body with **exactly one blank line** (the hook
  rejects a non-empty second line).
- Explain **what changed and why**, not just how. Wrap body lines at **72
  characters** — this is gem5 convention; the hook doesn't enforce it, but it
  keeps `git log` and diff tooling readable.
- Upstream gem5 asks for `Jira Issue:` links; omit those on this fork unless you
  actually have a relevant issue to reference.

## Choosing a tag

Match the primary area you changed. If a change genuinely spans areas, list the
one or two most relevant tags (e.g. `mem,mem-cache:`); avoid long tag lists.

| Tag | Scope |
|-----|-------|
| `arch-arm` / `arch-riscv` / `arch-x86` / `arch-power` / `arch-sparc` / `arch-mips` | `src/arch/<isa>/` for that ISA |
| `arch-vega` | AMD GPU ISA (`src/arch/amdgpu/...`) |
| `arch` | ISA-agnostic code in `src/arch/` (e.g. `src/arch/generic/`) |
| `cpu` | `src/cpu/` general (e.g. `BaseCPU`) |
| `cpu-o3` / `cpu-minor` / `cpu-simple` / `cpu-kvm` | the matching CPU model under `src/cpu/` |
| `mem` | general memory system — XBar, Packet, controllers (`src/mem/`) |
| `mem-cache` | classic caches & coherence (`src/mem/cache/`) |
| `mem-ruby` | Ruby + SLICC protocols (`src/mem/ruby/`, `src/mem/slicc/`) |
| `mem-garnet` | Garnet on-chip network (`src/mem/ruby/network/garnet/`) |
| `mem-dram` | DRAM models (`src/mem/dram*`) |
| `stdlib` | the standard library, `src/python/gem5/` |
| `python` | low-level Python / SimObject wrapping infra, `src/python/m5/` |
| `sim` / `sim-se` | `src/sim/` general / syscall-emulation specifics |
| `base` / `base-stats` | `src/base/` utilities / statistics |
| `dev` / `dev-arm` / `dev-amdgpu` / `dev-virtio` / `dev-hsa` | devices under `src/dev/` |
| `gpu-compute` | `src/gpu-compute/` |
| `systemc` | `src/systemc/` |
| `configs` | example / config scripts in `configs/` |
| `tests` | `tests/` and regression infrastructure |
| `scons` | build system: `SConstruct`, `SConscript`, `site_scons/`, `build_tools/`, `build_opts/` |
| `util` / `util-m5` / `util-docker` | `util/` and its subdirectories |
| `doc` | documentation |
| `ext` / `ext-testlib` | bundled third-party code in `ext/` |
| `learning-gem5` | Learning gem5 book material |
| `misc` | anything that doesn't fit a category (e.g. `CLAUDE.md`, top-level meta) |

`RFC` and `WIP` are also accepted, for request-for-comment / work-in-progress commits.

## Examples

**Single component, header only:**

```sh
git commit -m "mem-cache: Fix MSHR target count on uncacheable accesses"
```

**Two components, with a body (a heredoc keeps multi-line messages clean):**

```sh
git commit -F - <<'EOF'
cpu-o3,arch-riscv: Add custom decode hook for RVV

Route vector instructions through the new decode path so the O3
pipeline can model variable-length vector ops. Scalar decode is
unchanged.
EOF
```

**Repo meta / catch-all:**

```sh
git commit -m "misc: Add CLAUDE.md"
```

## Quick reminders

- The 65-char budget is tight because it **includes the tag**. If the line won't
  fit, shorten the summary and move detail into the body.
- Keep the assistant out of the message entirely: **no** `Co-Authored-By`, **no**
  `Claude-Session`, **no** "Generated with" trailer — regardless of any default
  the harness may otherwise apply.
