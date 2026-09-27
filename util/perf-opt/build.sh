#!/usr/bin/env bash
# Build one perf-opt configuration with the campaign toolchain (g++-12, system
# Python, no conda on PATH) and record exactly how it was built.
#
# Usage: [CCFLAGS_EXTRA=...] [LINKFLAGS_EXTRA=...] build.sh <dir> <opt|fast> [scons args...]
# Builds build/<dir>/gem5.<variant>. A new <dir> is configured from
# build_opts/ARM first. CCFLAGS_EXTRA and LINKFLAGS_EXTRA are not sticky in
# SCons, so pass the same values on every call for a given <dir>.
set -euo pipefail
DIR="$1"
VARIANT="$2"
shift 2
HERE=$(cd "$(dirname "$0")" && pwd)
GEM5=$(cd "$HERE/../.." && pwd)
cd "$GEM5"

export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
unset LD_LIBRARY_PATH PYTHONPATH PYTHONHOME CONDA_PREFIX
export CC=gcc-12 CXX=g++-12
export CCFLAGS_EXTRA="${CCFLAGS_EXTRA:-}" LINKFLAGS_EXTRA="${LINKFLAGS_EXTRA:-}"
JOBS="${JOBS:-$(nproc)}"
TARGET="build/$DIR/gem5.$VARIANT"

[ -d "build/$DIR" ] || scons defconfig "build/$DIR" build_opts/ARM

start=$(date +%s)
scons "$TARGET" -j"$JOBS" --ignore-style "$@"
# A fresh configure can finish a pass without linking; a second pass links.
[ -x "$TARGET" ] || scons "$TARGET" -j"$JOBS" --ignore-style "$@"
end=$(date +%s)

{
    echo "date: $(date -Is)"
    echo "git: $(git rev-parse HEAD)"
    echo "command: CC=$CC CXX=$CXX CCFLAGS_EXTRA='$CCFLAGS_EXTRA'" \
        "LINKFLAGS_EXTRA='$LINKFLAGS_EXTRA' scons $TARGET -j$JOBS" \
        "--ignore-style $*"
    echo "compiler: $($CXX --version | head -1)"
    echo "elapsed_s: $((end - start))"
    echo "binary_bytes: $(stat -c %s "$TARGET")"
    echo "text_bytes: $(size -A "$TARGET" | awk '$1 == ".text" {print $2}')"
    echo "tcmalloc: $(ldd "$TARGET" | grep -o 'libtcmalloc[^ ]*' || echo none)"
} | tee "build/$DIR/perf-opt-build-$VARIANT.txt"
