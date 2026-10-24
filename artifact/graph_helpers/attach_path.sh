#!/usr/bin/env bash
set -euo pipefail

# Writes a graph with a path attached: the graph of an "N M" file, the format
# graph_reader.hpp reads, with ids from FIRST_ID, relabeled from 0; then LENGTH
# new vertices N to N + LENGTH - 1 joined in a path, the first of them joined to
# vertex AT (numbered from 0). A search that reaches AT at level d goes on for
# LENGTH more levels of one vertex each. The output is in the same format, with
# ids from 0, and is written under another name first, so a run that stops
# partway leaves no file under the final one:
#   ./attach_path.sh <input> <FIRST_ID> <AT> <LENGTH> <output>

if [ "$#" -ne 5 ]; then
    echo "Usage: $0 <input> <first id of input, 0 or 1> <vertex to attach to> <path length> <output>" >&2
    exit 1
fi
input=$1 first=$2 at=$3 length=$4 output=$5
[[ "$first" =~ ^[01]$ ]] || { echo "the first id must be 0 or 1" >&2; exit 1; }
[[ "$at" =~ ^[0-9]+$ ]] || { echo "the vertex to attach to must be a whole number" >&2; exit 1; }
[[ "$length" =~ ^[1-9][0-9]*$ ]] || { echo "the path length must be a positive whole number" >&2; exit 1; }
[ -f "$input" ] || { echo "missing $input" >&2; exit 1; }

read -r n m < "$input"
[[ "$n" =~ ^[0-9]+$ && "$m" =~ ^[0-9]+$ ]] || { echo "$input does not start with an \"N M\" line" >&2; exit 1; }
if [ "$at" -ge "$n" ]; then
    echo "vertex $at is not one of the $n vertices of $input" >&2
    exit 1
fi
# labels are ints in the programs that read the graph
if [ $((n + length)) -gt 2147483647 ]; then
    echo "$((n + length)) vertices do not fit in an int" >&2
    exit 1
fi

awk -v first="$first" -v at="$at" -v len="$length" -v n="$n" -v m="$m" '
    NR == 1 { printf "%d %d\n", n + len, m + len; next }
    { printf "%d %d\n", $1 - first, $2 - first }
    END {
        prev = at
        for (i = 0; i < len; i++) {
            v = n + i
            printf "%d %d\n", (prev < v ? prev : v), (prev < v ? v : prev)
            prev = v
        }
    }' "$input" > "$output.partial"
lines=$(wc -l < "$output.partial")
if [ "$lines" -ne $((m + length + 1)) ]; then
    echo "wrote $lines lines, expected $((m + length + 1))" >&2
    rm -f -- "$output.partial"
    exit 1
fi
command mv -f -- "$output.partial" "$output"
echo "Wrote $output: $((n + length)) vertices, $((m + length)) edges, a path of $length joined to vertex $at"
