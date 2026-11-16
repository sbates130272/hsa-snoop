/* Render the kernel-timing page from timing/history.jsonl.
 *
 * Appended on gh-pages by .github/ci/report/publish-kernel-timing.py
 * after each hardware-test run on main; fetched at runtime like the
 * performance history.
 *
 * One record per line:
 *   { timestamp, meta: { sha, run_id, runner }, dispatches,
 *     kernels: { name: { dispatches, queue_us, gpu_ticks } },
 *     all: { queue_us, gpu_ticks } }
 * where queue_us / gpu_ticks are { n, median, min, max, p95 } or null.
 */

(function () {
  "use strict";

  const REPO = "https://github.com/sbates130272/hsa-snoop";
  const CANDIDATES = [
    "history.jsonl",
    "https://sbates130272.github.io/hsa-snoop/timing/history.jsonl",
  ];
  const WINDOW = 60; // runs charted

  function el(tag, cls, text) {
    const node = document.createElement(tag);
    if (cls) node.className = cls;
    if (text != null) node.textContent = text;
    return node;
  }

  function fmt(value) {
    if (value == null) return "–";
    return Number(value).toLocaleString("en-US", { maximumFractionDigits: 0 });
  }

  function stat(s, key) {
    return s ? fmt(s[key]) : "–";
  }

  async function load() {
    for (const url of CANDIDATES) {
      try {
        const res = await fetch(url, { cache: "no-store" });
        if (!res.ok) continue;
        const records = [];
        for (const line of (await res.text()).split("\n")) {
          if (!line.trim()) continue;
          try {
            records.push(JSON.parse(line));
          } catch (e) {
            /* one bad line should not hide the rest */
          }
        }
        return records;
      } catch (e) {
        /* try the next candidate */
      }
    }
    return [];
  }

  function sha(record) {
    return ((record.meta || {}).sha || "").slice(0, 8) || "unknown";
  }

  function when(record) {
    return (record.timestamp || "").slice(0, 16).replace("T", " ");
  }

  function renderTable(root, latest) {
    const table = document.createElement("table");
    const head = table.createTHead().insertRow();
    head.append(el("th", null, "Kernel"));
    for (const h of [
      "Dispatches",
      "GPU ticks (median)",
      "GPU ticks (min)",
      "GPU ticks (max)",
      "Queue µs (median)",
      "Queue µs (p95)",
    ]) head.append(el("th", "num", h));

    const body = table.createTBody();
    for (const [name, k] of Object.entries(latest.kernels || {})) {
      const row = body.insertRow();
      const cell = el("td");
      cell.append(el("code", null, name));
      row.append(cell);
      row.append(el("td", "num", fmt(k.dispatches)));
      row.append(el("td", "num", stat(k.gpu_ticks, "median")));
      row.append(el("td", "num", stat(k.gpu_ticks, "min")));
      row.append(el("td", "num", stat(k.gpu_ticks, "max")));
      row.append(el("td", "num", stat(k.queue_us, "median")));
      row.append(el("td", "num", stat(k.queue_us, "p95")));
    }
    root.replaceChildren(table);
  }

  let charts = [];

  function renderCharts(root, records, metric) {
    charts.forEach((c) => c.destroy());
    charts = [];
    root.replaceChildren();
    if (typeof Chart === "undefined") {
      root.append(el("p", "perf-empty", "Chart.js did not load; see the table above."));
      return;
    }

    const css = getComputedStyle(document.body);
    const pick = (name, fallback) => css.getPropertyValue(name).trim() || fallback;
    const line = pick("--md-primary-fg-color", "#009688");
    const text = pick("--md-default-fg-color--light", "#666");
    const grid = pick("--md-default-fg-color--lightest", "#ddd");

    const shown = records.slice(-WINDOW);
    const labels = shown.map((r) => (r.timestamp || "").slice(0, 10));
    const unit = metric === "gpu_ticks" ? "GPU ticks" : "queue µs";

    // Kernels of the latest run only: a name that no longer appears is a
    // kernel the workload no longer launches.
    const names = Object.keys(shown[shown.length - 1].kernels || {});
    for (const name of names) {
      const box = el("div", "perf-chart");
      box.append(el("div", "perf-chart__title", name + " (median " + unit + ")"));
      const wrap = el("div", "perf-chart__canvas");
      const canvas = document.createElement("canvas");
      wrap.append(canvas);
      box.append(wrap);
      root.append(box);

      charts.push(new Chart(canvas, {
        type: "line",
        data: {
          labels,
          datasets: [{
            data: shown.map((r) => {
              const s = ((r.kernels || {})[name] || {})[metric];
              return s ? s.median : null;
            }),
            borderColor: line,
            backgroundColor: line,
            borderWidth: 2,
            pointRadius: shown.length > 30 ? 0 : 3,
            pointHoverRadius: 5,
            tension: 0,
          }],
        },
        options: {
          responsive: true,
          maintainAspectRatio: false,
          interaction: { mode: "index", intersect: false },
          plugins: {
            legend: { display: false },
            tooltip: {
              callbacks: {
                title: (items) => {
                  const r = shown[items[0].dataIndex];
                  return when(r) + " · " + sha(r);
                },
                label: (item) => fmt(item.parsed.y) + " " + unit,
              },
            },
          },
          scales: {
            x: {
              ticks: { color: text, maxTicksLimit: 6, maxRotation: 0 },
              grid: { display: false },
            },
            y: {
              beginAtZero: true,
              ticks: { color: text, maxTicksLimit: 5 },
              grid: { color: grid },
            },
          },
        },
      }));
    }
  }

  async function main() {
    const latestEl = document.getElementById("timing-latest");
    if (!latestEl) return; // not the kernel-timing page

    const records = await load();
    if (!records.length) {
      latestEl.textContent =
        "No kernel timing has been published yet. The first successful " +
        "hardware-test run on main will record one.";
      return;
    }

    const latest = records[records.length - 1];
    const hasGpu = !!(latest.all || {}).gpu_ticks;
    latestEl.className = "";
    latestEl.replaceChildren(
      "Latest run: " + when(latest) + " UTC, commit " + sha(latest) + ", " +
        fmt(latest.dispatches) + " dispatches. ",
    );
    if ((latest.meta || {}).run_id) {
      const a = el("a", null, "Workflow run");
      a.href = REPO + "/actions/runs/" + latest.meta.run_id;
      latestEl.append(a, ". ");
    }
    if (!hasGpu) {
      latestEl.append(
        "No dispatch in this run carried GPU timestamps, so only " +
          "queue-observed timing is shown.",
      );
    }

    renderTable(document.getElementById("timing-table"), latest);

    // Chart the GPU interval when the latest run has it, and the queue
    // residency otherwise; never both on one axis.
    const metric = hasGpu ? "gpu_ticks" : "queue_us";
    const chartsEl = document.getElementById("timing-charts");
    renderCharts(chartsEl, records, metric);

    // The palette toggle recolours the page without a reload; the
    // canvases have to be redrawn to follow it.
    new MutationObserver(() => renderCharts(chartsEl, records, metric)).observe(
      document.body,
      { attributes: true, attributeFilter: ["data-md-color-scheme"] },
    );
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", main);
  } else {
    main();
  }
})();
