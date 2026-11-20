#!/usr/bin/env python3
"""
Summarise per-kernel timing from an hsa-snoop dispatch log and append it
to the kernel-timing history.

Reads the NDJSON written by `hsa-snoop --log-dispatches`, groups the
dispatch events by kernel name, and records two durations for each:

  queue_us   dur_us, the enqueue-to-queue-consumed residency hsa-snoop
             observes by polling the ring. Absent for a dispatch that
             was already consumed when first seen, so n can be below
             the dispatch count, and the whole entry is null when no
             dispatch was seen pending.
  gpu_ticks  gpu_end_ticks - gpu_start_ticks, the shader-clock interval
             the command processor wrote into the completion signal.
             Present only when the workload ran under
             HSA_TOOLS_LIB=libhsa-snoop-profiler.so and the device
             writes the timestamps. Raw ticks, not a time.

Appends one record to <out-dir>/history.jsonl and writes the shields.io
badge <out-dir>/badge-kernel_timing.json. The page that presents them is
docs/timing/ (docs/javascripts/timing.js).

Usage, against a checkout of the gh-pages branch:
  python3 .github/ci/report/publish-kernel-timing.py \\
    --dispatch-log dispatches.ndjson \\
    --out-dir gh-pages/timing \\
    [--runner github-hosted] [--sha <git-sha>] [--run-id <github-run-id>] \\
    [--summary "$GITHUB_STEP_SUMMARY"]
"""

import argparse
import datetime
import json
import math
import os
import statistics
import sys
from pathlib import Path

UNRESOLVED = "(unresolved)"
# Packets hsa-snoop logs as dispatches that are not kernel launches: other
# AMD vendor packets on the runtime's internal queue.
NOT_KERNELS = {"vendor_specific", "barrier_value"}


def stats(values: list[float]) -> dict | None:
    if not values:
        return None
    ordered = sorted(values)
    # Nearest-rank p95: with the handful of samples a run produces, an
    # interpolated percentile would report a value no dispatch had.
    p95 = ordered[max(0, math.ceil(0.95 * len(ordered)) - 1)]
    return {
        "n": len(ordered),
        "median": statistics.median(ordered),
        "min": ordered[0],
        "max": ordered[-1],
        "p95": p95,
    }


def summarise(log_text: str) -> dict:
    """Return {"dispatches", "kernels": {name: {dispatches, queue_us, gpu_ticks}}, "all": {...}}."""
    per_kernel: dict[str, dict[str, list[float]]] = {}
    counts: dict[str, int] = {}
    for line in log_text.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            ev = json.loads(line)
        except json.JSONDecodeError:
            continue
        if ev.get("ev") != "dispatch":
            continue
        name = ev.get("kernel") or UNRESOLVED
        if name in NOT_KERNELS:
            continue
        counts[name] = counts.get(name, 0) + 1
        samples = per_kernel.setdefault(name, {"queue_us": [], "gpu_ticks": []})
        if isinstance(ev.get("dur_us"), (int, float)):
            samples["queue_us"].append(float(ev["dur_us"]))
        start, end = ev.get("gpu_start_ticks"), ev.get("gpu_end_ticks")
        if isinstance(start, int) and isinstance(end, int) and end > start > 0:
            samples["gpu_ticks"].append(float(end - start))

    return {
        "dispatches": sum(counts.values()),
        "kernels": {
            name: {
                "dispatches": counts[name],
                "queue_us": stats(samples["queue_us"]),
                "gpu_ticks": stats(samples["gpu_ticks"]),
            }
            for name, samples in sorted(per_kernel.items())
        },
        "all": {
            "queue_us": stats([v for s in per_kernel.values() for v in s["queue_us"]]),
            "gpu_ticks": stats([v for s in per_kernel.values() for v in s["gpu_ticks"]]),
        },
    }


def badge(record: dict | None) -> dict:
    out = {"schemaVersion": 1, "label": "Kernel Timing", "message": "no data", "color": "lightgrey"}
    if not record:
        return out
    gpu, queue = record["all"]["gpu_ticks"], record["all"]["queue_us"]
    # The source is part of the message: a tick count and a queue residency
    # are different measurements and must not read as one series.
    if gpu:
        out.update(message=f"{gpu['median']:,.0f} ticks median (GPU)", color="blue")
    elif queue:
        out.update(message=f"{queue['median']:,.0f} µs median (queue)", color="blue")
    return out


def markdown(record: dict) -> str:
    def cell(s: dict | None, key: str) -> str:
        return "–" if not s else f"{s[key]:,.0f}"

    lines = [
        "### Kernel timing",
        "",
        f"{record['dispatches']} dispatches across {len(record['kernels'])} kernels.",
        "",
        "| Kernel | Dispatches | GPU ticks (median) | GPU ticks (max) | Queue µs (median) | Queue µs (p95) |",
        "| --- | ---: | ---: | ---: | ---: | ---: |",
    ]
    for name, k in record["kernels"].items():
        lines.append(
            f"| `{name}` | {k['dispatches']} | {cell(k['gpu_ticks'], 'median')} | "
            f"{cell(k['gpu_ticks'], 'max')} | {cell(k['queue_us'], 'median')} | "
            f"{cell(k['queue_us'], 'p95')} |"
        )
    if not record["all"]["gpu_ticks"]:
        lines += ["", "No dispatch carried GPU timestamps; only queue-observed timing was recorded."]
    return "\n".join(lines) + "\n"


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dispatch-log", required=True, help="NDJSON from hsa-snoop --log-dispatches")
    ap.add_argument("--out-dir", required=True,
                    help="Directory holding history.jsonl and the badge (timing/ on gh-pages)")
    ap.add_argument("--runner", default="github-hosted", help="Runner class label")
    ap.add_argument("--sha", default=os.environ.get("GITHUB_SHA", ""))
    ap.add_argument("--run-id", default=os.environ.get("GITHUB_RUN_ID", ""))
    ap.add_argument("--summary", metavar="PATH", help="Append a Markdown summary table to this file")
    args = ap.parse_args()

    summary = summarise(Path(args.dispatch_log).read_text())
    if not summary["dispatches"]:
        print(f"no dispatch events in {args.dispatch_log}", file=sys.stderr)
        sys.exit(1)

    record = {
        "timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat().replace("+00:00", "Z"),
        "meta": {"sha": args.sha, "run_id": args.run_id, "runner": args.runner},
        **summary,
    }

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    with (out_dir / "history.jsonl").open("a") as fh:
        fh.write(json.dumps(record) + "\n")
    (out_dir / "badge-kernel_timing.json").write_text(json.dumps(badge(record)))
    print(f"recorded {record['dispatches']} dispatches across {len(record['kernels'])} kernels in {out_dir}")

    if args.summary:
        with open(args.summary, "a") as fh:
            fh.write(markdown(record))


if __name__ == "__main__":
    main()
