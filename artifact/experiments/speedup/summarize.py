#!/usr/bin/env python3
"""Splits the gap between GAPBS and WF-DO from the runs of steps.sh.

GAPBS (logs/<graph>/gapbs-steps-r<round>.err): per thread count and round, its
time per search split into its setup and bookkeeping (allocating and filling
dist, clearing the bitmaps, turning the queue into a bitmap and back, fixing
dist at the end) and its traversal steps, each traversal step split into the
mean time before a thread starts it, on its share, and waiting at the barrier
that ends it.

WF-DO (logs/<graph>/wd-steps-r<round>.err): per thread count and round, the
reset, the search, and an average thread's time per search getting plans,
writing frontier codes, on its own share and helping.

The gap between the two splits into what helping recovers, the starts and
waits of the traversal steps of GAPBS less how much longer the levels of WF-DO
take than the threads of GAPBS spend working, and the rest, the setup and
bookkeeping of GAPBS beyond the reset and frontier codes of WF-DO. Each split
uses the round of each program whose total is the median, so that one round
slowed by load does not skew it; the splits go to speedup-summary.csv.

All times are microseconds per search.
"""
import argparse
import collections
import csv
import pathlib
import re
import statistics

HERE = pathlib.Path(__file__).resolve().parent
GRAPHS = ("lj", "twitter", "road", "uk", "kron", "hubs")

TRAVERSAL = ("td", "bu")
STEP_FIELDS = ("wall", "fork", "busy", "busy_max", "wait", "wait_min")

GAPBS_HEADER = re.compile(r"^steps threads (\d+) searches \d+ total ([\d.]+) median_total ([\d.]+)")
GAPBS_STEP = re.compile(r"^step (\d+) (\S+) wall ([\d.-]+) fork ([\d.-]+) busy ([\d.]+) "
                        r"busy_max ([\d.]+) wait ([\d.-]+) wait_min ([\d.-]+)")
GAPBS_KIND = re.compile(r"^kind (\S+) count (\d+) wall ([\d.-]+) fork ([\d.-]+) busy ([\d.]+) "
                        r"busy_max ([\d.]+) wait ([\d.-]+) wait_min ([\d.-]+)")
GAPBS_OTHER = re.compile(r"^kind other wall ([\d.-]+)")
WD_HEADER = re.compile(r"^levels at (\d+) threads, us per search: reset ([\d.]+), search ([\d.]+)")
WD_LEVEL = re.compile(r"^\s+(\d+)\s+(td|bu)\s+([\d.]+) \|\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)")


def parse_gapbs(path):
    """thread count -> {'total', 'kinds': kind -> fields, 'steps': [(kind, fields)]}"""
    runs = {}
    current = None
    for line in path.read_text().splitlines():
        if m := GAPBS_HEADER.match(line):
            current = {"total": float(m[2]), "kinds": {}, "steps": []}
            runs[int(m[1])] = current
        elif current is None:
            continue
        elif m := GAPBS_STEP.match(line):
            current["steps"].append((m[2], dict(zip(STEP_FIELDS, map(float, m.groups()[2:])))))
        elif m := GAPBS_KIND.match(line):
            fields = dict(zip(STEP_FIELDS, map(float, m.groups()[2:])))
            fields["count"] = int(m[2])
            current["kinds"][m[1]] = fields
        elif m := GAPBS_OTHER.match(line):
            fields = dict.fromkeys(STEP_FIELDS, 0.0)
            fields.update(wall=float(m[1]), count=1)
            current["kinds"]["other"] = fields
    return runs


def parse_wd(path):
    """thread count -> {'reset', 'search', 'levels': [(depth, dir, span, plan, codes, own, help)]}"""
    runs = {}
    current = None
    for line in path.read_text().splitlines():
        if m := WD_HEADER.match(line):
            current = {"reset": float(m[2]), "search": float(m[3]), "levels": []}
            runs[int(m[1])] = current
        elif current is not None and (m := WD_LEVEL.match(line)):
            current["levels"].append((int(m[1]), m[2], *map(float, m.groups()[2:])))
    return runs


def gapbs_row(run):
    kinds = run["kinds"]

    def total(names, field):
        return sum(kinds[k][field] for k in names if k in kinds)

    row = collections.OrderedDict()
    row["total"] = run["total"]
    traversal = [k for k in kinds if k in TRAVERSAL]
    others = [k for k in kinds if k not in TRAVERSAL]
    row["setup_book"] = total(others, "wall")
    row["trav"] = total(traversal, "wall")
    row["trav_fork"] = total(traversal, "fork")
    row["trav_busy"] = total(traversal, "busy")
    row["trav_wait"] = total(traversal, "wait")
    for k in TRAVERSAL:
        row[f"{k}_steps"] = kinds.get(k, {}).get("count", 0)
        for field in ("wall", "fork", "busy", "busy_max", "wait"):
            row[f"{k}_{field}"] = kinds.get(k, {}).get(field, 0.0)
    for k in others:
        row[k] = kinds[k]["wall"]
    return row


def wd_row(run, total):
    row = collections.OrderedDict()
    # without the time the program printed, the reset and the search, which add up to it
    row["total"] = total if total is not None else run["reset"] + run["search"]
    row["reset"] = run["reset"]
    row["search"] = run["search"]
    for direction in ("td", "bu"):
        of_direction = [lv for lv in run["levels"] if lv[1] == direction]
        row[direction + "_levels"] = len(of_direction)
        row[direction + "_span"] = sum(lv[2] for lv in of_direction)
    for i, name in enumerate(("plan", "codes", "own", "help")):
        row[name] = sum(lv[3 + i] for lv in run["levels"])
    return row


def printed_times(csv_path):
    """(graph, program, round, threads) -> us per search of the runs with the right distances"""
    times = {}
    if not csv_path.exists():
        return times
    with open(csv_path, newline="") as f:
        for row in csv.DictReader(f):
            if row["correct"] == "yes":
                key = (row["graph"], row["program"], int(row["round"]), int(row["threads"]))
                times[key] = float(row["us_per_search"])
    return times


def round_of(path):
    return int(re.search(r"-r(\d+)\.err$", path.name)[1])


def median_round(rows):
    return sorted(rows, key=lambda r: r["total"])[len(rows) // 2]


def split(g, w):
    gap = g["total"] - w["total"]
    # how much longer the levels of WF-DO, without their frontier codes, take
    # than the threads of GAPBS spend working in its traversal steps
    beyond = (w["search"] - w["codes"]) - g["trav_busy"]
    # helping stands in for the starts and waits of the traversal steps only
    recovered = g["trav_fork"] + g["trav_wait"] - beyond
    return {"gap": gap, "beyond": beyond, "recovered": recovered, "bookkeeping": gap - recovered}


def print_table(title, rows_by_threads):
    print(title)
    for threads in sorted(rows_by_threads):
        rows = rows_by_threads[threads]
        print(f"  {threads} threads, {len(rows)} rounds (mean, then each round)")
        keys = list(dict.fromkeys(k for r in rows for k in r))
        for key in keys:
            values = [r.get(key, 0.0) for r in rows]
            each = " ".join(f"{v:10.1f}" for v in values)
            print(f"    {key:24s} {statistics.mean(values):10.1f} | {each}")


def print_top_waits(runs_by_threads, top=3):
    """the steps with the longest mean wait, averaged over rounds by position"""
    print("GAPBS steps with the longest waits (mean over rounds): wall fork busy busy_max wait")
    for threads in sorted(runs_by_threads):
        runs = runs_by_threads[threads]
        sequences = [[kind for kind, _ in run["steps"]] for run in runs]
        if any(s != sequences[0] for s in sequences):
            print(f"  {threads} threads: the rounds took different steps")
            continue
        averaged = []
        for i, kind in enumerate(sequences[0]):
            fields = {f: statistics.mean(run["steps"][i][1][f] for run in runs) for f in STEP_FIELDS}
            averaged.append((i, kind, fields))
        averaged.sort(key=lambda s: s[2]["wait"], reverse=True)
        print(f"  {threads} threads, {len(sequences[0])} steps")
        for i, kind, f in averaged[:top]:
            print(f"    step {i:5d} {kind:22s} " + " ".join(
                f"{f[x]:10.1f}" for x in ("wall", "fork", "busy", "busy_max", "wait")))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--logs", default=str(HERE / "logs"), help="the step reports, a directory per graph")
    parser.add_argument("--csv", default=str(HERE / "steps.csv"), help="the times that the programs printed")
    parser.add_argument("--out", default=str(HERE / "speedup-summary.csv"), help="where the splits go")
    parser.add_argument("--details", action="store_true", help="also print every field per round")
    args = parser.parse_args()
    logs = pathlib.Path(args.logs)
    times = printed_times(pathlib.Path(args.csv))

    summary = []
    for graph in [g for g in GRAPHS if (logs / g).is_dir()]:
        gapbs = collections.defaultdict(list)
        gapbs_runs = collections.defaultdict(list)
        wd = collections.defaultdict(list)
        for path in sorted((logs / graph).glob("gapbs-steps-r*.err")):
            for threads, run in parse_gapbs(path).items():
                gapbs[threads].append(gapbs_row(run))
                gapbs_runs[threads].append(run)
        for path in sorted((logs / graph).glob("wd-steps-r*.err")):
            for threads, run in parse_wd(path).items():
                total = times.get((graph, "wd-steps", round_of(path), threads))
                wd[threads].append(wd_row(run, total))

        print(f"=== {graph}")
        for threads in sorted(set(gapbs) & set(wd)):
            g, w = median_round(gapbs[threads]), median_round(wd[threads])
            s = split(g, w)
            share = (lambda x: 100 * x / s["gap"]) if s["gap"] else (lambda x: float("nan"))
            print(f"  {threads} threads, median rounds: GAPBS {g['total']:.1f}, WF-DO {w['total']:.1f},"
                  f" gap {s['gap']:.1f}")
            print(f"    GAPBS: setup and bookkeeping {g['setup_book']:.1f}, traversal {g['trav']:.1f}"
                  f" (starts {g['trav_fork']:.1f}, working {g['trav_busy']:.1f}, waiting {g['trav_wait']:.1f})")
            print(f"    WF-DO: reset {w['reset']:.1f}, codes {w['codes']:.1f},"
                  f" levels without codes {w['search'] - w['codes']:.1f}"
                  f" ({s['beyond']:.1f} beyond the working time of GAPBS)")
            print(f"    recovered by helping {s['recovered']:.1f} ({share(s['recovered']):.0f}% of the gap),"
                  f" setup and bookkeeping {s['bookkeeping']:.1f} ({share(s['bookkeeping']):.0f}%)")
            summary.append({
                "graph": graph, "threads": threads,
                "rounds": min(len(gapbs[threads]), len(wd[threads])),
                "gapbs_total": g["total"], "gapbs_setup_book": g["setup_book"],
                "gapbs_trav_start": g["trav_fork"], "gapbs_trav_work": g["trav_busy"],
                "gapbs_trav_wait": g["trav_wait"],
                "wd_total": w["total"], "wd_reset": w["reset"], "wd_codes": w["codes"],
                "wd_levels": w["search"] - w["codes"], "wd_own": w["own"], "wd_help": w["help"],
                "gap": s["gap"], "beyond": s["beyond"],
                "recovered": s["recovered"], "bookkeeping": s["bookkeeping"],
            })
        print()
        print_top_waits(gapbs_runs)
        if args.details:
            print_table("GAPBS", gapbs)
            print_table("WF-DO", wd)
        print()

    print("what the timers cost: mean us per search over the rounds, plain and timed builds")
    for graph in GRAPHS:
        counts = sorted({k[3] for k in times if k[0] == graph})
        for threads in counts:
            means = {}
            for program in ("gapbs", "gapbs-steps", "wd", "wd-steps"):
                values = [v for k, v in times.items() if k[0] == graph and k[1] == program and k[3] == threads]
                means[program] = statistics.mean(values) if values else float("nan")
            print(f"  {graph:8s} {threads:4d}: GAPBS {means['gapbs']:10.1f} timed {means['gapbs-steps']:10.1f},"
                  f" WF-DO {means['wd']:10.1f} timed {means['wd-steps']:10.1f}")

    if summary:
        with open(args.out, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=list(summary[0]), lineterminator="\n")
            writer.writeheader()
            for row in summary:
                writer.writerow({k: (f"{v:.1f}" if isinstance(v, float) else v) for k, v in row.items()})
        print(f"\nsplits written to {args.out}")


if __name__ == "__main__":
    main()
