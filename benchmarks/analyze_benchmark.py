#!/usr/bin/env python3
"""Summarize benchmark CSV timings by hit/miss and URL."""

import argparse
import csv
import math
import statistics
from collections import defaultdict


def summarize(values):
    if not values:
        return {
            "count": 0,
            "mean_ms": "",
            "median_ms": "",
            "min_ms": "",
            "max_ms": "",
            "stdev_ms": "",
        }

    return {
        "count": len(values),
        "mean_ms": f"{statistics.mean(values):.3f}",
        "median_ms": f"{statistics.median(values):.3f}",
        "min_ms": f"{min(values):.3f}",
        "max_ms": f"{max(values):.3f}",
        "stdev_ms": f"{statistics.stdev(values):.3f}" if len(values) > 1 else "0.000",
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("csv_file", nargs="?", default="benchmark_results.csv")
    args = parser.parse_args()

    groups = defaultdict(list)
    failures = defaultdict(int)
    skipped = defaultdict(int)
    total_failures = 0

    with open(args.csv_file, newline="") as input_file:
        for row in csv.DictReader(input_file):
            group = (row["base_url"], row["kind"])
            if row["result"] == "ok" and row["time_ms"]:
                groups[group].append(float(row["time_ms"]))
            elif row["result"] == "skipped_after_miss_failure":
                skipped[group] += 1
            else:
                failures[group] += 1
                total_failures += 1

    print("Per-URL statistics")
    print("url,kind,count,mean_ms,median_ms,min_ms,max_ms,stdev_ms,failures,skipped")
    all_urls = (
        {url for url, _ in groups}
        | {url for url, _ in failures}
        | {url for url, _ in skipped}
    )
    for url in sorted(all_urls):
        for kind in ("miss", "hit"):
            stats = summarize(groups[(url, kind)])
            print(
                f"{url},{kind},{stats['count']},{stats['mean_ms']},"
                f"{stats['median_ms']},{stats['min_ms']},{stats['max_ms']},"
                f"{stats['stdev_ms']},{failures[(url, kind)]},{skipped[(url, kind)]}"
            )

    print("\nOverall statistics")
    for kind in ("miss", "hit"):
        stats = summarize([
            value
            for (url, group_kind), values in groups.items()
            if group_kind == kind
            for value in values
        ])
        failure_count = sum(count for (url, group_kind), count in failures.items() if group_kind == kind)
        skipped_count = sum(count for (url, group_kind), count in skipped.items() if group_kind == kind)
        print(
            f"{kind}: count={stats['count']} mean_ms={stats['mean_ms']} "
            f"median_ms={stats['median_ms']} min_ms={stats['min_ms']} "
            f"max_ms={stats['max_ms']} stdev_ms={stats['stdev_ms']} "
            f"failures={failure_count} skipped={skipped_count}"
        )

    total_skipped = sum(skipped.values())
    total_trials = sum(len(values) for values in groups.values()) + total_failures + total_skipped
    failure_rate = (total_failures / total_trials * 100) if total_trials else math.nan
    print(
        f"overall_failure_rate={failure_rate:.2f}% "
        f"({total_failures}/{total_trials}); skipped={total_skipped}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
