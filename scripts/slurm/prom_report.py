#!/usr/bin/env python3
"""Analyse the Prometheus exporter artefacts produced by prom-job.sh.

Checks the exporter against three independent references:

1. Ground truth from examples/gfx-test.hip (vdot's LDS is exactly 1024 B;
   vadd's footprint is exactly 3 x 4 MiB).
2. The trace-mode [mem] output over the same workload — the two modes are
   mutually exclusive in main.cpp, so agreeing is not automatic.
3. Prometheus's own view of the data: metric TYPE metadata, and the counter
   monotonicity contract across every scrape.

Usage: scripts/slurm/prom_report.py <results-dir>
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
from collections import Counter

MEM_RE = re.compile(
    r"\[mem\]\s+kernel=(?P<kernel>.+?)\s+gpu=(?P<gpu>\d+)\s+grid=(?P<grid>\d+)\s+"
    r"lds=(?P<lds>\d+)B\s+scratch=(?P<scratch>\d+)B\s+"
    r"mapped_vram=(?P<vram>\d+)B\s+ptrs=(?P<ptrs>\d+)"
)

KERNELS = {"vadd": (0, 3), "vscale": (0, 1), "vdot": (1024, 3)}


class Check:
    def __init__(self, name, status, detail=""):
        self.name, self.status, self.detail = name, status, detail


def classify(name):
    for k in KERNELS:
        if k in name:
            return k
    return None


def load_range(d: pathlib.Path, metric: str):
    """Return {kernel_key: [(ts, value), ...]} for a query_range dump."""
    f = d / f"range-{metric}.json"
    if not f.exists():
        return None
    try:
        doc = json.loads(f.read_text())
    except json.JSONDecodeError:
        return None
    if doc.get("status") != "success":
        return None
    out = {}
    for series in doc.get("data", {}).get("result", []):
        key = classify(series.get("metric", {}).get("kernel_name", ""))
        if not key:
            continue
        pts = [(float(t), float(v)) for t, v in series.get("values", [])]
        out.setdefault(key, []).extend(pts)
    return out


def analyse(d: pathlib.Path):
    checks = []
    params = {}
    pf = d / "params.txt"
    if pf.exists():
        for line in pf.read_text().splitlines():
            if "=" in line:
                k, _, v = line.partition("=")
                params[k] = v
    n_elems = int(params.get("n_elems", "1048576"))
    buf = n_elems * 4

    rc = (d / "build.rc").read_text().strip() if (d / "build.rc").exists() else None
    if rc is None:
        return [Check("prom:build", "SKIP", "no build.rc")]
    checks.append(Check("prom:build (PROMETHEUS=ON)",
                        "PASS" if rc == "0" else "FAIL", f"rc={rc}"))
    if rc != "0":
        return checks

    # --- did Prometheus consider the target healthy? ------------------------
    up = d / "up.json"
    if up.exists():
        try:
            doc = json.loads(up.read_text())
            vals = doc.get("data", {}).get("result", [])
            v = vals[0]["value"][1] if vals else "0"
            checks.append(Check("prom:target up", "PASS" if v == "1" else "FAIL",
                                f"up={v}"))
        except (json.JSONDecodeError, KeyError, IndexError):
            checks.append(Check("prom:target up", "FAIL", "could not parse up.json"))

    # --- exposition format actually ingested -------------------------------
    lds = load_range(d, "hsa_kernel_lds_bytes")
    vram = load_range(d, "hsa_kernel_mapped_vram_bytes")
    total = load_range(d, "hsa_kernel_mapped_vram_bytes_total")
    if not lds:
        checks.append(Check("prom:series ingested", "FAIL",
                            "no hsa_kernel_lds_bytes series in the TSDB"))
        return checks
    n_samples = sum(len(v) for v in lds.values())
    checks.append(Check("prom:series ingested", "PASS",
                        f"{n_samples} samples across {len(lds)} kernels"))

    # --- ground truth: LDS --------------------------------------------------
    for key, (exp_lds, _) in KERNELS.items():
        pts = lds.get(key)
        if not pts:
            checks.append(Check(f"prom:{key}:lds", "FAIL", "series absent"))
            continue
        vals = {v for _, v in pts}
        checks.append(Check(f"prom:{key}:lds=={exp_lds}B",
                            "PASS" if vals == {float(exp_lds)} else "FAIL",
                            f"observed {sorted(vals)} over {len(pts)} samples"))

    # --- the gauge the fix guards ------------------------------------------
    if vram is None or not vram:
        checks.append(Check("prom:mapped_vram gauge present", "FAIL",
                            "hsa_kernel_mapped_vram_bytes absent from the TSDB"))
    else:
        checks.append(Check("prom:mapped_vram gauge present", "PASS",
                            f"{len(vram)} kernel series"))
        for key, floor in (("vadd", 3 * buf), ("vscale", 1 * buf)):
            pts = vram.get(key)
            if not pts:
                checks.append(Check(f"prom:{key}:mapped_vram", "FAIL", "series absent"))
                continue
            modal = Counter(v for _, v in pts).most_common(1)[0][0]
            checks.append(Check(
                f"prom:{key}:mapped_vram=={floor // (1 << 20)}MiB",
                "PASS" if modal == float(floor) else "FAIL",
                f"modal {int(modal):,} B vs expected {floor:,} B"))

    # --- cross-mode agreement ----------------------------------------------
    tm = d / "trace-mode.log"
    if tm.exists() and vram:
        recs = [m.groupdict() for m in MEM_RE.finditer(tm.read_text(errors="replace"))]
        by = {}
        for r in recs:
            k = classify(r["kernel"])
            if k:
                by.setdefault(k, []).append(int(r["vram"]))
        for key in KERNELS:
            if key not in by or key not in vram:
                continue
            t_modal = Counter(by[key]).most_common(1)[0][0]
            p_modal = Counter(v for _, v in vram[key]).most_common(1)[0][0]
            checks.append(Check(
                f"prom:{key}:trace==prometheus",
                "PASS" if float(t_modal) == p_modal else "FAIL",
                f"trace {t_modal:,} B vs prometheus {int(p_modal):,} B"))

    # --- counter contract ---------------------------------------------------
    # A Prometheus counter must never decrease. More interestingly, a single
    # bogus sample from the kernarg-recycling race permanently inflates it, so
    # quantify the largest single increment against the kernel's true footprint.
    if total:
        violations, worst_jump = [], []
        for key, pts in total.items():
            pts = sorted(pts)
            for (t0, v0), (t1, v1) in zip(pts, pts[1:]):
                if v1 < v0:
                    violations.append((key, v0, v1))
            gauge = vram.get(key) if vram else None
            expected = (Counter(v for _, v in gauge).most_common(1)[0][0]
                        if gauge else None)
            if expected:
                jumps = [v1 - v0 for (_, v0), (_, v1) in zip(pts, pts[1:]) if v1 > v0]
                if jumps:
                    worst_jump.append((key, max(jumps), expected))
        checks.append(Check(
            "prom:counter monotonic",
            "PASS" if not violations else "FAIL",
            f"{len(violations)} decrease(s)" if violations
            else f"no decreases across {sum(len(v) for v in total.values())} samples"))

        for key, jump, expected in worst_jump:
            ratio = jump / expected if expected else 0
            # Several dispatches land between two 1s scrapes, so a jump of a few
            # multiples is normal; a large multiple means a bogus sample.
            checks.append(Check(
                f"prom:{key}:counter inflation",
                "INFO",
                f"largest 1s increment {int(jump):,} B = {ratio:.1f}x the "
                f"{int(expected):,} B per-dispatch footprint"))
    else:
        checks.append(Check("prom:counter monotonic", "SKIP",
                            "no _total series in the TSDB"))

    # --- metric type vs name ------------------------------------------------
    # Prometheus's own metadata settles whether the _total suffix is honest.
    for metric in ("hsa_kernel_scratch_bytes_total",
                   "hsa_kernel_mapped_vram_bytes_total"):
        f = d / f"metadata-{metric}.json"
        if not f.exists():
            continue
        try:
            doc = json.loads(f.read_text())
            entries = doc.get("data", {}).get(metric, [])
            mtype = entries[0].get("type") if entries else "absent"
        except (json.JSONDecodeError, KeyError, IndexError):
            mtype = "unparseable"
        ok = mtype == "counter"
        checks.append(Check(
            f"prom:{metric} type",
            "PASS" if ok else "FAIL",
            f"Prometheus reports type={mtype}; the _total suffix is reserved "
            f"for counters"))

    return checks


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("results_dir", type=pathlib.Path)
    args = ap.parse_args()

    dirs = sorted(p for p in args.results_dir.glob("*/prom") if p.is_dir())
    if not dirs:
        print(f"no prom/ artefacts under {args.results_dir}", file=sys.stderr)
        return 1

    all_checks, findings = {}, []
    for d in dirs:
        label = d.parent.name
        cs = analyse(d)
        all_checks[label] = cs
        for c in cs:
            if c.status == "FAIL":
                findings.append(f"- **{label}** — `{c.name}`: {c.detail}")

    names = []
    for cs in all_checks.values():
        for c in cs:
            if c.name not in names:
                names.append(c.name)

    print("## Prometheus exporter results\n")
    print("| Check | " + " | ".join(all_checks) + " |")
    print("|---" * (len(all_checks) + 1) + "|")
    sym = {"PASS": "PASS", "FAIL": "**FAIL**", "SKIP": "skip", "INFO": "info"}
    for n in names:
        row = [f"`{n}`"]
        for label, cs in all_checks.items():
            c = next((x for x in cs if x.name == n), None)
            row.append(sym.get(c.status, "-") if c else "-")
        print("| " + " | ".join(row) + " |")

    print("\n## Findings\n")
    print("\n".join(findings) if findings else "No failing assertions.")
    print("\n## Detail\n")
    for label, cs in all_checks.items():
        for c in cs:
            if c.detail:
                print(f"- {label} `{c.name}` [{c.status}]: {c.detail}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
