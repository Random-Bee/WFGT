#!/usr/bin/env bash
set -euo pipefail

# The total work of a search, against GAPBS and the sequential baseline, from
# vertex 0 over both sockets: wf-dir-count.cpp, WF-DO with counters (wd), at
# 16, 64 and 112 threads in one process, RUNS runs (3 by default);
# wf-dir-count-stalls.cpp, the same with stalls, at 112 threads, one of them
# stalled STALL_US microseconds (10000 by default) at the start of every level
# (wdstall), RUNS runs, on the graphs of STALL_GRAPHS; GAPBS
# (dir-bfs-count.cpp) at 16, 64 and 112 threads, and the sequential baseline
# (dir-bfs-seq-count.cpp), one run each, since their work does not change from
# run to run. For each run and thread count, per search: the vertices that
# top-down levels expand and the neighbors they examine, the vertices that
# bottom-up levels read, those of them still undiscovered, and the neighbors
# they examine; for wd also the bucket positions read in top-down levels and
# in the passes that write codes, the entries of nodes visited, and the
# neighbors examined again: those of top-down levels beyond their degrees.
# GRAPHS chooses the graphs (LiveJournal, Twitter, Road-USA, uk-2002, Kronecker
# and the graph of hubs by default), and STALL_GRAPHS those of the stalled runs
# (the same by default), none if it is empty. On Road-USA the stalled thread
# stalls for at most 1000 us, since it pays a stall at each of 6,262 levels and
# every search waits for it to return.
# WD_VARIANT=exact counts wf-dir-count-exact.cpp instead, whose direction rule
# counts each vertex of a frontier once, and has no stalled runs. Our rows are
# named WD_NAME and WD_NAMEstall (wd and wdstall by default), and BASELINES=0
# leaves out GAPBS and the sequential baseline. Rows go to work-counts.csv, or OUT.csv:
#   setsid nohup ./work-counts.sh > work-counts.log 2>&1 &

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$here/../../benchmark/config.sh"
runs=${RUNS:-3}
stall_us=${STALL_US:-10000}
build="$here/work-build"
out="$here/${OUT:-work-counts}.csv"
read -r -a graphs <<< "${GRAPHS:-lj twitter road uk kron hubs}"
read -r -a stall_graphs <<< "${STALL_GRAPHS-lj twitter road uk kron hubs}"
case ${WD_VARIANT:-} in
    "") wd_source="$here/wf-dir-count.cpp"; wd_options=(-DHUB_CHUNK=256) ;;
    exact)
        wd_source="$here/wf-dir-count-exact.cpp"; wd_options=()
        if [ "${#stall_graphs[@]}" -gt 0 ]; then
            echo "WD_VARIANT=exact has no stalled runs; set STALL_GRAPHS to nothing" >&2
            exit 1
        fi ;;
    *) echo "unknown WD_VARIANT $WD_VARIANT" >&2; exit 1 ;;
esac
wd_name=${WD_NAME:-wd}
baselines=${BASELINES:-1}

for graph in "${graphs[@]}" "${stall_graphs[@]}"; do
    set_graph "$graph" || { echo "unknown graph $graph" >&2; exit 1; }
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
       -falign-loops=32 -falign-jumps=32 -include "$root/benchmark/text-shift.h" -DTEXT_SHIFT=0)
"$cxx" "${flags[@]}" "${wd_options[@]}" -pthread "$wd_source" -o "$build/wd.out"
if [ "${#stall_graphs[@]}" -gt 0 ]; then
    "$cxx" "${flags[@]}" -pthread "$here/wf-dir-count-stalls.cpp" -o "$build/wdstall.out"
fi
"$cxx" "${flags[@]}" -fopenmp "$here/dir-bfs-count.cpp" -o "$build/gapbs.out"
"$cxx" "${flags[@]}" -pthread "$here/dir-bfs-seq-count.cpp" -o "$build/seq.out"
echo "built our counting builds and the baselines'"

cd "$build/run"
cleanup() {
    rm -f -- wfd-out.txt dir-out.txt dir-seq-out.txt stdout.txt stderr.txt
}
trap cleanup EXIT

fields="td_vertices,td_edges,bu_vertices,bu_candidates,bu_edges,positions,code_positions,entries,repeated_edges"
echo "graph,program,run,threads,stall_us,us,$fields,correct" > "$out"

# a program's "work" lines (and for ours the "help" lines before them) follow
# its searches, in the order of the thread counts
record() {
    local graph=$1 name=$2 run=$3 stall=$4 correct=$5
    awk -v graph="$graph" -v name="$name" -v run="$run" -v stall="$stall" -v correct="$correct" '
        FNR == NR {
            if (NF == 3 && $1 ~ /^[0-9]+$/) { count[++counts] = $1; us[counts] = $2 }
            else if (NF == 1 && $1 ~ /^[0-9]+$/) single = $1
            next
        }
        $1 == "help" { for (f = 2; f < NF; f += 2) if ($f == "repeated_edges") rep[++helps] = $(f + 1) }
        $1 == "work" {
            delete c
            for (f = 2; f < NF; f += 2) c[$f] = $(f + 1)
            k = ++works
            t = counts ? count[k] : threads_single
            time = counts ? us[k] : single
            printf "%s,%s,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%s,%s\n", graph, name, run, t, stall, time,
                   c["td_vertices"], c["td_edges"], c["bu_vertices"], c["bu_candidates"], c["bu_edges"],
                   c["positions"], c["code_positions"], c["entries"], (k in rep ? rep[k] : ""), correct
        }' threads_single="$6" stdout.txt stderr.txt >> "$out"
}

check() {
    local output=$1 expect=$2
    [ "$status" = 0 ] && cmp -s "$output" "$ref" &&
        awk -v expect="$expect" 'NF == 3 && $1 ~ /^[0-9]+$/ { n++; if ($3 == "same") same++ }
                                 END { exit !(n == expect && same == expect) }' stdout.txt
}

for graph in "${graphs[@]}"; do
    set_graph "$graph"
    if [ "$baselines" = 1 ]; then
        status=0
        "$build/seq.out" "$path" < /dev/null > stdout.txt 2> stderr.txt || status=$?
        correct=no
        if [ "$status" = 0 ] && cmp -s dir-seq-out.txt "$ref"; then correct=yes; fi
        record "$graph" seq 1 0 "$correct" 1
        echo "$(date +%T) $graph seq: $(tail -n 1 "$out")"

        status=0
        env GOMP_SPINCOUNT=300000 BFS_SPREAD_NODES=1 numactl --interleave=all \
            "$build/gapbs.out" "$path" 16,64,112 < /dev/null > stdout.txt 2> stderr.txt || status=$?
        correct=no
        if check dir-out.txt 3; then correct=yes; fi
        record "$graph" gapbs 1 0 "$correct" 0
        echo "$(date +%T) $graph gapbs: correct $correct"
    fi

    for run in $(seq 1 "$runs"); do
        rm -f -- wfd-out.txt
        status=0
        env BFS_SPREAD_NODES=1 numactl --interleave=all \
            "$build/wd.out" "$path" 16,64,112 < /dev/null > stdout.txt 2> stderr.txt || status=$?
        correct=no
        if check wfd-out.txt 3; then correct=yes; fi
        record "$graph" "$wd_name" "$run" 0 "$correct" 0
        echo "$(date +%T) $graph $wd_name run $run: correct $correct"
    done
done

for graph in "${stall_graphs[@]}"; do
    set_graph "$graph"
    us=$stall_us
    [ "$graph" != road ] || us=$(( stall_us < 1000 ? stall_us : 1000 ))
    for run in $(seq 1 "$runs"); do
        rm -f -- wfd-out.txt
        status=0
        env BFS_SPREAD_NODES=1 numactl --interleave=all env BFS_STALLED=1 BFS_STALL_US="$us" \
            "$build/wdstall.out" "$path" 112 < /dev/null > stdout.txt 2> stderr.txt || status=$?
        correct=no
        if [ "$status" = 0 ] && cmp -s wfd-out.txt "$ref"; then correct=yes; fi
        record "$graph" "${wd_name}stall" "$run" "$us" "$correct" 112
        echo "$(date +%T) $graph ${wd_name}stall run $run: $(tail -n 1 "$out")"
    done
done
echo "done $(date '+%F %T')"
