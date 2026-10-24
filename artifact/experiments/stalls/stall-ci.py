#!/usr/bin/env python3
"""Confidence intervals for the runs of stalls.sh that stall one worker per
level or round, from one of its CSV files:
    python3 stall-ci.py results/stalls-twitter.csv

The unit is a run, the mean of the 20 searches of one process; each setting has
five. Per program and stall length, it prints the mean time per search of the
runs, in ms, with its 95% Student t interval, and the difference from the runs
without stalls, in ms and as a share of their mean, with its 95% interval by
Welch's method; "excludes 0" marks a difference whose interval does not reach
0. Also the median over the runs of when the last thread returned, for our
programs, which report it.

Only the Python standard library is needed: the t quantiles come from
integrating the density of the t distribution.
"""
import argparse
import collections
import csv
import math
import statistics


def t_density(x, df):
    log_scale = math.lgamma((df + 1) / 2) - math.lgamma(df / 2) - 0.5 * math.log(df * math.pi)
    return math.exp(log_scale - (df + 1) / 2 * math.log1p(x * x / df))


def t_cdf(x, df, steps=2000):
    """P(T <= x) for x >= 0, by Simpson's rule on the density from 0 to x"""
    h = x / steps
    total = t_density(0.0, df) + t_density(x, df)
    for i in range(1, steps):
        total += (4 if i % 2 else 2) * t_density(i * h, df)
    return 0.5 + total * h / 3


def t_quantile(p, df):
    """the x with P(T <= x) = p, for 0.5 < p < 1, by bisection"""
    low, high = 0.0, 1.0
    while t_cdf(high, df) < p:
        high *= 2
    for _ in range(60):
        middle = (low + high) / 2
        if t_cdf(middle, df) < p:
            low = middle
        else:
            high = middle
    return (low + high) / 2


def mean_interval(values):
    mean = statistics.mean(values)
    half = t_quantile(0.975, len(values) - 1) * statistics.stdev(values) / math.sqrt(len(values))
    return mean, mean - half, mean + half


def welch_interval(stalled, base):
    """the difference of the means with Welch's 95% interval"""
    va = statistics.variance(stalled) / len(stalled)
    vb = statistics.variance(base) / len(base)
    df = (va + vb) ** 2 / (va ** 2 / (len(stalled) - 1) + vb ** 2 / (len(base) - 1))
    difference = statistics.mean(stalled) - statistics.mean(base)
    half = t_quantile(0.975, df) * math.sqrt(va + vb)
    return difference, difference - half, difference + half


def read_runs(path):
    """(program, stall in us) -> {run: ms per search}, and the same for last returns"""
    runs = collections.defaultdict(dict)
    lasts = collections.defaultdict(dict)
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            if row["experiment"] != "length" or row["stalled"] != "1" or row["correct"] != "yes":
                continue
            key = (row["program"], int(row["stall_us"]))
            runs[key][row["run"]] = int(row["us"]) / 1000
            if row["last_us"]:
                lasts[key][row["run"]] = int(row["last_us"]) / 1000
    return runs, lasts


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("csv", help="a CSV file that stalls.sh wrote, such as results/stalls-twitter.csv")
    args = parser.parse_args()
    runs, lasts = read_runs(args.csv)
    for program in sorted({p for p, _ in runs}):
        print(program)
        base = list(runs[(program, 0)].values())
        base_mean = statistics.mean(base)
        for stall in sorted(u for p, u in runs if p == program):
            values = list(runs[(program, stall)].values())
            mean, low, high = mean_interval(values)
            line = f"  {stall:>5} us: mean {mean:7.1f} ({low:.1f} to {high:.1f}), n={len(values)}"
            if stall > 0:
                d, d_low, d_high = welch_interval(values, base)
                excludes = "excludes 0" if d_low > 0 or d_high < 0 else "includes 0"
                line += (f"; difference {d:.2f} ({d_low:.2f} to {d_high:.2f}) ms,"
                         f" {d / base_mean * 100:.1f}% ({d_low / base_mean * 100:.1f} to"
                         f" {d_high / base_mean * 100:.1f}%), {excludes}")
            if lasts[(program, stall)]:
                line += f"; last return, median {statistics.median(lasts[(program, stall)].values()):.1f}"
            print(line)


if __name__ == "__main__":
    main()
