# misc: Document the perf-opt build recipes in CLAUDE.md

- **Date:** 2026-09-27 14:22   ·   **Branch:** feat/opt

## Goal
Make CLAUDE.md say how to build gem5 now that the perf-opt campaign has
settled the recipe, and record the no-AI-trailers rule.

## Summary of changes
- New "Recommended builds (perf-opt recipe)" section: gem5.opt for debugging,
  gem5.fast + LTO + tcmalloc + memset flag for development (1.55x), plus PGO
  for bulk runs (1.83x); the util/perf-opt/build.sh commands, the PGO
  steps, the non-sticky flags, and the kratos2 build script and binaries.
- The build callout and the Anaconda section now point to it instead of
  presenting build-gem5.sh as the only way to build.
- Commit conventions: never add AI trailers to commits or PRs.

## Files changed
- `CLAUDE.md` — recommended builds section, build callout, no-AI-trailers rule.
