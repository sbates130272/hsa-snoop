/* Render the performance tracker from perf/history.jsonl.
 *
 * The history is not built into the site: the hardware-test lane
 * appends to it on gh-pages (.github/ci/report/publish-perf.py). So
 * this fetches at runtime and degrades to a message when nothing is
 * published, which is also what a fresh fork sees.
 *
 * One record per line:
 *   { timestamp, meta: { sha, run_id, runner }, metrics: { key: value } }
 */

(function () {
  "use strict";

  const REPO = "https://github.com/sbates130272/hsa-snoop";

  // Relative first, which is right on Pages. The absolute URL makes a
  // local `mkdocs serve` show the published data rather than nothing.
  const CANDIDATES = [
    "history.jsonl",
    "https://sbates130272.github.io/hsa-snoop/perf/history.jsonl",
  ];

  // Mirrors METRICS in publish-perf.py. A key missing here still
  // renders, under its raw name.
  const METRICS = {
    kernel_launches: {
      label: "Kernel dispatches",
      unit: "dispatches",
      note: "Total HSA kernel dispatches observed",
    },
    sdma_copies: {
      label: "SDMA copies",
      unit: "copies",
      note: "Total SDMA copy operations observed",
    },
    ais_rx_bytes: {
      label: "AIS RX bytes",
      unit: "bytes",
      note: "AIS read bytes total",
    },
    ais_tx_bytes: {
      label: "AIS TX bytes",
      unit: "bytes",
      note: "AIS write bytes total",
    },
  };

  const WINDOW = 60; // runs charted
  const TABLE_ROWS = 15;

  function info(key) {
    return METRICS[key] || { label: key.replace(/_/g, " "), unit: "", note: "" };
  }

  function fmt(value) {
    if (value == null) return "–";
    return Number(value).toLocaleString("en-US", { maximumFractionDigits: 2 });
  }

  function el(tag, cls, text) {
    const node = document.createElement(tag);
    if (cls) node.className = cls;
    if (text != null) node.textContent = text;
    return node;
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

  function metricKeys(records) {
    const keys = Object.keys(METRICS);
    for (const r of records)
      for (const k of Object.keys(r.metrics || {}))
        if (!keys.includes(k)) keys.push(k);
    return keys;
  }

  function sha(record) {
    return ((record.meta || {}).sha || "").slice(0, 8) || "unknown";
  }

  function renderCards(root, records, keys) {
    const latest = records[records.length - 1];
    const prev = records.length > 1 ? records[records.length - 2] : null;
    for (const key of keys) {
      const m = info(key);
      const value = (latest.metrics || {})[key];
      const card = el("div", "perf-card");
      card.append(el("div", "perf-card__label", m.label));
      card.append(el("div", "perf-card__value", fmt(value)));
      let note = m.note;
      const before = prev ? (prev.metrics || {})[key] : null;
      if (value != null && before != null) {
        const delta = value - before;
        note = delta === 0
          ? "unchanged from the previous run"
          : (delta > 0 ? "+" : "−") + fmt(Math.abs(delta)) + " vs the previous run";
      }
      card.append(el("div", "perf-card__note", note));
      root.append(card);
    }
  }

  let charts = [];

  function renderCharts(root, records, keys) {
    charts.forEach((c) => c.destroy());
    charts = [];
    root.replaceChildren();
    if (typeof Chart === "undefined") {
      root.append(el("p", "perf-empty", "Chart.js did not load; see the table below."));
      return;
    }

    const css = getComputedStyle(document.body);
    const pick = (name, fallback) => css.getPropertyValue(name).trim() || fallback;
    const line = pick("--md-primary-fg-color", "#009688");
    const text = pick("--md-default-fg-color--light", "#666");
    const grid = pick("--md-default-fg-color--lightest", "#ddd");

    const shown = records.slice(-WINDOW);
    const labels = shown.map((r) => (r.timestamp || "").slice(0, 10));

    for (const key of keys) {
      const m = info(key);
      const box = el("div", "perf-chart");
      box.append(el("div", "perf-chart__title", m.label + (m.unit ? " (" + m.unit + ")" : "")));
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
            data: shown.map((r) => (r.metrics || {})[key] ?? null),
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
                  return (r.timestamp || "").slice(0, 16).replace("T", " ") + " · " + sha(r);
                },
                label: (item) => fmt(item.parsed.y) + (m.unit ? " " + m.unit : ""),
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

  function renderTable(root, records, keys) {
    const table = document.createElement("table");
    const head = table.createTHead().insertRow();
    for (const h of ["Time (UTC)", "Commit", "Run", "Runner"]) head.append(el("th", null, h));
    for (const key of keys) head.append(el("th", "num", info(key).label));

    const body = table.createTBody();
    for (const r of records.slice(-TABLE_ROWS).reverse()) {
      const meta = r.meta || {};
      const row = body.insertRow();
      row.append(el("td", null, (r.timestamp || "").slice(0, 16).replace("T", " ")));

      const commit = el("td");
      if (meta.sha) {
        const a = el("a");
        a.href = REPO + "/commit/" + meta.sha;
        a.append(el("code", null, sha(r)));
        commit.append(a);
      } else {
        commit.textContent = "unknown";
      }
      row.append(commit);

      const run = el("td");
      if (meta.run_id) {
        const a = el("a", null, meta.run_id);
        a.href = REPO + "/actions/runs/" + meta.run_id;
        run.append(a);
      } else {
        run.textContent = "–";
      }
      row.append(run);

      row.append(el("td", null, meta.runner || "unknown"));
      for (const key of keys) row.append(el("td", "num", fmt((r.metrics || {})[key])));
    }
    root.replaceChildren(table);
  }

  async function main() {
    const latestEl = document.getElementById("perf-latest");
    if (!latestEl) return; // not the performance page

    const records = await load();
    if (!records.length) {
      latestEl.textContent =
        "No performance history has been published yet. The first " +
        "successful hardware-test run on main will record one.";
      return;
    }

    const keys = metricKeys(records);
    const latest = records[records.length - 1];
    latestEl.className = "";
    latestEl.textContent =
      "Latest run: " + (latest.timestamp || "").slice(0, 16).replace("T", " ") +
      " UTC, commit " + sha(latest) + ", " + ((latest.meta || {}).runner || "unknown") +
      " runner. " + records.length + " runs recorded.";

    renderCards(document.getElementById("perf-cards"), records, keys);
    const chartsEl = document.getElementById("perf-charts");
    renderCharts(chartsEl, records, keys);
    renderTable(document.getElementById("perf-runs"), records, keys);

    // The palette toggle recolours the page without a reload; the
    // canvases have to be redrawn to follow it.
    new MutationObserver(() => renderCharts(chartsEl, records, keys)).observe(
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
