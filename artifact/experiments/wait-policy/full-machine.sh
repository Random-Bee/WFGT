#!/usr/bin/env bash
set -euo pipefail

# Every program when its threads take all of a socket's or all of the
# machine's cores, so that whatever else the host runs must take cores from
# them: on NUMA node 0 at 56 and then 64 threads, and spread over both nodes
# with memory interleaved, as benchmark.sh places them, at 112 and then 128.
# The programs are benchmark.sh's -O3 builds (benchmark/build, code unshifted),
# with GAPBS twice, with OpenMP's default wait policy and with
# OMP_WAIT_POLICY=active.
# Every graph, placement and program runs RUNS times (3 by default), the
# programs taking turns. Before each run the share of each node's CPU time that
# the host spent busy over a second, while none of ours ran, is recorded as the
# background load. A run that takes longer than LIMIT seconds (480 by default)
# is stopped: the thread counts it had not finished are recorded as stopped,
# and since a program writes its distances only at the end, its correctness as
# n/a. A run killed by a signal is recorded the same way, as killed. A run is
# correct when its distances match a sequential BFS's. Rows go to $RESULTS.csv
# (RESULTS is full-machine by default), per point medians to
# $RESULTS-summary.txt, and each run's output to $RESULTS/. A thread count that
# already has a row is not run again, so a stopped script picks up where it was:
#   setsid nohup ./full-machine.sh > full-machine.log 2>&1 &

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$here/../../benchmark/config.sh"
runs=${RUNS:-3}
limit=${LIMIT:-480}
read -r -a graphs <<< "${GRAPHS:-lj twitter road uk kron hubs}"
read -r -a placements <<< "${PLACEMENTS:-node0 both}"
bench="$root/benchmark/build"
results=${RESULTS:-full-machine}
dir="$here/$results"
out="$here/$results.csv"
summary="$here/$results-summary.txt"
configs=(wd mem gapbs gapbs-active gbbs pasgal)

# GAPBS runs road over both nodes only at 112 threads: at 128 one run holds
# every core of the shared host for many minutes
skipped() { [ "$1" = both ] && [ "$2" = road ] && [[ $3 == gapbs* ]] && [ "$4" = 128 ]; }

# whether the results already have a row for a placement, graph, program, run
# and thread count
have() {
    awk -F, -v p="$1" -v g="$2" -v n="$3" -v r="$4" -v t="$5" \
        '$1 == p && $2 == g && $3 == n && $4 == r && $5 == t { found = 1 } END { exit !found }' "$out"
}

set_graph() {
    case $1 in
        lj) path="$datasets/liveJournal1.edges"; ref="$refs/seq-liveJournal1.txt" ;;
        twitter) path="$datasets/twitter-2010.txt"; ref="$refs/twitter-ref.txt" ;;
        road) path="$datasets/road-road-usa.mtx"; ref="$refs/road-reference.txt" ;;
        uk) path="$datasets/uk-2002.mtx"; ref="$refs/uk-2002-ref.txt" ;;
        kron) path="$datasets/kron-25.edges"; ref="$refs/kron-25-ref.txt" ;;
        hubs) path="$root/datasets/hubs.edges"; ref="$refs/hubs-ref.txt" ;;
        *) echo "unknown graph $1" >&2; exit 1 ;;
    esac
}

# sets launcher and counts for a placement
set_placement() {
    case $1 in
        node0) launcher=(numactl --cpunodebind=0 --membind=0); counts=56,64 ;;
        both) launcher=(env BFS_SPREAD_NODES=1 numactl --interleave=all); counts=112,128 ;;
        *) echo "unknown placement $1" >&2; exit 1 ;;
    esac
}

# sets binary, output and settings for a program
set_program() {
    settings=(BFS_NOISE_US=0)
    case $1 in
        wd) binary="$bench/wd-0.out"; output=wfd-out.txt ;;
        mem) binary="$bench/mem-0.out"; output=wfd-out.txt ;;
        gapbs) binary="$bench/gapbs-0.out"; output=dir-out.txt ;;
        gapbs-active) binary="$bench/gapbs-0.out"; output=dir-out.txt; settings=(OMP_WAIT_POLICY=active) ;;
        gbbs) binary="$bench/gbbs-0.out"; output=gbbs-out.txt ;;
        pasgal) binary="$bench/pasgal-0.out"; output=pasgal-out.txt ;;
    esac
}

for graph in "${graphs[@]}"; do
    set_graph "$graph"
    for file in "$path" "$ref"; do
        [ -f "$file" ] || { echo "missing $file" >&2; exit 1; }
    done
done
for placement in "${placements[@]}"; do
    set_placement "$placement"
    for name in "${configs[@]}"; do
        set_program "$name"
        [ -f "$binary" ] || { echo "missing $binary; run benchmark.sh to build it" >&2; exit 1; }
    done
done

# the share of each node's CPU time spent busy over a second, host-wide, as
# "node0 node1" in percent; CPUs alternate between the two nodes
background() {
    local a b
    a=$(awk '/^cpu[0-9]+ /' /proc/stat)
    sleep 1
    b=$(awk '/^cpu[0-9]+ /' /proc/stat)
    paste <(echo "$a") <(echo "$b") | awk '{
        n = NF / 2; c = substr($1, 4) % 2; busy = 0; total = 0
        for (i = 2; i <= 11; i++) { d = $(i + n) - $i; total += d; if (i != 5 && i != 6) busy += d }
        B[c] += busy; T[c] += total }
        END { printf "%.1f %.1f\n", 100 * B[0] / T[0], 100 * B[1] / T[1] }'
}

mkdir -p "$(dirname "$lock")"
exec 9> "$lock"
if ! flock -n 9; then
    echo "waiting for $lock, held by another benchmark"
    flock 9
fi
echo "started $(date '+%F %T')"

mkdir -p "$dir/run"
if [ ! -f "$out" ]; then
    echo "placement,graph,program,run,threads,us,correct,limited,background_node0,background_node1,finished" > "$out"
fi
cd "$dir/run"
trap 'rm -f -- wfd-out.txt dir-out.txt gbbs-out.txt pasgal-out.txt' EXIT
turn=0
for placement in "${placements[@]}"; do
    set_placement "$placement"
    for graph in "${graphs[@]}"; do
        set_graph "$graph"
        for run in $(seq 1 "$runs"); do
            turn=$((turn + 1))
            for i in "${!configs[@]}"; do
                name=${configs[(i + turn) % ${#configs[@]}]}
                set_program "$name"
                todo=()
                for t in ${counts//,/ }; do
                    skipped "$placement" "$graph" "$name" "$t" && continue
                    have "$placement" "$graph" "$name" "$run" "$t" || todo+=("$t")
                done
                [ ${#todo[@]} -gt 0 ] || continue
                run_counts=$(printf '%s,' "${todo[@]}")
                log="$dir/$placement-$graph-$name-$run"
                read -r bg0 bg1 <<< "$(background)"
                rm -f -- "$output"
                status=0
                start=$(date +%s)
                timeout --signal=TERM "$limit" "${launcher[@]}" env "${settings[@]}" "$binary" "$path" \
                    "${run_counts%,}" < /dev/null > "$log.out" 2> "$log.err" || status=$?
                elapsed=$(( $(date +%s) - start ))
                limited=no
                note=""
                correct=no
                missing=failed
                if [ "$status" = 124 ]; then
                    limited=yes
                    correct=n/a
                    missing=stopped
                    note=" (stopped after $elapsed s)"
                elif [ "$status" -gt 128 ]; then
                    correct=n/a
                    missing=killed
                    note=" (killed by signal $((status - 128)) after $elapsed s)"
                elif [ "$status" = 0 ] && cmp -s "$output" "$ref"; then
                    correct=yes
                fi
                shown=""
                for t in "${todo[@]}"; do
                    # given one thread count, a program prints its time alone
                    us=$(awk -v t="$t" -v one="$(( ${#todo[@]} == 1 ))" \
                        'one && NF == 1 { print $1 } !one && $1 == t { print $2 }' "$log.out")
                    [ -n "$us" ] || us=$missing
                    echo "$placement,$graph,$name,$run,$t,$us,$correct,$limited,$bg0,$bg1,$(date +%FT%T)" >> "$out"
                    shown+="$t:$(awk -v us="$us" 'BEGIN { if (us ~ /^[0-9]+$/) printf "%.1f", us / 1000; else printf "%s", us }') "
                done
                echo "$(date +%T) $placement $graph $name run $run: ${shown}correct $correct$note; background $bg0% and $bg1%"
            done
        done
    done
done

# per point, the median of the runs that finished it in ms, the fastest and
# slowest of them, how many runs were stopped or killed first, and the median
# background load on each node
awk -F, 'NR > 1 { key = $1 " " $2 " " $5 " " $3; keys[key] = 1; b0[key] = b0[key] " " $9; b1[key] = b1[key] " " $10
                  if ($6 ~ /^[0-9]+$/) t[key] = t[key] " " $6; else if ($6 == "stopped") stopped[key]++
                  else if ($6 == "killed") killed[key]++; else failed[key]++ }
    function med(s,   v, k) { k = split(s, v, " "); asort(v); return (k % 2) ? v[(k + 1) / 2] : (v[k / 2] + v[k / 2 + 1]) / 2 }
    END {
        for (key in keys) {
            k = split(t[key], w, " "); asort(w)
            line = sprintf("%29s", "none finished")
            if (k) line = sprintf("%10.2f ms (%.2f-%.2f)", med(t[key]) / 1000, w[1] / 1000, w[k] / 1000)
            if (stopped[key]) line = line sprintf(", %d stopped", stopped[key])
            if (killed[key]) line = line sprintf(", %d killed", killed[key])
            if (failed[key]) line = line sprintf(", %d failed", failed[key])
            printf "%-26s %s  background %4.1f%% %4.1f%%\n", key, line, med(b0[key]), med(b1[key])
        }
    }' "$out" | sort -k1,1 -k2,2 -k3,3n -k4,4 > "$summary"
cat -- "$summary"
echo "finished $(date '+%F %T')"
