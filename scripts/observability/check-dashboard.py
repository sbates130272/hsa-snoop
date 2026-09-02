#!/usr/bin/env python3
"""Execute every query in the hsa-snoop dashboard against the replay stack.

Loading a dashboard in a browser tells you it renders. It does not tell you
which of its forty panels are silently empty because a label was renamed, a
template variable expands to a regex that matches nothing, or a metric is only
created after the first error. This walks the dashboard's own JSON, runs each
target against Prometheus or Loki over the capture window, and reports the
series count -- so "no data" becomes a build failure rather than something you
notice a month later.

    scripts/observability/check-dashboard.py [--start UNIX] [--end UNIX]
                                             [--dashboard PATH] [-v]

With no window it reads the capture staged by stack.sh and uses its full span.
Exits non-zero if any panel that should have data returned none.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request

REPO = pathlib.Path(__file__).resolve().parents[2]

# Panels that are legitimately empty on a capture without the corresponding
# hsa-snoop flag. Reported, but not counted as failures.
OPTIONAL = re.compile(r"^(AIS|hsa_errors|Packet Errors)", re.I)


def published_port(container: str, cport: int) -> int:
    out = subprocess.run(["docker", "port", container, str(cport)],
                         capture_output=True, text=True)
    if out.returncode != 0 or not out.stdout.strip():
        sys.exit(f"stack not running: no published {cport} on {container}")
    return int(out.stdout.strip().splitlines()[0].rsplit(":", 1)[1])


def get(url: str, params: dict) -> dict:
    full = url + "?" + urllib.parse.urlencode(params)
    try:
        with urllib.request.urlopen(full, timeout=60) as r:
            return json.load(r)
    except urllib.error.HTTPError as e:
        return {"status": "error", "error": e.read().decode()[:200]}
    except Exception as e:  # noqa: BLE001 - report, do not abort the sweep
        return {"status": "error", "error": str(e)}


def expand(expr: str, start: int, end: int) -> str:
    """Substitute Grafana's built-in and template variables.

    Every variable is expanded to its "All" form, which is the widest possible
    match. A panel that is empty even then is empty for a real reason.
    """
    span = max(end - start, 60)
    subs = {
        "$__rate_interval": "1m",
        "$__interval": "15s",
        "$__range": f"{span}s",
        "$__auto": "15s",
        "$__auto_interval_interval": "15s",
    }
    for k, v in subs.items():
        expr = expr.replace(k, v)
    # Template variables. ".*" matches everything for the =~ comparisons the
    # dashboard uses throughout.
    expr = re.sub(r"\$\{?(host|gpu_id|comm|pid|kernel|pcie_id|device_type)\}?",
                  ".*", expr)
    return expr


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--start", type=int)
    ap.add_argument("--end", type=int)
    ap.add_argument("--dashboard",
                    default=str(REPO / "grafana" / "hsa-snoop-dashboard.json"))
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    pport = published_port("hsasnoop-prometheus", 9090)
    lport = published_port("hsasnoop-loki", 3100)

    if args.start is None or args.end is None:
        # Derive the window from the data itself rather than the wall clock:
        # a replayed capture is always in the past, and defaulting to now-1h
        # would report every single panel as empty.
        #
        # Scanning from the epoch is not an option -- Prometheus caps a
        # query_range at 11000 points and rejects anything wider. Sweep the last
        # 30 days at 5 min, then refine.
        #
        # 5 min, not 1 h: a capture is ~10 min long, Prometheus' lookback delta
        # is 5 min, and hourly steps therefore step straight over it. Thirty days
        # at 300 s is 8640 points, just inside the cap.
        now = int(__import__("time").time())
        coarse = get(f"http://127.0.0.1:{pport}/api/v1/query_range",
                     {"query": "hsa_snoop_up", "start": now - 30 * 86400,
                      "end": now, "step": "300"})
        vals = [v for s in coarse.get("data", {}).get("result", [])
                for v in s["values"]]
        if not vals:
            sys.exit("no hsa_snoop_up in the last 30 days of the replay "
                     "Prometheus; is a capture loaded? "
                     "(pass --start/--end for an older one)")
        lo, hi = int(min(v[0] for v in vals)), int(max(v[0] for v in vals))
        fine = get(f"http://127.0.0.1:{pport}/api/v1/query_range",
                   {"query": "hsa_snoop_up", "start": lo - 3600,
                    "end": hi + 3600, "step": "10"})
        vals = [v for s in fine.get("data", {}).get("result", [])
                for v in s["values"]] or vals
        args.start = int(min(v[0] for v in vals)) - 60
        args.end = int(max(v[0] for v in vals)) + 60

    print(f"window {args.start}..{args.end} "
          f"({args.end - args.start}s)  prom=:{pport} loki=:{lport}\n")

    dash = json.loads(pathlib.Path(args.dashboard).read_text())

    # Collapsed rows carry their children in row["panels"]; walk both.
    def walk(panels):
        for p in panels:
            if p.get("type") == "row":
                yield from walk(p.get("panels", []))
            else:
                yield p

    empty, failed, ok = [], [], 0
    for panel in walk(dash["panels"]):
        title = panel.get("title", f"#{panel.get('id')}")
        for t in panel.get("targets", []) or []:
            expr = t.get("expr")
            if not expr:
                continue
            ds = (t.get("datasource") or panel.get("datasource") or {})
            is_loki = ds.get("type") == "loki"
            q = expand(expr, args.start, args.end)
            if is_loki:
                r = get(f"http://127.0.0.1:{lport}/loki/api/v1/query_range",
                        {"query": q, "start": f"{args.start}000000000",
                         "end": f"{args.end}000000000", "limit": "10",
                         "step": "15"})
            else:
                r = get(f"http://127.0.0.1:{pport}/api/v1/query_range",
                        {"query": q, "start": args.start, "end": args.end,
                         "step": "15"})
            label = f"{title} [{t.get('refId', '?')}]"
            if r.get("status") != "success":
                failed.append((label, r.get("error", "?"), q))
                print(f"  FAIL  {label}: {str(r.get('error'))[:120]}")
                continue
            n = len(r.get("data", {}).get("result", []))
            if n == 0:
                empty.append((label, q))
                print(f"  EMPTY {label}")
            else:
                ok += 1
                if args.verbose:
                    print(f"  ok    {label}: {n} series")

    print(f"\n{ok} target(s) returned data, {len(empty)} empty, {len(failed)} failed")

    hard_empty = [e for e in empty if not OPTIONAL.search(e[0])]
    if failed or hard_empty:
        print("\nnon-optional problems:")
        for label, err, q in failed:
            print(f"  FAIL  {label}\n        {err}\n        {q}")
        for label, q in hard_empty:
            print(f"  EMPTY {label}\n        {q}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
