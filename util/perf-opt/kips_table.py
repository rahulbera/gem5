#!/usr/bin/env python3
"""KIPS comparison table and verdict for one perf-opt trial (markdown).

KIPS is the region hostInstRate from stats.txt divided by 1000. Each
checkpoint is compared with the mean of the --prev trial dirs (the last
accepted configuration; pass both baseline runs for the first trial) and
with the mean of the --base trial dirs. The run-to-run spread of the two
baseline runs is the noise figure. Stats identity is checked against the
first --base dir with stats_gate.

Verdict rule: accepted iff stats are identical on every checkpoint AND a
majority of checkpoints are faster than the previous configuration.

  kips_table.py --trial T --prev P [--prev P2] --base A --base B [--exclude F]
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import stats_gate  # noqa: E402


def kips(stats_path):
    for line in Path(stats_path).read_text().splitlines():
        toks = line.split()
        if toks and toks[0] == "hostInstRate":
            return float(toks[1]) / 1000.0
    raise ValueError(f"no hostInstRate in {stats_path}")


def _mean_kips(dirs, cid):
    vals = [kips(Path(d) / cid / "stats.txt") for d in dirs]
    return sum(vals) / len(vals)


def rows(trial, prev_dirs, base_dirs, exclude=frozenset()):
    out = []
    for cid in stats_gate.trial_checkpoints(trial):
        new = kips(Path(trial) / cid / "stats.txt")
        prev = _mean_kips(prev_dirs, cid)
        base = _mean_kips(base_dirs, cid)
        noise = None
        if len(base_dirs) >= 2:
            vals = [kips(Path(d) / cid / "stats.txt") for d in base_dirs]
            noise = 100.0 * (max(vals) - min(vals)) / base
        change = 100.0 * (new / prev - 1.0)
        if new > prev:
            direction = "faster"
        elif new < prev:
            direction = "slower"
        else:
            direction = "unchanged"
        if noise is not None and new != prev and abs(change) <= noise:
            direction += " (within noise)"
        identical = not stats_gate.diff(
            stats_gate.load(Path(base_dirs[0]) / cid / "stats.txt"),
            stats_gate.load(Path(trial) / cid / "stats.txt"),
            exclude,
        )
        out.append({
            "cid": cid, "prev": prev, "new": new, "change_pct": change,
            "direction": direction, "vs_base_pct": 100.0 * (new / base - 1.0),
            "noise_pct": noise, "identical": identical,
        })
    return out


def verdict(table_rows):
    n = len(table_rows)
    need = n // 2 + 1
    identical = sum(1 for r in table_rows if r["identical"])
    faster = sum(1 for r in table_rows if r["new"] > r["prev"])
    if identical < n:
        return False, f"stats identical on only {identical}/{n}"
    if faster < need:
        return False, f"stats identical on {n}/{n}, but faster on only {faster}/{n}"
    return True, f"stats identical on {n}/{n}; faster on {faster}/{n}"


def markdown(table_rows):
    lines = [
        "| Checkpoint | Previous KIPS | New KIPS | Change | Direction "
        "| Cumulative vs baseline | Baseline noise | Stats identical |",
        "|---|---|---|---|---|---|---|---|",
    ]
    for r in table_rows:
        noise = "n/a" if r["noise_pct"] is None else f"{r['noise_pct']:.2f}%"
        lines.append(
            f"| {r['cid']} | {r['prev']:.1f} | {r['new']:.1f} "
            f"| {r['change_pct']:+.2f}% | {r['direction']} "
            f"| {r['vs_base_pct']:+.2f}% | {noise} "
            f"| {'yes' if r['identical'] else 'NO'} |"
        )
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--trial", required=True)
    ap.add_argument("--prev", action="append", required=True)
    ap.add_argument("--base", action="append", required=True)
    ap.add_argument("--exclude", default=None)
    args = ap.parse_args()
    table = rows(args.trial, args.prev, args.base,
                 stats_gate.load_exclusions(args.exclude))
    print(markdown(table))
    ok, reason = verdict(table)
    print(f"\nSuggested verdict: {'ACCEPT' if ok else 'REJECT'} ({reason})")


if __name__ == "__main__":
    main()
