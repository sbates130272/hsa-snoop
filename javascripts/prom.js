/* Render the Prometheus snapshot page from prom/snapshot.json.
 *
 * Written onto gh-pages by .github/ci/report/publish-perf.py after each
 * successful hardware-test run on main; fetched at runtime like the
 * performance history.
 *
 *   { generated, sha, run_id, runner,
 *     families: [ { name, type, help, series: [ { name, value } ] } ] }
 */

(function () {
  "use strict";

  const REPO = "https://github.com/sbates130272/hsa-snoop";
  const CANDIDATES = [
    "snapshot.json",
    "https://sbates130272.github.io/hsa-snoop/prom/snapshot.json",
  ];

  function el(tag, cls, text) {
    const node = document.createElement(tag);
    if (cls) node.className = cls;
    if (text != null) node.textContent = text;
    return node;
  }

  function fmt(value) {
    if (value == null) return "–";
    return Number.isInteger(value)
      ? value.toLocaleString("en-US")
      : Number(value).toPrecision(6).replace(/\.?0+$/, "");
  }

  async function load() {
    for (const url of CANDIDATES) {
      try {
        const res = await fetch(url, { cache: "no-store" });
        if (res.ok) return await res.json();
      } catch (e) {
        /* try the next candidate */
      }
    }
    return null;
  }

  async function main() {
    const metaEl = document.getElementById("prom-meta");
    if (!metaEl) return; // not the snapshot page

    const snap = await load();
    if (!snap || !(snap.families || []).length) {
      metaEl.textContent =
        "No snapshot has been published yet. The next successful " +
        "hardware-test run on main will publish one.";
      return;
    }

    metaEl.className = "";
    metaEl.replaceChildren(
      "Captured " + (snap.generated || "").slice(0, 16).replace("T", " ") + " UTC",
    );
    if (snap.sha) {
      const a = el("a", null, snap.sha.slice(0, 8));
      a.href = REPO + "/commit/" + snap.sha;
      metaEl.append(", commit ", a);
    }
    if (snap.run_id) {
      const a = el("a", null, snap.run_id);
      a.href = REPO + "/actions/runs/" + snap.run_id;
      metaEl.append(", run ", a);
    }
    metaEl.append(", " + (snap.runner || "unknown") + " runner.");

    const root = document.getElementById("prom-families");
    for (const family of snap.families) {
      const heading = el("h2");
      heading.append(el("code", null, family.name));
      if (family.type) heading.append(el("span", "prom-type", family.type));
      root.append(heading);
      if (family.help) root.append(el("p", null, family.help));

      const table = document.createElement("table");
      const head = table.createTHead().insertRow();
      head.append(el("th", null, "Series"), el("th", "num", "Value"));
      const body = table.createTBody();
      for (const s of family.series || []) {
        const row = body.insertRow();
        const name = el("td");
        name.append(el("code", null, s.name));
        row.append(name);
        row.append(el("td", "num", fmt(s.value)));
      }
      root.append(table);
    }
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", main);
  } else {
    main();
  }
})();
