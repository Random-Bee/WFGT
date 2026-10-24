#!/usr/bin/env bash
set -euo pipefail

# The full benchmark: wf-dir.cpp and wf-dir-mem.cpp (ours), GAPBS's
# direction-optimizing BFS (DO_BFS/dir-bfs.cpp), PASGAL and GBBS, and at one
# thread DO_BFS/dir-bfs-seq.cpp as the sequential baseline. Two placements, both by default:
#   both   threads spread evenly over the NUMA nodes (BFS_SPREAD_NODES), each
#          still pinned to a CPU, and memory interleaved over all of them,
#          from 1 thread up to every CPU the benchmark may use (128 on the
#          paper's machine)
#   node0  threads and memory on NUMA node 0, from 1 thread up to every CPU
#          of the node, only when named
# A run loads its graph once and goes through every thread count of its
# placement in turn, resting a few seconds between them. Each program is
# built as the build scripts build it (C++23, loops and jump targets on
# 32-byte boundaries), all with the flags that the fastest of the other
# programs' own builds use, -O3 -march=native -DNDEBUG as GBBS's and PASGAL's
# do, and five times over, its code moved by 0 to 256 bytes in
# steps of 64 (text-shift.h): where the code lands moves a time by up to 5%,
# so each build runs once per graph and placement, and a point is the median
# of its five builds. A round runs every program once, so drift over the
# benchmark reaches them all alike. A point is correct when its distances
# match those of its run's first thread count, and the run's last distances
# match a sequential BFS's. wf-dir.cpp and wf-dir-mem.cpp are built as they
# ship: their helpers visit the owners on their own NUMA node first and then
# the rest, each group in a random order, and their top-down levels expand a
# vertex of more than 256 neighbors in chunks that its threads share.
#
# GAPBS runs with GOMP_SPINCOUNT=300000 (GAPBS_SPINCOUNT in config.sh): a
# thread waiting at a barrier spins that many times, about 3 ms by libgomp's
# reckoning, and then sleeps, which is upstream libgomp's default when
# OMP_WAIT_POLICY is unset. The system libgomp of the paper's machine spins
# not at all by default (it reports OMP_WAIT_POLICY PASSIVE
# and GOMP_SPINCOUNT 0): on liveJournal its threads slept about 32 times a
# search, and a search on 56 threads of one node took 10.6 ms against 6.4 ms
# with the spinning. OMP_WAIT_POLICY=active, which spins without end, was no
# faster there (6.5 ms) and collapsed once every CPU had a thread. Every program pins each of its
# threads to a CPU of its own (cpu_helpers/cpu_affinity.hpp), so OMP_PROC_BIND
# plays no part, and GBBS and PASGAL run with ParlayLib's defaults.
#
# Three more programs run only when PROGRAMS names them: wfbfs,
# WF_BFS/wf-bfs.cpp, our method with top-down levels only, to which wf-dir.cpp
# adds bottom-up levels; wdexact, experiments/work/wf-dir-exact.cpp, whose
# direction rule counts each vertex of a frontier once, at a CAS per bottom-up
# discovery; and wdfenced, experiments/work/wf-dir-fenced.cpp, whose marks,
# chunk certificates and block stamps are atomic exchanges.
#
# Usage: ./benchmark.sh [placements] [graphs], e.g. ./benchmark.sh node0 "lj uk";
# both placements and all five graphs by default, and only when named hubs,
# grid and ljtail, the graphs of graph_helpers/hubs_generate.cpp,
# grid_generate.cpp and attach_path.sh (datasets/hubs.edges, grid.edges and
# lj-tail.edges), or chain, a path of 10,000 vertices (datasets/chain.edges,
# "N M" and then "i i+1" for each edge); PROGRAMS="wd gapbs" limits it to
# some programs, SHIFTS="0 128 256" to some of the builds, and COUNTS="128"
# runs every program but the sequential baseline at those thread counts rather
# than its placement's. SKIP="both,road,gapbs,128" leaves out the points it
# names, each as placement,graph,program,threads, separated by spaces.
# DRY_RUN=1 lists the runs left without building or running anything. Rows go to results/benchmark.csv
# and per-point medians to results/benchmark-summary.csv, or with RESULTS=name
# to results/name.csv and results/name-summary.csv. A stopped benchmark
# picks up where it left off, as a run with every point correct is not run
# again; delete results/benchmark.csv to start over, which is needed after
# any program changes. It runs for hours, so start it detached:
#   setsid nohup ./benchmark.sh > results/benchmark.log 2>&1 &
# The compiler, where the graphs and their reference distances are, the lock
# and GAPBS's spin count come from config.sh.

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$here/config.sh"
placements=${1:-both}
graphs=${2:-lj twitter road uk kron}
dry_run=${DRY_RUN:-0}

build="$here/build"
results="$here/results"
out="$results/${RESULTS:-benchmark}.csv"
summary="$results/${RESULTS:-benchmark}-summary.csv"
read -r -a shifts <<< "${SHIFTS:-0 64 128 192 256}"
read -r -a programs <<< "${PROGRAMS:-wd mem gapbs pasgal gbbs seq}"

# the thread counts a run (placement, program, graph) goes through: the
# placement's, as many of the paper's as fit in its CPUs and then all of them,
# or COUNTS, or 1 for the sequential baseline, less any point SKIP names
thread_counts() {
    local counts t kept=()
    case $1 in
        node0) counts=$(counts_upto "$cpus_node0" 1 2 4 8 16 24 32 40 48 56 64) ;;
        both) counts=$(counts_upto "$cpus" 1 2 4 8 16 24 32 40 48 56 64 80 96 112 128) ;;
        *) return 1 ;;
    esac
    if [ "$2" = seq ]; then
        counts=1
    elif [ -n "${COUNTS:-}" ]; then
        counts=$COUNTS
    fi
    for t in $counts; do
        [[ " ${SKIP:-} " == *" $1,${3:-},$2,$t "* ]] || kept+=("$t")
    done
    echo "${kept[*]}"
}

# sets launcher, the command each run starts under
set_launcher() {
    case $1 in
        node0) launcher=(numactl --cpunodebind=0 --membind=0) ;;
        both) launcher=(env BFS_SPREAD_NODES=1 numactl --interleave=all) ;;
    esac
}

for placement in $placements; do
    thread_counts "$placement" wd > /dev/null || { echo "unknown placement $placement" >&2; exit 1; }
done
for name in "${programs[@]}"; do
    case $name in
        wd|mem|gapbs|pasgal|gbbs|seq|wfbfs|wdexact|wdfenced) ;;
        *) echo "unknown program $name" >&2; exit 1 ;;
    esac
done
for graph in $graphs; do
    set_graph "$graph" || { echo "unknown graph $graph" >&2; exit 1; }
    [ -f "$path" ] || { echo "missing $path (BFS_DATASETS in config.sh)" >&2; exit 1; }
    [ -f "$ref" ] || { echo "missing $ref: make-refs.sh $graph writes it" >&2; exit 1; }
done

# the points each run (placement, graph, build, program) has measured
# correctly so far
declare -A correct_point
if [ -f "$out" ]; then
    while IFS=, read -r placement graph threads shift name us correct finished; do
        if [ "$correct" = yes ]; then
            correct_point["$placement,$graph,$shift,$name,$threads"]=1
        fi
    done < <(tail -n +2 "$out")
fi

# a run is left while any of its thread counts lacks a correct point
pending=()
for placement in $placements; do
    for graph in $graphs; do
        for shift in "${shifts[@]}"; do
            for name in "${programs[@]}"; do
                run="$placement,$graph,$shift,$name"
                read -r -a list <<< "$(thread_counts "$placement" "$name" "$graph")"
                for threads in "${list[@]}"; do
                    if [ -z "${correct_point[$run,$threads]:-}" ]; then
                        pending+=("$run")
                        break
                    fi
                done
            done
        done
    done
done
echo "${#pending[@]} runs to go: placements $placements; graphs $graphs"
if [ "$dry_run" = 1 ]; then
    printf '%s\n' "${pending[@]}"
    exit 0
fi

mkdir -p "$(dirname "$lock")"
exec 9> "$lock"
if ! flock -n 9; then
    echo "waiting for $lock, held by another benchmark"
    flock 9
fi
echo "started $(date '+%F %T') on $(hostname), $("$cxx" --version | awk 'NR == 1')," \
     "commit $(git -C "$root" rev-parse --short HEAD 2>/dev/null || echo none)"

# GCC's intermediate files go here rather than in /tmp
export TMPDIR="$build/tmp"
mkdir -p "$TMPDIR" "$build/run"
flags=(-std=c++23 -O3 -march=native -DNDEBUG -g -fno-omit-frame-pointer
       -falign-loops=32 -falign-jumps=32 -include "$here/text-shift.h")
parlay=(-mcx16 -pthread)
build_program() {
    local name=$1 shift=$2
    local binary="$build/$name-$shift.out"
    case $name in
        wd) "$cxx" "${flags[@]}" -DTEXT_SHIFT=$shift -pthread "$root/WF_DO_BFS/wf-dir.cpp" \
                -o "$binary" ;;
        mem) "$cxx" "${flags[@]}" -DTEXT_SHIFT=$shift -pthread \
                "$root/WF_DO_BFS/wf-dir-mem.cpp" -o "$binary" ;;
        wfbfs) "$cxx" "${flags[@]}" -DTEXT_SHIFT=$shift -pthread "$root/WF_BFS/wf-bfs.cpp" \
                -o "$binary" ;;
        wdexact) "$cxx" "${flags[@]}" -DTEXT_SHIFT=$shift -pthread \
                "$root/experiments/work/wf-dir-exact.cpp" -o "$binary" ;;
        wdfenced) "$cxx" "${flags[@]}" -DTEXT_SHIFT=$shift -pthread \
                "$root/experiments/work/wf-dir-fenced.cpp" -o "$binary" ;;
        gapbs) "$cxx" "${flags[@]}" -DTEXT_SHIFT=$shift -fopenmp "$root/DO_BFS/dir-bfs.cpp" \
                -o "$binary" ;;
        pasgal) "$cxx" "${flags[@]}" -DTEXT_SHIFT=$shift "${parlay[@]}" \
                -I "$root/submodules/PASGAL/external/parlaylib/include" \
                "$root/PASGAL_BFS/pasgal-bfs.cpp" \
                -o "$binary" ;;
        gbbs) "$cxx" "${flags[@]}" -DTEXT_SHIFT=$shift "${parlay[@]}" -DPARLAY_USE_STD_ALLOC \
                -I "$root/submodules/parlaylib/include" "$root/GBBS_BFS/gbbs-bfs.cpp" \
                -o "$binary" ;;
        seq) "$cxx" "${flags[@]}" -DTEXT_SHIFT=$shift -pthread "$root/DO_BFS/dir-bfs-seq.cpp" \
                -o "$binary" ;;
    esac
}
pids=()
logs=()
for name in "${programs[@]}"; do
    for shift in "${shifts[@]}"; do
        log="$TMPDIR/build-$name-$shift.log"
        build_program "$name" "$shift" 2> "$log" &
        pids+=($!)
        logs+=("$log")
    done
done
for i in "${!pids[@]}"; do
    if ! wait "${pids[i]}"; then
        cat -- "${logs[i]}" >&2
        exit 1
    fi
done
rm -f -- "${logs[@]}"
echo "built ${#pids[@]} programs"

mkdir -p "$results"
if [ ! -f "$out" ]; then
    echo "placement,graph,threads,shift,program,us,correct,finished" > "$out"
fi
cd "$build/run"
cleanup() {
    rm -f -- wfd-out.txt dir-out.txt pasgal-out.txt gbbs-out.txt dir-seq-out.txt \
        stdout.txt stderr.txt
}
trap cleanup EXIT

for run in "${pending[@]}"; do
    IFS=, read -r placement graph shift name <<< "$run"
    set_launcher "$placement"
    set_graph "$graph"
    counts=$(thread_counts "$placement" "$name" "$graph")
    case $name in
        gapbs) output=dir-out.txt ;;
        pasgal) output=pasgal-out.txt ;;
        gbbs) output=gbbs-out.txt ;;
        seq) output=dir-seq-out.txt ;;
        *) output=wfd-out.txt ;;
    esac
    args=("$path")
    if [ "$name" != seq ]; then
        args+=("${counts// /,}")
    fi
    binary="$build/$name-$shift.out"
    settings=()
    if [ "$name" = gapbs ]; then
        settings=(GOMP_SPINCOUNT="$gapbs_spincount")
    fi
    rm -f -- "$output"
    status=0
    "${launcher[@]}" env "${settings[@]}" "$binary" "${args[@]}" > stdout.txt 2> stderr.txt ||
        status=$?
    ok=$(cmp -s "$output" "$ref" && echo yes || echo no)

    # a line per thread count, "threads us same|differs", or for one count
    # just its time; a count with no line failed
    rows=$(awk -v counts="$counts" -v ok="$ok" -v run="$run" -v when="$(date +%FT%T)" '
        NF == 1 && $1 ~ /^[0-9]+$/ { alone = $1 }
        NF == 3 && $1 ~ /^[0-9]+$/ && $2 ~ /^[0-9]+$/ { us[$1] = $2; check[$1] = $3 }
        END { split(run, r, ",")
              n = split(counts, c, " ")
              if (n == 1 && alone != "") { us[c[1]] = alone; check[c[1]] = "same" }
              for (i = 1; i <= n; i++) {
                  t = c[i]
                  time = (t in us) ? us[t] : "failed"
                  correct = (time != "failed" && check[t] == "same" && ok == "yes") ? "yes" : "no"
                  printf "%s,%s,%s,%s,%s,%s,%s,%s\n", r[1], r[2], t, r[3], r[4], time,
                         correct, when
              } }' stdout.txt)
    echo "$rows" >> "$out"
    read -r -a list <<< "$counts"
    good=$(echo "$rows" | awk -F, '$7 == "yes" { n++ } END { print n + 0 }')
    echo "$(date +%T) $run: $(echo "$rows" | awk -F, '{ printf "%s:%s ", $3, $6 }')" \
         "($good of ${#list[@]} correct)"
    if [ "$status" != 0 ] || [ "$good" -ne "${#list[@]}" ]; then
        echo "    exit status $status, output matches the reference: $ok"
        tail -n 5 stderr.txt | sed 's/^/    /'
    fi
done

# per point, the median of the builds that ran correctly (a rerun build's
# latest time), the fastest and slowest of them, how many there are (five
# when complete), and how many rows, reruns included, failed or gave wrong
# distances
{
    echo "placement,graph,threads,program,median_us,min_us,max_us,builds,failed"
    awk -F, 'NR > 1 { key = $1 "," $2 "," $3 "," $5; seen[key] = 1
                      if ($7 == "yes" && $6 ~ /^[0-9]+$/) last[key SUBSEP $4] = $6
                      else failed[key]++ }
             END { for (ks in last) { split(ks, p, SUBSEP); times[p[1]] = times[p[1]] " " last[ks] }
                   for (key in seen) {
                       k = split(times[key], v, " ")
                       if (k) { asort(v); median = (k % 2) ? v[(k + 1) / 2] : (v[k / 2] + v[k / 2 + 1]) / 2 }
                       printf "%s,%s,%s,%s,%d,%d\n", key, k ? median : "", k ? v[1] : "",
                              k ? v[k] : "", k, failed[key]
                   } }' "$out" | sort -t, -k1,1r -k2,2 -k4,4 -k3,3n
} > "$summary"
cat -- "$summary"
echo "finished $(date '+%F %T')"
