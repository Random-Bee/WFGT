#!/usr/bin/env bash
set -euo pipefail

# The experiments with threads stalled at random times and at chosen points:
# LiveJournal, or the graph GRAPH names (twitter, road, uk, kron or hubs), from
# vertex 0 on THREADS threads
# spread over the NUMA nodes, with memory interleaved, in the stall builds of
# the programs (the *-stalls.cpp files here, with stall.hpp). THREADS is seven eighths of the CPUs unless set, 112 of the 128
# of the paper's machine, which leaves the rest to the host and to the signals
# that stall the workers. Four experiments, and runs with
# no stalls to compare them with:
#   length  every thread stalled for 2% of its time, in stalls of 10 to 10000 us
#   share   every thread stalled for 0.5% to 50% of its time, in stalls of 1000 us
#   cas     our programs only, stalled right after a discovery's winning
#           compare-and-swap, in stalls of 10 to 10000 us
#   hub     the same, halfway through the neighbors of a vertex with at least
#           1024 of them
# On Road-USA, whose 6,262 levels each wait for stalled threads in the programs
# that wait, stalls last at most 1000 us and shares reach 5%, and hub does not
# run, since no vertex there has 1024 neighbors.
# In length and share, each thread on its own runs for an exponentially
# distributed time and is then stopped by a signal wherever it is, so stalls
# fall anywhere in the code, as often as time is spent there; a thread stalled
# for a share s of its time in stalls of d us runs d (1 - s) / s us between
# them on average. In cas and hub, a thread stalls at each point of the kind
# with the probability that gives a search as many stalls as a share of 2%
# does at random times, so that the stalls differ only in where they fall:
# with T threads, a search of t us takes T t 0.02 / d of them, and passing n
# points it stalls at each with probability T t 0.02 / (d n). For each of our
# programs, t and n are the medians of three runs without stalls, kept in
# results/noise-both-points.csv (noise-<graph>-points.csv for another graph,
# and so for the other files). A thread draws a random number at each point, so
# cas and hub also run with stalls of 0 us at a probability of one per
# billion, which costs what the points themselves cost.
# Each program is built once, with the benchmark's flags and its code unshifted.
# A run is 20 searches, and every setting runs RUNS times (5 by default) for
# each program; the programs take turns, so drift reaches them all alike. A run
# is correct when its distances match a sequential BFS's. Rows go to
# results/noise-both.csv, and per setting the median of the runs to
# results/noise-both-summary.csv; for our programs, last_us is when the last thread
# returned. A stopped run picks up where it left off, as a run already correct
# is not run again. It runs for about 20 minutes:
#   setsid nohup ./noise.sh > noise.log 2>&1 &
# The compiler, where the graphs and their reference distances are, the lock
# and GAPBS's spin count come from config.sh.

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$here/../../benchmark/config.sh"
runs=${RUNS:-5}
threads=${THREADS:-$((cpus - cpus / 8))}

case ${GRAPH:-lj} in
    lj) suffix=both ;;
    twitter|road|uk|kron|hubs) suffix=$GRAPH ;;
    *) echo "unknown graph ${GRAPH}" >&2; exit 1 ;;
esac
set_graph "${GRAPH:-lj}"
graph=$path
build="$here/build"
results="$here/results"
out="$results/noise-$suffix.csv"
summary="$results/noise-$suffix-summary.csv"
points="$results/noise-$suffix-points.csv"
programs=(wd mem gapbs gbbs pasgal)
ours=(wd mem)
# the share of each thread's time stalled for cas and hub, per mille
target_share=20

lengths="10 100 1000 10000"
shares="5 10 50 100 200 500"
cas_lengths="0 10 100 1000 10000"
hub_lengths=$cas_lengths
if [ "${GRAPH:-lj}" = road ]; then
    lengths="10 100 1000"
    shares="5 10 50"
    cas_lengths="0 10 100 1000"
    hub_lengths=""
fi

# a setting a line: experiment, microseconds per stall, share per mille
settings=$(
    echo "none 0 0"
    for us in $lengths; do echo "length $us 20"; done
    for share in $shares; do echo "share 1000 $share"; done
    for us in $cas_lengths; do echo "cas $us $target_share"; done
    for us in $hub_lengths; do echo "hub $us $target_share"; done
)

[ -f "$graph" ] || { echo "missing $graph (BFS_DATASETS in config.sh)" >&2; exit 1; }
[ -f "$ref" ] || { echo "missing $ref: make-refs.sh ${GRAPH:-lj} writes it" >&2; exit 1; }

declare -A done_run
if [ -f "$out" ]; then
    while IFS=, read -r experiment us share ppb run name time last correct finished; do
        if [ "$correct" = yes ]; then
            done_run["$experiment,$us,$share,$run,$name"]=1
        fi
    done < <(tail -n +2 "$out")
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
mkdir -p "$TMPDIR" "$build/noise-run" "$results"
flags=(-std=c++23 -O3 -march=native -DNDEBUG -g -fno-omit-frame-pointer
       -falign-loops=32 -falign-jumps=32 -include "$root/benchmark/text-shift.h"
       -DTEXT_SHIFT=0)
parlay=(-mcx16 -pthread)
build_program() {
    local binary="$build/noise-$1.out"
    case $1 in
        wd) "$cxx" "${flags[@]}" -pthread "$here/wf-dir-stalls.cpp" -o "$binary" ;;
        mem) "$cxx" "${flags[@]}" -pthread "$here/wf-dir-mem-stalls.cpp" -o "$binary" ;;
        gapbs) "$cxx" "${flags[@]}" -fopenmp "$here/dir-bfs-stalls.cpp" -o "$binary" ;;
        pasgal) "$cxx" "${flags[@]}" "${parlay[@]}" \
                -I "$root/submodules/PASGAL/external/parlaylib/include" \
                "$here/pasgal-bfs-stalls.cpp" \
                -o "$binary" ;;
        gbbs) "$cxx" "${flags[@]}" "${parlay[@]}" -DPARLAY_USE_STD_ALLOC \
                -I "$root/submodules/parlaylib/include" "$here/gbbs-bfs-stalls.cpp" \
                -o "$binary" ;;
    esac
}
pids=()
for name in "${programs[@]}"; do
    build_program "$name" 2> "$TMPDIR/noise-build-$name.log" &
    pids+=($!)
done
for i in "${!pids[@]}"; do
    if ! wait "${pids[i]}"; then
        cat -- "$TMPDIR/noise-build-${programs[i]}.log" >&2
        exit 1
    fi
done
echo "built ${#programs[@]} programs"

cd "$build/noise-run"
cleanup() {
    rm -f -- wfd-out.txt dir-out.txt gbbs-out.txt pasgal-out.txt stdout.txt stderr.txt
}
trap cleanup EXIT

output_of() {
    case $1 in
        gapbs) echo dir-out.txt ;;
        gbbs) echo gbbs-out.txt ;;
        pasgal) echo pasgal-out.txt ;;
        *) echo wfd-out.txt ;;
    esac
}

# runs a program with the given settings (NAME=value words) into stdout.txt
# and stderr.txt; status is its exit status, time and correct what it gave
run_program() {
    local name=$1
    shift
    local output
    output=$(output_of "$name")
    rm -f -- "$output"
    status=0
    local settings=("$@")
    [ "$name" != gapbs ] || settings+=(GOMP_SPINCOUNT="$gapbs_spincount")
    env BFS_SPREAD_NODES=1 numactl --interleave=all env "${settings[@]}" \
        "$build/noise-$name.out" "$graph" "$threads" < /dev/null > stdout.txt 2> stderr.txt ||
        status=$?
    # for one thread count a program prints just its time
    time=$(awk 'NF == 1 && $1 ~ /^[0-9]+$/ { t = $1 } END { print t }' stdout.txt)
    correct=no
    if [ "$status" = 0 ] && [ -n "$time" ] && cmp -s "$output" "$ref"; then
        correct=yes
    fi
}

# for each of our programs, the median time of a search without stalls and
# the median points of each kind it passes
declare -A base_us passed_cas passed_hub
if [ ! -f "$points" ]; then
    echo "program,us,cas,hub" > "$points.part"
    for name in "${ours[@]}"; do
        : > "$TMPDIR/noise-points-$name.txt"
        for attempt in 1 2 3; do
            run_program "$name" BFS_NOISE_US=0
            if [ "$correct" != yes ]; then
                echo "calibration run of $name failed, exit status $status" >&2
                tail -n 5 stderr.txt >&2
                exit 1
            fi
            awk -v t="$time" '$1 == "passed" && $2 == "per" { print t, $5, $7 }' stderr.txt \
                >> "$TMPDIR/noise-points-$name.txt"
        done
        awk -v name="$name" '
            { t[NR] = $1; c[NR] = $2; h[NR] = $3 }
            END {
                if (NR != 3) exit 1
                asort(t); asort(c); asort(h)
                printf "%s,%d,%d,%d\n", name, t[2], c[2], h[2]
            }' "$TMPDIR/noise-points-$name.txt" >> "$points.part"
    done
    command mv -f -- "$points.part" "$points"
fi
while IFS=, read -r name us cas hub; do
    base_us[$name]=$us
    passed_cas[$name]=$cas
    passed_hub[$name]=$hub
done < <(tail -n +2 "$points")
echo "without stalls, per search:"
for name in "${ours[@]}"; do
    echo "    $name ${base_us[$name]} us, ${passed_cas[$name]} cas and" \
         "${passed_hub[$name]} hub points"
done

# the probability per billion of a stall at a point, for cas and hub
probability() {
    local name=$1 kind=$2 us=$3 share=$4 passed
    if [ "$kind" = cas ]; then passed=${passed_cas[$name]}; else passed=${passed_hub[$name]}; fi
    awk -v threads="$threads" -v t="${base_us[$name]}" -v share="$share" -v d="$us" \
        -v n="$passed" 'BEGIN {
            if (n <= 0) { print 0; exit }
            p = 1e9 * threads * t * share / 1000 / d / n
            printf "%d\n", (p > 1e9 ? 1e9 : p + 0.5)
        }'
}

if [ ! -f "$out" ]; then
    echo "experiment,stall_us,share_permille,ppb,run,program,us,last_us,correct,finished" > "$out"
fi

for run in $(seq 1 "$runs"); do
    while read -r experiment us share; do
        case $experiment in
            cas | hub) names=("${ours[@]}") ;;
            *) names=("${programs[@]}") ;;
        esac
        # a different program starts each run's settings
        for i in "${!names[@]}"; do
            name=${names[(i + run) % ${#names[@]}]}
            key="$experiment,$us,$share,$run,$name"
            if [ -n "${done_run[$key]:-}" ]; then
                continue
            fi
            ppb=0
            case $experiment in
                none) settings_now=(BFS_NOISE_US=0) ;;
                length | share)
                    settings_now=(BFS_NOISE_US="$us"
                                  BFS_NOISE_GAP_US=$((us * (1000 - share) / share))
                                  BFS_NOISE_SEED="$run") ;;
                cas | hub)
                    ppb=1
                    if [ "$us" != 0 ]; then
                        ppb=$(probability "$name" "$experiment" "$us" "$share")
                    fi
                    settings_now=(BFS_TARGET="$experiment" BFS_TARGET_US="$us"
                                  BFS_TARGET_PPB="$ppb" BFS_NOISE_SEED="$run") ;;
            esac
            run_program "$name" "${settings_now[@]}"
            last=$(awk '$1 == "last" && $2 == "return" && $3 ~ /^[0-9]+$/ { t = $3 } END { print t }' \
                stderr.txt)
            echo "$experiment,$us,$share,$ppb,$run,$name,${time:-failed},$last,$correct,$(date +%FT%T)" \
                >> "$out"
            echo "$(date +%T) $key: ${time:-failed} us${last:+, last return $last us}, correct $correct"
            if [ "$correct" != yes ]; then
                echo "    exit status $status"
                tail -n 5 stderr.txt | sed 's/^/    /'
            fi
        done
    done <<< "$settings"
done

# per setting and program, the median, fastest and slowest of the correct runs
# (a rerun's latest), the median last return for ours, and how many runs
{
    echo "experiment,stall_us,share_permille,program,median_us,min_us,max_us,median_last_us,runs"
    awk -F, 'NR > 1 && $9 == "yes" { key = $1 "," $2 "," $3 "," $6
                                     t[key SUBSEP $5] = $7; l[key SUBSEP $5] = $8 }
             function median(v, k) { return (k % 2) ? v[(k + 1) / 2] : (v[k / 2] + v[k / 2 + 1]) / 2 }
             END { for (ks in t) { split(ks, p, SUBSEP)
                                   times[p[1]] = times[p[1]] " " t[ks]
                                   if (l[ks] != "") lasts[p[1]] = lasts[p[1]] " " l[ks] }
                   for (key in times) {
                       k = split(times[key], v, " "); asort(v)
                       m = split(lasts[key], w, " "); if (m) asort(w)
                       printf "%s,%d,%d,%d,%s,%d\n", key, median(v, k), v[1], v[k],
                              m ? sprintf("%d", median(w, m)) : "", k
                   } }' "$out" | sort -t, -k1,1 -k4,4 -k2,2n -k3,3n
} > "$summary"
cat -- "$summary"
echo "finished $(date '+%F %T')"
