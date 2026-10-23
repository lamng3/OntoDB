// Small helpers for the showcases: DOM, tabs, bars, a line chart. Everything is drawn from recorded data.
const $ = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => [...r.querySelectorAll(s)];
function h(tag, attrs = {}, ...kids) {
  const el = document.createElement(tag);
  if (tag === "button") el.type = "button";
  for (const [k, v] of Object.entries(attrs || {})) {
    if (k === "class") el.className = v;
    else if (k === "html") el.innerHTML = v;
    else if (k.startsWith("on")) el.addEventListener(k.slice(2), v);
    else if (v !== false && v != null) el.setAttribute(k, v === true ? "" : v);
  }
  for (const kid of kids.flat()) if (kid != null) el.append(kid.nodeType ? kid : document.createTextNode(kid));
  return el;
}
const f2 = (x) => (x == null ? "–" : Number(x).toFixed(2));
const usd = (x) => (x ? "$" + Number(x).toFixed(2) : "–");

// Tabs: <nav class="tabs"><button data-tab="id">…</button></nav> and <section class="tab" id="tab-id">. The hash picks a tab.
function tabs(onShow = {}) {
  const buttons = $$("nav.tabs button");
  const show = (id) => {
    buttons.forEach((b) => b.setAttribute("aria-selected", String(b.dataset.tab === id)));
    $$("section.tab").forEach((s) => (s.hidden = s.id !== "tab-" + id));
    if (onShow[id]) onShow[id]();
    history.replaceState(null, "", "#" + id);
  };
  buttons.forEach((b) => b.addEventListener("click", () => show(b.dataset.tab)));
  window.addEventListener("hashchange", () => {
    const id = location.hash.slice(1);
    if (buttons.some((b) => b.dataset.tab === id)) { show(id); scrollTo({ top: 0 }); }
  });
  const start = location.hash.slice(1);
  show(buttons.some((b) => b.dataset.tab === start) ? start : buttons[0].dataset.tab);
}

// Horizontal bars: rows of {label, value, max?, alt?, text?}. Values are shown as given.
function bars(el, rows, { max = 1, labelWidth = "7em" } = {}) {
  el.className = "bars";
  el.style.setProperty("--lw", labelWidth);
  el.replaceChildren(
    ...rows.map((r) => {
      const row = h("div", { class: "bar" + (r.alt ? " alt" : "") }, h("span", {}, r.label), h("i"), h("b", {}, r.text ?? f2(r.value)));
      row.children[1].style.setProperty("--w", Math.max(0, Math.min(100, (r.value / (r.max ?? max)) * 100)) + "%");
      return row;
    }),
  );
}

// Line chart: series of {name, color, points:[[x, y], …]}; x values are the same across series or sorted.
function lineChart(el, series, { xLabel = "", yLabel = "", yMax = 1, xTicks, width = 560, height = 260 } = {}) {
  const m = { l: 40, r: 12, t: 26, b: 34 };
  const xs = [...new Set(series.flatMap((s) => s.points.map((p) => p[0])))].sort((a, b) => a - b);
  const x0 = xs[0], x1 = xs[xs.length - 1] || 1;
  const X = (x) => m.l + ((x - x0) / (x1 - x0 || 1)) * (width - m.l - m.r);
  const Y = (y) => height - m.b - (y / yMax) * (height - m.t - m.b);
  const NS = "http://www.w3.org/2000/svg";
  const svg = document.createElementNS(NS, "svg");
  svg.setAttribute("viewBox", `0 0 ${width} ${height}`);
  svg.setAttribute("class", "chart");
  svg.setAttribute("role", "img");
  const add = (tag, attrs, text) => {
    const n = document.createElementNS(NS, tag);
    for (const [k, v] of Object.entries(attrs)) n.setAttribute(k, v);
    if (text != null) n.textContent = text;
    svg.append(n);
    return n;
  };
  for (let i = 0; i <= 4; i++) {
    const y = (yMax * i) / 4;
    add("line", { class: "grid", x1: m.l, x2: width - m.r, y1: Y(y), y2: Y(y) });
    add("text", { x: m.l - 6, y: Y(y) + 4, "text-anchor": "end" }, +y.toFixed(2));
  }
  for (const x of xTicks || xs) {
    add("text", { x: X(x), y: height - m.b + 16, "text-anchor": "middle" }, x);
  }
  add("line", { class: "axis", x1: m.l, x2: width - m.r, y1: Y(0), y2: Y(0) });
  if (xLabel) add("text", { x: (m.l + width - m.r) / 2, y: height - 4, "text-anchor": "middle" }, xLabel);
  if (yLabel) add("text", { x: m.l - 34, y: 12, "text-anchor": "start" }, yLabel);
  for (const s of series) {
    const d = s.points.map((p, i) => (i ? "L" : "M") + X(p[0]).toFixed(1) + " " + Y(p[1]).toFixed(1)).join(" ");
    add("path", { class: "line", d, style: `stroke:${s.color}`, ...(s.dash ? { "stroke-dasharray": s.dash } : {}) });
    for (const p of s.points) {
      const dot = add("circle", { class: "dot", cx: X(p[0]), cy: Y(p[1]), r: 3.5, style: `fill:${s.color}` });
      const title = document.createElementNS(NS, "title");
      title.textContent = `${s.name}: ${f2(p[1])} at ${p[0]}`;
      dot.append(title);
    }
  }
  const legend = h("div", { class: "legend" }, series.map((s) => h("span", { style: `--c:${s.color}` }, h("i"), s.name)));
  el.replaceChildren(svg, legend);
}
