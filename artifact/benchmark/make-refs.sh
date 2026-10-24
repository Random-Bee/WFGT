#!/usr/bin/env bash
set -euo pipefail

# Writes the distances that the sequential BFS (DO_BFS/dir-bfs-seq.cpp) finds
# for each graph named, the files that benchmark.sh and the experiments
# check every run against, into BFS_REFS (see config.sh). A graph whose file is
# there already is left as it is.
#   ./make-refs.sh lj twitter road uk kron

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$here/config.sh"
if [ "$#" -eq 0 ]; then
    echo "Usage: $0 <graph>..., e.g. $0 lj twitter road uk kron" >&2
    exit 1
fi
for graph in "$@"; do
    set_graph "$graph" || { echo "unknown graph $graph" >&2; exit 1; }
    [ -f "$path" ] || { echo "missing $path (BFS_DATASETS in config.sh)" >&2; exit 1; }
done

# the searches would disturb a benchmark that is running
mkdir -p "$(dirname "$lock")"
exec 9> "$lock"
if ! flock -n 9; then
    echo "waiting for $lock, held by another benchmark"
    flock 9
fi

build="$here/build"
# GCC's intermediate files go here rather than in /tmp
export TMPDIR="$build/tmp"
mkdir -p "$TMPDIR" "$build/refs-run" "$refs"
"$cxx" -std=c++23 -O3 -march=native -DNDEBUG -pthread "$root/DO_BFS/dir-bfs-seq.cpp" \
    -o "$build/refs-seq.out"

cd "$build/refs-run"
for graph in "$@"; do
    set_graph "$graph"
    if [ -f "$ref" ]; then
        echo "$graph: $ref is there already"
        continue
    fi
    rm -f -- dir-seq-out.txt
    "$build/refs-seq.out" "$path" > /dev/null
    command mv -f -- dir-seq-out.txt "$ref"
    echo "$graph: wrote $ref"
done
