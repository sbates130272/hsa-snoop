#!/usr/bin/env python3
"""Aggregate hsa-snoop Slurm validation results into a pass/fail matrix.

All oracle logic lives here rather than in the node jobs, so the checks can be
corrected and re-evaluated against already-collected logs without re-running
any GPU jobs.

Ground truth comes from examples/gfx-test.hip:

    const dim3 block(256);
    const dim3 grid((n_elems + 255) / 256);
    hipMalloc(&a, n*4); hipMalloc(&b, n*4); hipMalloc(&c, n*4);
    hipMalloc(&result, 4);

    vadd  (const float* a, const float* b, float* c, int n)   -> 3 ptrs, 0 LDS
    vscale(float* a, float s, int n)                          -> 1 ptr,  0 LDS
    vdot  (const float* a, const float* b, float* result,
           int n, int iters)  __shared__ float sdata[256]     -> 3 ptrs, 1024 B LDS

Usage: scripts/slurm/report.py <results-dir> [--markdown report.md]
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
from collections import Counter

# The kernel field is a demangled C++ signature and contains spaces, e.g.
# "vadd(float const*, float const*, float*, int)", so it must be matched
# non-greedily up to the following " gpu=" rather than as \S+.
MEM_RE = re.compile(
    r"\[mem\]\s+kernel=(?P<kernel>.+?)\s+gpu=(?P<gpu>\d+)\s+grid=(?P<grid>\d+)\s+"
    r"lds=(?P<lds>\d+)B\s+scratch=(?P<scratch>\d+)B\s+"
    r"mapped_vram=(?P<vram>\d+)B\s+ptrs=(?P<ptrs>\d+)"
)

# hsa-snoop prints "mapped_vram=unavailable ptrs=unavailable" when the kernarg
# scan could not run. Those lines must be counted, not silently dropped by
# MEM_RE — an unmeasurable dispatch is a result, and a harness that ignores it
# would report a clean pass over a partially blind run.
MEM_UNAVAIL_RE = re.compile(
    r"\[mem\]\s+kernel=(?P<kernel>.+?)\s+gpu=(?P<gpu>\d+)\s+grid=(?P<grid>\d+)\s+"
    r"lds=(?P<lds>\d+)B\s+scratch=(?P<scratch>\d+)B\s+mapped_vram=unavailable"
)

# kernel-substring -> (expected LDS bytes, expected pointer count, ptr buffers)
KERNELS = {
    "vadd": (0, 3, 3),
    "vscale": (0, 1, 1),
    "vdot": (1024, 3, 2),  # a, b are n*4; result is 4 B, so 2 large buffers
}


def parse_kv(path: pathlib.Path) -> dict[str, str]:
    out: dict[str, str] = {}
    if not path.exists():
        return out
    for line in path.read_text(errors="replace").splitlines():
        if "=" in line and not line.startswith("-"):
            k, _, v = line.partition("=")
            out.setdefault(k.strip(), v.strip())
    return out


def classify(name: str) -> str | None:
    """Map a (possibly mangled) kernel symbol to one of the known kernels."""
    for key in KERNELS:
        if key in name:
            return key
    return None


class Check:
    def __init__(self, name: str, status: str, detail: str = ""):
        self.name = name
        self.status = status  # PASS / FAIL / SKIP / INFO
        self.detail = detail


def analyse(node_dir: pathlib.Path) -> tuple[dict, list[Check]]:
    probe = parse_kv(node_dir / "probe.txt")
    params = parse_kv(node_dir / "params.txt")
    checks: list[Check] = []

    n_elems = int(params.get("n_elems", "1048576"))
    buf_bytes = n_elems * 4
    expected_grid = ((n_elems + 255) // 256) * 256

    # --- build -------------------------------------------------------------
    if (node_dir / "NO_ROCM").exists():
        checks.append(
            Check("build", "SKIP", "node has no usable ROCm HIP install (/opt/rocm stub)")
        )
        return probe, checks
    rc_file = node_dir / "build.rc"
    if not rc_file.exists():
        checks.append(Check("build", "SKIP", "no build.rc (job did not reach build)"))
        return probe, checks
    build_rc = rc_file.read_text().strip()
    checks.append(
        Check("build", "PASS" if build_rc == "0" else "FAIL", f"rc={build_rc}")
    )
    if build_rc != "0":
        return probe, checks

    # --- ctest -------------------------------------------------------------
    ctest = (node_dir / "ctest.log").read_text(errors="replace") if (
        node_dir / "ctest.log"
    ).exists() else ""
    for label, key in (("unit:sdma-ring-test", "unit_rc"), ("hw:sdma-hardware", "hw_rc")):
        m = re.search(rf"^{key}=(\d+)$", ctest, re.M)
        if not m:
            checks.append(Check(label, "SKIP", "no result line in ctest.log"))
        else:
            rc = m.group(1)
            checks.append(Check(label, "PASS" if rc == "0" else "FAIL", f"rc={rc}"))

    # --- mem-snoop ---------------------------------------------------------
    mem_log = node_dir / "mem-snoop.log"
    if not mem_log.exists():
        checks.append(Check("mem-snoop:ran", "SKIP", "no mem-snoop.log"))
        return probe, checks
    text = mem_log.read_text(errors="replace")

    records = [m.groupdict() for m in MEM_RE.finditer(text)]
    unavailable = [m.groupdict() for m in MEM_UNAVAIL_RE.finditer(text)]
    if unavailable:
        checks.append(
            Check(
                "mem-snoop:footprint measurable",
                "FAIL",
                f"{len(unavailable)}/{len(records) + len(unavailable)} dispatches "
                "reported mapped_vram=unavailable (kernarg or VM map unreadable)",
            )
        )
    else:
        checks.append(
            Check(
                "mem-snoop:footprint measurable",
                "PASS",
                f"all {len(records)} dispatches measured",
            )
        )
    if not records:
        rc = re.search(r"^mem_snoop_rc=(\d+)$", text, re.M)
        checks.append(
            Check(
                "mem-snoop:emitted [mem] lines",
                "FAIL",
                f"0 records parsed (hsa-snoop rc={rc.group(1) if rc else '?'}); "
                "kernarg scan produced nothing",
            )
        )
        return probe, checks
    checks.append(
        Check("mem-snoop:emitted [mem] lines", "PASS", f"{len(records)} records")
    )

    by_kernel: dict[str, list[dict]] = {}
    for r in records:
        key = classify(r["kernel"])
        if key:
            by_kernel.setdefault(key, []).append(r)

    unmatched = {r["kernel"] for r in records if classify(r["kernel"]) is None}
    if unmatched:
        checks.append(
            Check(
                "mem-snoop:kernel names",
                "INFO",
                f"unrecognised symbols: {sorted(unmatched)[:3]}",
            )
        )

    for key, (exp_lds, exp_ptrs, big_bufs) in KERNELS.items():
        rs = by_kernel.get(key)
        if not rs:
            checks.append(Check(f"{key}:present", "FAIL", "no [mem] record for kernel"))
            continue

        # LDS is the strongest oracle: it comes straight from the AQL packet's
        # group_segment_size and is fixed by the kernel source.
        lds = {int(r["lds"]) for r in rs}
        checks.append(
            Check(
                f"{key}:lds=={exp_lds}B",
                "PASS" if lds == {exp_lds} else "FAIL",
                f"observed {sorted(lds)}",
            )
        )

        # Compare the MODAL pointer count, not the set. A minority of
        # dispatches near process teardown read a recycled kernarg block and
        # report garbage; that is tracked separately as a stability check so it
        # does not mask whether steady-state decoding is correct.
        ptrs = [int(r["ptrs"]) for r in rs]
        modal_ptrs = Counter(ptrs).most_common(1)[0][0]
        checks.append(
            Check(
                f"{key}:ptrs=={exp_ptrs}",
                "PASS" if modal_ptrs == exp_ptrs else "FAIL",
                f"mode={modal_ptrs} over {len(ptrs)} dispatches, "
                f"distribution {dict(sorted(Counter(ptrs).items()))}",
            )
        )

        grids = {int(r["grid"]) for r in rs}
        checks.append(
            Check(
                f"{key}:grid=={expected_grid}",
                "PASS" if grids == {expected_grid} else "FAIL",
                f"observed {sorted(grids)}",
            )
        )

        # mapped_vram is documented as an upper bound (whole backing regions,
        # no pointer chasing), so assert a lower bound on the modal value only.
        floor = big_bufs * buf_bytes
        vram = [int(r["vram"]) for r in rs]
        modal_vram = Counter(vram).most_common(1)[0][0]
        checks.append(
            Check(
                f"{key}:mapped_vram>={floor // (1 << 20)}MiB",
                "PASS" if modal_vram >= floor else "FAIL",
                f"mode={modal_vram // (1 << 20)} MiB "
                f"(range {min(vram) // (1 << 20)}–{max(vram) // (1 << 20)} MiB)",
            )
        )

    # --- kernarg stability / teardown race ----------------------------------
    # Identical dispatches of the same kernel must produce identical footprints.
    # On gfx90a a minority of dispatches (observed: the last one or two before
    # process exit) report inflated pointer counts and wildly inflated
    # mapped_vram, consistent with the kernarg block being freed or recycled
    # between packet observation and the process_vm_readv that scans it.
    outliers = []
    worst_ratio = 1.0
    for key, rs in by_kernel.items():
        vram = [int(r["vram"]) for r in rs]
        modal_vram = Counter(vram).most_common(1)[0][0]
        for r in rs:
            v = int(r["vram"])
            if v != modal_vram:
                ratio = v / modal_vram if modal_vram else float("inf")
                worst_ratio = max(worst_ratio, ratio)
                outliers.append((key, int(r["ptrs"]), v, modal_vram))
    if outliers:
        k, p, v, m = max(outliers, key=lambda o: o[2])
        checks.append(
            Check(
                "mem-snoop:kernarg stability",
                "FAIL",
                f"{len(outliers)}/{len(records)} dispatches deviate from their "
                f"kernel's modal footprint; worst {k} reported {v:,} B "
                f"(ptrs={p}) vs modal {m:,} B — {worst_ratio:.0f}x overestimate",
            )
        )
    else:
        checks.append(
            Check(
                "mem-snoop:kernarg stability",
                "PASS",
                f"all {len(records)} dispatches match their kernel's modal footprint",
            )
        )

    # --- scratch consistency ------------------------------------------------
    # main.cpp prints mr.private_seg_bytes on the [mem] line, while the commit
    # message and hsa_kernel_scratch_bytes_total both specify
    # private_segment_size * grid_size. Record which one trace mode matches.
    scratches = {(int(r["scratch"]), int(r["grid"])) for r in records}
    nonzero = [s for s, _ in scratches if s]
    if not nonzero:
        detail = "all scratch=0 (kernels use no scratch); cannot discriminate"
        status = "INFO"
    else:
        looks_total = all(s % g == 0 and s // g > 0 for s, g in scratches if s)
        detail = (
            f"scratch values {sorted(nonzero)}; "
            + ("consistent with per-item*grid" if looks_total else "looks per-item only")
        )
        status = "INFO"
    checks.append(Check("scratch:trace-vs-prometheus units", status, detail))

    return probe, checks


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("results_dir", type=pathlib.Path)
    ap.add_argument("--markdown", type=pathlib.Path, default=None)
    args = ap.parse_args()

    node_dirs = sorted(
        d for d in args.results_dir.iterdir() if d.is_dir() and (d / "probe.txt").exists()
    )
    if not node_dirs:
        print(f"no completed node directories under {args.results_dir}", file=sys.stderr)
        return 1

    lines: list[str] = []
    findings: list[str] = []
    env_rows: list[str] = []

    all_checks: dict[str, dict[str, Check]] = {}
    for d in node_dirs:
        probe, checks = analyse(d)
        label = f"{probe.get('arch', '?')} ({probe.get('node', d.name)})"
        all_checks[label] = {c.name: c for c in checks}
        env_rows.append(
            "| {} | {} | {} | {} | {} | {} |".format(
                label,
                probe.get("partition", "?"),
                probe.get("sudo_nopasswd", "?"),
                probe.get("tracefs_readable", "?"),
                probe.get("docker", "?").split()[0] if probe.get("docker") else "?",
                probe.get("kernel", "?"),
            )
        )
        for c in checks:
            if c.status == "FAIL":
                findings.append(f"- **{label}** — `{c.name}`: {c.detail} (`{d}`)")

    lines.append("## Environment probe\n")
    lines.append("| Target | Partition | sudo -n | tracefs | docker | kernel |")
    lines.append("|---|---|---|---|---|---|")
    lines.extend(env_rows)

    check_names: list[str] = []
    for m in all_checks.values():
        for n in m:
            if n not in check_names:
                check_names.append(n)

    lines.append("\n## Results matrix\n")
    header = "| Check | " + " | ".join(all_checks) + " |"
    lines.append(header)
    lines.append("|---" * (len(all_checks) + 1) + "|")
    sym = {"PASS": "PASS", "FAIL": "**FAIL**", "SKIP": "skip", "INFO": "info"}
    for name in check_names:
        row = [f"`{name}`"]
        for label in all_checks:
            c = all_checks[label].get(name)
            row.append(sym.get(c.status, "-") if c else "-")
        lines.append("| " + " | ".join(row) + " |")

    lines.append("\n## Findings\n")
    lines.extend(findings if findings else ["No failing assertions."])

    lines.append("\n## Informational\n")
    for label, m in all_checks.items():
        for c in m.values():
            if c.status == "INFO":
                lines.append(f"- **{label}** — `{c.name}`: {c.detail}")

    out = "\n".join(lines) + "\n"
    print(out)
    if args.markdown:
        args.markdown.write_text(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
