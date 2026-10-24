#!/usr/bin/env bash
set -euo pipefail

# The work that threads repeat, with lists expanded whole or in chunks, and
# with affine or uniformly random orders: the counting builds of WF-DO, on
# LiveJournal and the hub graph (graph_helpers/hubs_generate.cpp) from vertex 0
# at 16 and then 112 threads over both sockets, RUNS runs each (3 by default),
# the builds taking turns. BUILDS chooses the builds, by default b256, whole
# and uniform: bN, ../work/wf-dir-count.cpp with chunks of N neighbors;
# whole, wf-dir-count-whole.cpp, which expands every list whole; and uniform,
# wf-dir-count-uniform.cpp, with chunks of 256 and uniformly random orders
# instead of affine ones. GRAPHS chooses the graphs. For each run and
# thread count: the time of a search, and per search the neighbors examined in
# top-down levels, the share of them that helpers examined, the share examined
# again, beyond the degrees of the vertices of those levels, the share of the
# blocks scanned in bottom-up levels that were scanned again, and the share of
# the expansions of vertices beyond one per vertex. Counting slows the builds, so their times are only a guide. Rows go to
# chunk-counts.csv, or OUT.csv, and the same counts by top-down level and by
# degree class to OUT-levels.csv:
#   setsid nohup ./chunk-counts.sh > chunk-counts.log 2>&1 &

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$here/../../benchmark/config.sh"
runs=${RUNS:-3}
build="$here/chunk-build"
out="$here/${OUT:-chunk-counts}.csv"
levels="$here/${OUT:-chunk-counts}-levels.csv"
read -r -a builds <<< "${BUILDS:-b256 whole uniform}"
read -r -a graphs <<< "${GRAPHS:-lj hubs}"

# sets build_source and options, what a build compiles
build_options() {
    case $1 in
        whole) build_source="$here/wf-dir-count-whole.cpp"; options=() ;;
        uniform) build_source="$here/wf-dir-count-uniform.cpp"; options=() ;;
        b*) [[ ${1#b} =~ ^[0-9]+$ ]] || { echo "unknown build $1" >&2; return 1; }
            build_source="$here/../work/wf-dir-count.cpp"; options=(-DHUB_CHUNK="${1#b}") ;;
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

echo "graph,build,run,threads,us,edges,helper_share,repeated_share,bu_repeated_share,vertex_repeated_share,correct" > "$out"
echo "graph,build,run,threads,kind,bin,entries,examined,again,again_helping,correct" > "$levels"
for run in $(seq 1 "$runs"); do
    for graph in "${graphs[@]}"; do
        set_graph "$graph"
        for i in "${!builds[@]}"; do
            name=${builds[(i + run) % ${#builds[@]}]}
            rm -f -- wfd-out.txt
            status=0
            env BFS_SPREAD_NODES=1 numactl --interleave=all \
                "$build/$name.out" "$path" 16,112 < /dev/null > stdout.txt 2> stderr.txt || status=$?
            correct=no
            if [ "$status" = 0 ] && cmp -s wfd-out.txt "$ref" &&
               awk '$1 == 112 && $3 == "same" { ok = 1 } END { exit !ok }' stdout.txt; then
                correct=yes
            fi
            # a thread count's "help" line follows its searches, in the order of
            # the counts, and its "levels" and "degrees" lines follow that. Every
            # bottom-up level scans each block at least once
            awk -v graph="$graph" -v name="$name" -v run="$run" -v correct="$correct" -v levels="$levels" '
                FNR == NR { if (NF == 3 && $1 ~ /^[0-9]+$/) { count[++counts] = $1; us[$1] = $2 }; next }
                $1 == "help" {
                    delete c
                    for (f = 2; f < NF; f += 2) c[$f] = $(f + 1)
                    e = c["e00"] + c["e01"] + c["e10"] + c["e11"]
                    b = c["b00"] + c["b01"] + c["b10"] + c["b11"]
                    expanded = c["v00"] + c["v01"] + c["v10"] + c["v11"]
                    once = c["blocks"] * c["bu_levels"]
                    t = count[++seen]
                    printf "%s,%s,%d,%d,%d,%d,%.4f,%.4f,%.4f,%.4f,%s\n", graph, name, run, t, us[t], e,
                           (e > 0 ? (c["e01"] + c["e11"]) / e : 0), (e > 0 ? c["repeated_edges"] / e : 0),
                           (b > 0 ? (b - once) / b : 0), (expanded > 0 ? c["repeated"] / expanded : 0), correct
                }
                $1 == "levels" || $1 == "degrees" {
                    for (f = 2; f <= NF; f++) {
                        split($f, bin, ":")
                        split(bin[2], v, ",")
                        if ($1 == "levels") printf "%s,%s,%d,%d,level,%d,%d,%d,%d,%d,%s\n", graph, name, run, t, bin[1], v[1], v[2], v[3], v[4], correct >> levels
                        else printf "%s,%s,%d,%d,degree,%d,0,%d,%d,0,%s\n", graph, name, run, t, bin[1], v[1], v[2], correct >> levels
                    }
                }' stdout.txt stderr.txt >> "$out"
            echo "$(date +%T) $graph $name run $run: $(tail -n 2 "$out" | cut -d, -f4-11 | tr '\n' ' ')"
            if [ "$correct" != yes ]; then
                echo "    exit status $status"
                tail -n 5 stderr.txt | sed 's/^/    /'
            fi
        done
    done
done
echo "done $(date '+%F %T')"
