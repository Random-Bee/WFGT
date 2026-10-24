#!/usr/bin/env bash
set -euo pipefail

# the time of a search with lists expanded whole or in chunks, and with affine
# or uniformly random orders, on LiveJournal, the hub graph and Twitter from
# vertex 0 at 16, 64 and 112 threads in one process, RUNS runs each (3 by
# default), the builds taking turns. BUILDS (b256, whole and uniform by
# default) and GRAPHS choose others; the builds are those of chunk-counts.sh
# without counters: bN, wf-dir-chunks.cpp with chunks of N neighbors, whole,
# wf-dir-whole.cpp, and uniform, wf-dir-uniform.cpp. Rows go to
# chunk-times.csv, or OUT.csv:
#   setsid nohup ./chunk-times.sh > chunk-times.log 2>&1 &

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$here/../../benchmark/config.sh"
runs=${RUNS:-3}
build="$here/chunk-build"
out="$here/${OUT:-chunk-times}.csv"
read -r -a builds <<< "${BUILDS:-b256 whole uniform}"
read -r -a graphs <<< "${GRAPHS:-lj hubs twitter}"

# sets build_source and options, what a build compiles
build_options() {
    case $1 in
        whole) build_source="$here/wf-dir-whole.cpp"; options=() ;;
        uniform) build_source="$here/wf-dir-uniform.cpp"; options=() ;;
        b*) [[ ${1#b} =~ ^[0-9]+$ ]] || { echo "unknown build $1" >&2; return 1; }
            build_source="$here/wf-dir-chunks.cpp"; options=(-DHUB_CHUNK="${1#b}") ;;
        *) echo "unknown build $1" >&2; return 1 ;;
    esac
}

set_graph() {
    case $1 in
        lj) path="$datasets/liveJournal1.edges"; ref="$refs/seq-liveJournal1.txt" ;;
        hubs) path="$root/datasets/hubs.edges"; ref="$refs/hubs-ref.txt" ;;
        twitter) path="$datasets/twitter-2010.txt"; ref="$refs/twitter-ref.txt" ;;
        uk) path="$datasets/uk-2002.mtx"; ref="$refs/uk-2002-ref.txt" ;;
        kron) path="$datasets/kron-25.edges"; ref="$refs/kron-25-ref.txt" ;;
        road) path="$datasets/road-road-usa.mtx"; ref="$refs/road-reference.txt" ;;
        *) echo "unknown graph $1" >&2; return 1 ;;
    esac
}

for name in "${builds[@]}"; do
    build_options "$name"
done
for graph in "${graphs[@]}"; do
    set_graph "$graph"
    for file in "$path" "$ref"; do
        [ -f "$file" ] || { echo "missing $file" >&2; exit 1; }
    done
done

mkdir -p "$(dirname "$lock")"
exec 9> "$lock"
if ! flock -n 9; then
    echo "waiting for $lock, held by another benchmark"
    flock 9
fi
echo "started $(date '+%F %T') on $(hostname)"

export TMPDIR="$build/tmp"
mkdir -p "$TMPDIR" "$build/run"
flags=(-std=c++23 -O3 -march=native -DNDEBUG -g -fno-omit-frame-pointer
       -falign-loops=32 -falign-jumps=32 -include "$root/benchmark/text-shift.h"
       -DTEXT_SHIFT=0 -pthread)
for name in "${builds[@]}"; do
    build_options "$name"
    "$cxx" "${flags[@]}" "${options[@]}" "$build_source" -o "$build/$name.out"
done
echo "built ${#builds[@]} programs"

cd "$build/run"
cleanup() {
    rm -f -- wfd-out.txt stdout.txt stderr.txt
}
trap cleanup EXIT

echo "graph,build,run,threads,us,correct" > "$out"
for run in $(seq 1 "$runs"); do
    for graph in "${graphs[@]}"; do
        set_graph "$graph"
        for i in "${!builds[@]}"; do
            name=${builds[(i + run) % ${#builds[@]}]}
            rm -f -- wfd-out.txt
            status=0
            env BFS_SPREAD_NODES=1 numactl --interleave=all \
                "$build/$name.out" "$path" 16,64,112 < /dev/null > stdout.txt 2> stderr.txt || status=$?
            correct=no
            if [ "$status" = 0 ] && cmp -s wfd-out.txt "$ref" &&
               awk '$1 == 112 && $3 == "same" { ok = 1 } END { exit !ok }' stdout.txt; then
                correct=yes
            fi
            awk -v graph="$graph" -v name="$name" -v run="$run" -v correct="$correct" \
                'NF == 3 && $1 ~ /^[0-9]+$/ { printf "%s,%s,%d,%d,%d,%s\n", graph, name, run, $1, $2, correct }' \
                stdout.txt >> "$out"
            echo "$(date +%T) $graph $name run $run: $(awk '{ printf "%s:%.1f ", $1, $2 / 1000 }' stdout.txt)correct $correct"
            if [ "$correct" != yes ]; then
                echo "    exit status $status"
                tail -n 5 stderr.txt | sed 's/^/    /'
            fi
        done
    done
done
echo "done $(date '+%F %T')"
