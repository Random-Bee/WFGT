#!/usr/bin/env bash
set -euo pipefail

# where the random stalls of noise.sh cost WF-DO time: its build
# there (build/noise-wd.out, from wf-dir-stalls.cpp) on LiveJournal, or the
# graph GRAPH names (twitter, road, uk, kron or hubs), from vertex 0 with 112
# threads over both sockets, without stalls, with every thread stalled at random
# for 2% of its time in stalls of 1 and 10 ms, and stalled as often in stalls of
# 10 ms right after a winning CAS, at the probability noise.sh used; on
# Road-USA, as in noise.sh, the stalls are ten times shorter. RUNS runs
# each (3 by default). For each run: the mean search, its reset, which waits
# for every thread, the search after it, and when the last thread returned.
# Rows go to diag-noise.csv, or diag-noise-<graph>.csv for another graph:
#   setsid nohup ./diag-noise.sh > diag-noise.log 2>&1 &

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$here/../../benchmark/config.sh"
binary="$here/build/noise-wd.out"
graph_name=${GRAPH:-lj}
case $graph_name in
    lj) suffix=both ;;
    twitter|road|uk|kron|hubs) suffix=$graph_name ;;
    *) echo "unknown graph $graph_name" >&2; exit 1 ;;
esac
set_graph "$graph_name"
graph=$path
runs=${RUNS:-3}
out="$here/diag-noise.csv"
[ "$graph_name" = lj ] || out="$here/diag-noise-$graph_name.csv"
short_us=1000
long_us=10000
if [ "$graph_name" = road ]; then
    short_us=100
    long_us=1000
fi
# the probability per billion of a stall at a CAS that noise.sh gave wd in its
# runs on the graph, so this runs after them
noise="$here/results/noise-$suffix.csv"
cas_ppb=$(awk -F, -v us="$long_us" '$1 == "cas" && $2 == us && $6 == "wd" { p = $4 } END { print p }' "$noise" 2>/dev/null || true)
[ -n "$cas_ppb" ] || { echo "no stall probability at a CAS for wd in $noise" >&2; exit 1; }

for file in "$binary" "$graph" "$ref"; do
    [ -f "$file" ] || { echo "missing $file" >&2; exit 1; }
done

mkdir -p "$(dirname "$lock")"
exec 9> "$lock"
if ! flock -n 9; then
    echo "waiting for $lock, held by another benchmark"
    flock 9
fi
echo "started $(date '+%F %T') on $(hostname)"

work=$(mktemp -d "$here/diag-noise.XXXXXX")
cleanup() {
    rm -f -- "$work/wfd-out.txt" "$work/stdout.txt" "$work/stderr.txt"
    rmdir -- "$work" 2>/dev/null || true
}
trap cleanup EXIT
cd "$work"

echo "setting,run,mean_us,reset_us,search_us,last_us,correct" > "$out"
for run in $(seq 1 "$runs"); do
    for setting in none "length-$short_us" "length-$long_us" "cas-$long_us"; do
        # a share of 2%: a stall of d us after 49 d us of running, on average
        case $setting in
            none) run_env=(BFS_NOISE_US=0) ;;
            length-*) us=${setting#length-}
                      run_env=(BFS_NOISE_US="$us" BFS_NOISE_GAP_US="$((49 * us))" BFS_NOISE_SEED="$run") ;;
            cas-*) run_env=(BFS_TARGET=cas BFS_TARGET_US="${setting#cas-}" BFS_TARGET_PPB="$cas_ppb"
                            BFS_NOISE_SEED="$run") ;;
        esac
        rm -f -- wfd-out.txt
        status=0
        env BFS_SPREAD_NODES=1 numactl --interleave=all env "${run_env[@]}" \
            "$binary" "$graph" 112 < /dev/null > stdout.txt 2> stderr.txt || status=$?
        correct=no
        if [ "$status" = 0 ] && cmp -s wfd-out.txt "$ref"; then
            correct=yes
        fi
        awk -v setting="$setting" -v run="$run" -v correct="$correct" '
            FNR == NR { if (NF == 1 && $1 ~ /^[0-9]+$/) mean = $1; next }
            $1 == "last" && $2 == "return" { last = $3 }
            $1 == "phases" { reset = $3; search = $5 }
            END { printf "%s,%d,%s,%s,%s,%s,%s\n", setting, run, mean, reset, search, last, correct }' \
            stdout.txt stderr.txt >> "$out"
        echo "$(date +%T) $(tail -n 1 "$out")"
    done
done
echo "done $(date '+%F %T')"
