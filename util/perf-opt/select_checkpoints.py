#!/usr/bin/env python3
"""Pick the perf-opt test suite and PGO training set from the SPEC26 manifest.

Test checkpoints are random.Random(seed).sample(rows, n_test). The same
generator then draws the training checkpoints from the remaining rows, so
the two sets never overlap and the whole draw is reproducible from the seed.
"""
import argparse
import json
import random
from pathlib import Path

REMOTE = "kratos2:/home/rahbera/tracezoo/gem5/fs_ckpts/spec26/ckpts"
LOCAL = "/home/rbera/work/tracezoo/gem5/fs_ckpts"


def checkpoint_id(row):
    return f"{row['benchmark']}.{row['inv']}.{row['simpoint']}"


def rel_path(row):
    b, i, s = row["benchmark"], row["inv"], row["simpoint"]
    return f"{b}/{i}/cpt.{b}.{i}.{s}"


def select(rows, seed, n_test, n_train):
    rng = random.Random(seed)
    test = rng.sample(rows, n_test)
    test_ids = {checkpoint_id(r) for r in test}
    remaining = [r for r in rows if checkpoint_id(r) not in test_ids]
    train = rng.sample(remaining, n_train)
    return test, train


def entry(row, role):
    return {
        "id": checkpoint_id(row),
        "role": role,
        "benchmark": row["benchmark"],
        "inv": row["inv"],
        "simpoint": row["simpoint"],
        "weight": row["weight"],
        "remote": f"{REMOTE}/{rel_path(row)}",
        "local": f"{LOCAL}/{rel_path(row)}",
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("manifest", type=Path)
    ap.add_argument("--seed", type=int, default=20260926)
    ap.add_argument("--n-test", type=int, default=5)
    ap.add_argument("--n-train", type=int, default=3)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    rows = json.loads(args.manifest.read_text())
    test, train = select(rows, args.seed, args.n_test, args.n_train)
    suite = {
        "seed": args.seed,
        "manifest": str(args.manifest),
        "checkpoints": [entry(r, "test") for r in test]
        + [entry(r, "train") for r in train],
    }
    args.out.write_text(json.dumps(suite, indent=1) + "\n")
    for c in suite["checkpoints"]:
        print(c["role"], c["id"])


if __name__ == "__main__":
    main()
