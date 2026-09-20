#!/usr/bin/env python3
"""Measure cold proxy misses and immediate cache hits for a URL list."""

import argparse
import csv
import subprocess
import sys
import time
from pathlib import Path


def request_url_for_pass(url, pass_number):
    separator = "&" if "?" in url else "?"
    return f"{url}{separator}proxy_bench_pass={pass_number}"


def measure(proxy, url, timeout_seconds):
    command = [
        "curl",
        "--noproxy", "",
        "--proxy", proxy,
        "--max-time", str(timeout_seconds),
        "--connect-timeout", str(timeout_seconds),
        "--silent",
        "--show-error",
        "--output", "/dev/null",
        "--write-out", "%{http_code}\t%{time_total}",
        url,
    ]

    started = time.monotonic()
    try:
        result = subprocess.run(
            command,
            capture_output=True,
            text=True,
            timeout=timeout_seconds + 2,
        )
    except subprocess.TimeoutExpired:
        return "", "", "timeout", (time.monotonic() - started) * 1000

    output = result.stdout.strip().split("\t")
    http_code = output[0] if output else ""
    seconds = output[1] if len(output) > 1 else ""
    error = result.stderr.strip().replace("\n", " ")

    if result.returncode != 0 or http_code == "000":
        reason = error or f"curl_exit_{result.returncode}"
        return http_code, seconds, reason, (time.monotonic() - started) * 1000

    return http_code, seconds, "ok", float(seconds) * 1000


def load_urls(path):
    urls = []
    for line in Path(path).read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            urls.append(line)
    return urls


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--urls", default="benchmark_urls.txt")
    parser.add_argument("--proxy", default="http://127.0.0.1:9000")
    parser.add_argument("--passes", type=int, default=3)
    parser.add_argument("--hits", type=int, default=10)
    parser.add_argument("--timeout", type=int, default=5)
    parser.add_argument("--output", default="benchmark_results.csv")
    args = parser.parse_args()

    urls = load_urls(args.urls)
    if not urls:
        print("No URLs found", file=sys.stderr)
        return 1

    total_trials = len(urls) * args.passes * (args.hits + 1)
    completed = 0
    failures = 0
    skipped = 0

    with open(args.output, "w", newline="") as output_file:
        writer = csv.DictWriter(
            output_file,
            fieldnames=[
                "pass", "base_url", "request_url", "trial", "kind",
                "time_ms", "http_code", "result", "error",
            ],
        )
        writer.writeheader()

        for pass_number in range(1, args.passes + 1):
            for base_url in urls:
                request_url = request_url_for_pass(base_url, pass_number)
                miss_succeeded = True
                for trial in range(args.hits + 1):
                    kind = "miss" if trial == 0 else "hit"
                    if trial > 0 and not miss_succeeded:
                        http_code = ""
                        curl_seconds = ""
                        fallback_ms = 0
                        result = "skipped_after_miss_failure"
                    else:
                        http_code, curl_seconds, result, fallback_ms = measure(
                            args.proxy,
                            request_url,
                            args.timeout,
                        )
                        if trial == 0:
                            miss_succeeded = result == "ok"

                    completed += 1
                    if result == "skipped_after_miss_failure":
                        skipped += 1
                    elif result != "ok":
                        failures += 1

                    time_ms = ""
                    if result == "ok":
                        time_ms = f"{float(curl_seconds) * 1000:.3f}"
                    elif result == "timeout":
                        time_ms = f"{fallback_ms:.3f}"

                    writer.writerow({
                        "pass": pass_number,
                        "base_url": base_url,
                        "request_url": request_url,
                        "trial": trial,
                        "kind": kind,
                        "time_ms": time_ms,
                        "http_code": http_code,
                        "result": result,
                        "error": "" if result == "ok" else result,
                    })
                    output_file.flush()

                print(
                    f"pass={pass_number} url={base_url} "
                    f"progress={completed}/{total_trials} "
                    f"failures={failures} skipped={skipped}",
                    flush=True,
                )

    print(
        f"Wrote {completed} trials to {args.output}; "
        f"failures={failures} skipped={skipped}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
