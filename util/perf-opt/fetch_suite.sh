#!/usr/bin/env bash
# Copy the perf-opt suite (suite.json) and the shared SPEC26 restore resources
# from kratos2 into the local layout run_suite.sh expects, then verify that
# every checkpoint arrived with the same file count and byte size.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
LOCAL_ROOT=/home/rbera/work/tracezoo/gem5
REMOTE_ROOT=/home/rahbera/tracezoo/gem5/fs_ckpts/spec26
mkdir -p "$LOCAL_ROOT/fs_ckpts" "$LOCAL_ROOT/restore_resources"

# Test checkpoints first, then the shared resources every run needs, then the
# training checkpoints (needed only for the PGO trial). --partial keeps a
# half-copied file across an interruption, so a rerun resumes it.
rels() {
    python3 -c '
import json, sys
for c in json.load(open(sys.argv[1]))["checkpoints"]:
    if c["role"] == sys.argv[2]:
        print(c["remote"].split("/ckpts/", 1)[1])
' "$HERE/suite.json" "$1"
}
mapfile -t TEST_RELS < <(rels test)
mapfile -t TRAIN_RELS < <(rels train)
RELS=("${TEST_RELS[@]}" "${TRAIN_RELS[@]}")

copy_checkpoints() {
    for rel in "$@"; do
        echo ">> $rel"
        rsync -a --partial --relative "kratos2:$REMOTE_ROOT/ckpts/./$rel" \
            "$LOCAL_ROOT/fs_ckpts/"
    done
}

copy_checkpoints "${TEST_RELS[@]}"
echo ">> restore_resources (sparse)"
rsync -aSz --partial "kratos2:$REMOTE_ROOT/restore_resources/" \
    "$LOCAL_ROOT/restore_resources/"
copy_checkpoints "${TRAIN_RELS[@]}"

echo ">> verify"
fail=0
for rel in "${RELS[@]}"; do
    remote=$(ssh -o BatchMode=yes kratos2 \
        "cd $REMOTE_ROOT/ckpts/$rel && echo \$(ls | wc -l) \$(find . -type f -printf '%s\\n' | awk '{s+=\$1} END {print s}')")
    local=$(cd "$LOCAL_ROOT/fs_ckpts/$rel" && echo "$(ls | wc -l)" \
        "$(find . -type f -printf '%s\n' | awk '{s+=$1} END {print s}')")
    if [ "$remote" = "$local" ]; then echo "ok   $rel ($local)"; else echo "BAD  $rel remote=[$remote] local=[$local]"; fail=1; fi
done
exit $fail
