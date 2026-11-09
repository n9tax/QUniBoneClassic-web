/* QBone control panel - front end. Plain JS, no build step, no libraries.
   Talks to qbcontrold's JSON API (see qbcontrol/server.py). */
"use strict";

// ------------------------------------------------------------------ helpers

const $ = (sel, root) => (root || document).querySelector(sel);
const $$ = (sel, root) => Array.from((root || document).querySelectorAll(sel));

function el(tag, attrs, ...kids) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v === null || v === undefined || v === false) continue;
    if (k === "class") e.className = v;
    else if (k === "text") e.textContent = v;
    else if (k.startsWith("on")) e.addEventListener(k.slice(2), v);
    else if (k === "value") e.value = v;
    else if (k === "checked") e.checked = !!v;
    else e.setAttribute(k, v === true ? "" : v);
  }
  for (const kid of kids.flat()) {
    if (kid === null || kid === undefined || kid === false) continue;
    e.append(kid instanceof Node ? kid : document.createTextNode(String(kid)));
  }
  return e;
}

const clone = (o) => JSON.parse(JSON.stringify(o));
const store = {
  get(k, d) { try { const v = localStorage.getItem("qbc." + k); return v === null ? d : JSON.parse(v); } catch (e) { return d; } },
  set(k, v) { try { localStorage.setItem("qbc." + k, JSON.stringify(v)); } catch (e) { /* private mode */ } },
};

async function api(method, path, body) {
  const opt = { method, headers: {} };
  if (body !== undefined) { opt.body = JSON.stringify(body); opt.headers["Content-Type"] = "application/json"; }
  let r;
  try { r = await fetch("/api/" + path, opt); }
  catch (e) { setLink(false); throw new Error("No connection to the control daemon"); }
  setLink(true);
  let data = {};
  try { data = await r.json(); } catch (e) { /* empty */ }
  if (!r.ok) throw new Error(data.error || ("HTTP " + r.status));
  return data;
}
const GET = (p) => api("GET", p);
const POST = (p, b) => api("POST", p, b || {});
const PUT = (p, b) => api("PUT", p, b);
const DEL = (p) => api("DELETE", p);

let toastTimer = null;
function toast(msg, isErr) {
  const t = $("#toast");
  t.textContent = msg;
  t.classList.toggle("err", !!isErr);
  t.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { t.hidden = true; }, isErr ? 6000 : 2800);
}
const fail = (e) => toast(e.message || String(e), true);

function confirmBox(title, text, yes) {
  const d = $("#dlg-confirm");
  $("#confirm-title").textContent = title;
  $("#confirm-text").textContent = text;
  $("#confirm-yes").textContent = yes || "YES";
  d.showModal();
  return new Promise((resolve) => {
    const done = (v) => { d.close(); $("#confirm-yes").onclick = $("#confirm-no").onclick = null; resolve(v); };
    $("#confirm-yes").onclick = () => done(true);
    $("#confirm-no").onclick = () => done(false);
    d.oncancel = () => done(false);
  });
}

function printout(title, text) {
  $("#print-title").textContent = title;
  $("#print-body").textContent = text;
  $("#dlg-print").showModal();
}
$$("[data-close]").forEach((b) => b.addEventListener("click", () => b.closest("dialog").close()));

function fmtSize(n) {
  if (!n) return "";
  if (n >= 1 << 20) return (n / (1 << 20)).toFixed(1) + " MB";
  return Math.round(n / 1024) + " KB";
}
function fmtDur(s) {
  s = Math.max(0, Math.floor(s));
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), x = s % 60;
  return (h ? h + "h " : "") + (h || m ? m + "m " : "") + x + "s";
}

// ------------------------------------------------------------------ state

const S = {
  status: null,
  catalog: null,
  configs: [],
  stock: [],
  stockDir: "",
  images: null,
  imagesAt: 0,
  view: "operate",
  cur: null,          // configure editor: {id, cfg, saved}
};

function isRunning() { return S.status && S.status.runner.state !== "stopped"; }

// ------------------------------------------------------------------ navigation

function showView(v) {
  if (S.view === "configure" && v !== "configure" && S.cur && dirty()) {
    // keep edits in memory; just remind
    toast("Unsaved changes in \"" + S.cur.cfg.name + "\"");
  }
  S.view = v;
  $$(".view").forEach((m) => { m.hidden = m.id !== "view-" + v; });
  $$("#nav .paddle-nav").forEach((b) => b.setAttribute("aria-current", b.dataset.view === v ? "page" : "false"));
  store.set("view", v);
  if (!location.hash.startsWith("#" + v)) history.replaceState(null, "", "#" + v);
  if (v === "library") loadStock().then(renderLibrary).catch(fail);
  if (v === "configure") renderCfgList();
  if (v === "autostart") loadAutostart().catch(fail);
  if (v === "system") loadSystem().catch(fail);
  if (v === "operate") setTimeout(() => $("#crt-in").focus(), 0);
}
$$("#nav .paddle-nav").forEach((b) => b.addEventListener("click", () => showView(b.dataset.view)));

// ------------------------------------------------------------------ status / lamps

function setLink(ok) { $("#lamp-link").classList.toggle("on", ok); }

async function pollStatus() {
  try {
    S.status = await GET("status");
    renderStatus();
  } catch (e) { /* link lamp shows it */ }
  setTimeout(pollStatus, 2000);
}

function renderStatus() {
  const st = S.status, r = st.runner;
  const running = r.state === "running", stopping = r.state === "stopping";
  $("#lamp-run").classList.toggle("on", running || stopping);
  $("#lamp-run").classList.toggle("blink", stopping);
  $("#lamp-stop").classList.toggle("on", r.state === "stopped");
  $("#lamp-auto").classList.toggle("on", st.autostart.mode !== "off");
  $("#bus-label").textContent = st.platform;
  $("#bus-sub").textContent = st.platform === "QBUS" ? st.address_width + "-BIT" : "18-BIT";
  $("#brand-sub").textContent = "PDP-11 · " + st.platform + " · " + (st.platform === "QBUS" ? "QBONE" : "UNIBONE");
  document.title = (running ? "▶ " + r.label + " – " : "") + (st.platform === "QBUS" ? "QBone" : "UniBone") + " Control Panel";
  $("#foot-ver").textContent = st.version;
  $("#foot-host").textContent = st.hostname + "  " + (st.ips || []).map((ip) => ip + ":" + st.port).join("  ");
  $$(".qbus-only").forEach((b) => { b.hidden = st.platform !== "QBUS"; });

  // operate panel
  const bs = $("#op-state");
  bs.textContent = running ? "RUN" : stopping ? "STOPPING" : "HALT";
  bs.className = "big-state" + (running ? " run" : stopping ? " stopping" : "");
  $("#op-label").textContent = r.label ? r.label : "Nothing running";
  let meta = "";
  if (running) meta = "pid " + r.pid + " · started " + new Date(r.started_at * 1000).toLocaleTimeString();
  else if (r.exit_code !== null && r.exit_code !== undefined) meta = "exited with code " + r.exit_code + " at " + new Date(r.stopped_at * 1000).toLocaleTimeString();
  $("#op-meta").textContent = meta;
  $("#op-uptime").textContent = running ? "UP " + fmtDur(Date.now() / 1000 - r.started_at) : "";
  $("#btn-start").disabled = running || stopping;
  $("#btn-stop").disabled = !running;
  $("#btn-kill").disabled = r.state === "stopped";
  $("#btn-restart").disabled = !r.target || stopping;
  $("#btn-showcmd").disabled = !r.cmdfile;
  $("#crt-in").disabled = !running;
  $("#crt-in").placeholder = running ? "type a demo command, RETURN sends" : "nothing running";

  // banner for installation problems
  const msgs = [];
  if (!st.demo_ok) msgs.push(["demo program not found", " at " + st.demo_bin + ". Build it on the BeagleBone with ./compile.sh."]);
  else if (!st.is_root && !st.fake_demo) msgs.push(["Daemon is not running as root.", " demo needs root to load the PRU code; start qbcontrold via systemd (see README)."]);
  const bn = $("#banner");
  bn.replaceChildren(...msgs.map(([b, t]) => el("div", {}, el("b", { text: b }), t)));
  bn.hidden = !msgs.length;

  if (S.view === "autostart") renderDip(st.switches);
  if (S.view === "configure") renderCfgList();
}

// ------------------------------------------------------------------ targets

async function loadConfigs() { S.configs = (await GET("configs")).configs; }
async function loadStock() { const r = await GET("stock"); S.stock = r.stock; S.stockDir = r.apps_dir; }

function targetLabel(t) {
  if (!t) return "";
  const [kind, ref] = [t.slice(0, t.indexOf(":")), t.slice(t.indexOf(":") + 1)];
  if (kind === "config") { const c = S.configs.find((x) => x.id === ref); return c ? c.name : ref + " (deleted)"; }
  const s = S.stock.find((x) => x.id === ref); return s ? s.name : ref;
}

function fillTargetSelect(sel, value, withNone) {
  const opts = [];
  if (withNone) opts.push(el("option", { value: "", text: "— nothing —" }));
  if (S.configs.length) {
    opts.push(el("optgroup", { label: "My configurations" },
      S.configs.map((c) => el("option", { value: "config:" + c.id, text: c.name }))));
  }
  const groups = {};
  S.stock.forEach((s) => { (groups[s.group] = groups[s.group] || []).push(s); });
  Object.keys(groups).sort().forEach((g) => {
    opts.push(el("optgroup", { label: "Stock · " + g },
      groups[g].map((s) => el("option", { value: "stock:" + s.id, text: s.name + (s.ready ? "" : "  (images missing)") + (s.cpu ? "  [CPU]" : "") }))));
  });
  sel.replaceChildren(...opts);
  if (value && !Array.from(sel.options).some((o) => o.value === value)) {
    sel.append(el("option", { value, text: targetLabel(value) + " (missing)" }));
  }
  sel.value = value || "";
}

function renderOperate() {
  const sel = $("#op-target");
  const last = store.get("lastTarget", "");
  fillTargetSelect(sel, sel.value || last || (S.configs[0] ? "config:" + S.configs[0].id : ""), false);
  const q = $("#op-quick");
  const items = [];
  S.configs.slice(0, 8).forEach((c) => items.push(el("button", { onclick: () => startTarget("config:" + c.id) },
    el("span", { text: c.name }), el("small", { text: c.kind === "raw" ? "RAW" : "CONFIG" }))));
  const ready = S.stock.filter((s) => s.ready).slice(0, 6);
  ready.forEach((s) => items.push(el("button", { class: "stock", onclick: () => startTarget("stock:" + s.id) },
    el("span", { text: s.name }), el("small", { text: "STOCK" }))));
  q.replaceChildren(...(items.length ? [el("div", { class: "label", text: "Quick start" }), ...items] : []));
}

async function waitStopped(timeoutMs) {
  const end = Date.now() + (timeoutMs || 30000);
  while (Date.now() < end) {
    S.status = await GET("status");
    renderStatus();
    if (S.status.runner.state === "stopped") return true;
    await new Promise((r) => setTimeout(r, 500));
  }
  return false;
}

async function startTarget(target) {
  if (!target) { toast("Select something to start", true); return; }
  try {
    if (isRunning()) {
      const ok = await confirmBox("STOP AND START",
        "\"" + S.status.runner.label + "\" is running. Stop it and start \"" + targetLabel(target) + "\"?", "STOP & START");
      if (!ok) return;
      await POST("stop");
      if (!(await waitStopped())) { toast("demo did not stop; try KILL", true); return; }
    }
    store.set("lastTarget", target);
    await POST("start", { target });
    toast("START " + targetLabel(target));
    S.status = await GET("status");
    renderStatus();
    if (S.view !== "operate") showView("operate");
  } catch (e) { fail(e); }
}

$("#btn-start").addEventListener("click", () => startTarget($("#op-target").value));
$("#btn-stop").addEventListener("click", () => POST("stop").then(() => toast("STOP: leaving demo menus")).catch(fail));
$("#btn-kill").addEventListener("click", async () => {
  if (await confirmBox("KILL", "Kill demo with SIGTERM/SIGKILL without leaving its menus? Use this only if STOP does not work.", "KILL"))
    POST("stop", { force: true }).catch(fail);
});
$("#btn-restart").addEventListener("click", async () => {
  const t = S.status && S.status.runner.target;
  if (!t) return;
  try {
    if (isRunning()) { await POST("stop"); if (!(await waitStopped())) { toast("demo did not stop", true); return; } }
    await POST("start", { target: t });
    toast("RESTART " + targetLabel(t));
  } catch (e) { fail(e); }
});
$("#btn-showcmd").addEventListener("click", async () => {
  const r = S.status.runner;
  if (r.target && r.target.startsWith("stock:")) {
    try { const x = await GET("stock/cmd?id=" + encodeURIComponent(r.target.slice(6))); printout(r.cmdfile, x.cmd); } catch (e) { fail(e); }
  } else if (r.target && r.target.startsWith("config:")) {
    try {
      const c = (await GET("configs/" + encodeURIComponent(r.target.slice(7)))).config;
      const p = await POST("preview", { config: c, id: c.id });
      printout(r.cmdfile, p.cmd || p.error);
    } catch (e) { fail(e); }
  }
});

// ------------------------------------------------------------------ console (VT emulation, minimal)

const term = {
  lines: [""],
  notes: [false],
  col: 0,
  pending: "",
  noteMode: false,
  nodes: [],
  dirtyFrom: 0,
  MAX: 3000,

  write(data) {
    data = this.pending + data;
    this.pending = "";
    let i = 0;
    let cur = this.lines.length - 1;
    this.dirtyFrom = Math.min(this.dirtyFrom, cur);
    while (i < data.length) {
      const ch = data[i];
      if (ch === "\x1b") {
        const m = /^\x1b\[([0-9;?]*)([A-Za-z])/.exec(data.slice(i, i + 16));
        if (!m) {
          if (data.length - i < 16) { this.pending = data.slice(i); break; }
          i++; continue;
        }
        if (m[2] === "m") {
          if (m[1] === "7") { this.noteMode = true; this.notes[cur] = true; }
          else if (m[1] === "0" || m[1] === "") this.noteMode = false;
        } else if (m[2] === "J" && m[1] === "2") { this.clear(); cur = 0; }
        i += m[0].length;
        continue;
      }
      if (ch === "\n") {
        this.lines.push(""); this.notes.push(this.noteMode); cur++; this.col = 0;
      } else if (ch === "\r") {
        this.col = 0;
      } else if (ch === "\b") {
        this.col = Math.max(0, this.col - 1);
      } else if (ch === "\t") {
        const n = 8 - (this.col % 8);
        this.put(cur, " ".repeat(n));
      } else if (ch >= " " || ch > "\x7f") {
        // collect a run of printable characters
        let j = i;
        while (j < data.length && data[j] >= " " && data[j] !== "\x7f" && data[j] !== "\x1b") j++;
        if (j === i) j = i + 1;
        this.put(cur, data.slice(i, j));
        i = j;
        continue;
      }
      i++;
    }
    if (this.lines.length > this.MAX) {
      const cut = this.lines.length - this.MAX;
      this.lines.splice(0, cut); this.notes.splice(0, cut);
      this.nodes.splice(0, cut).forEach((n) => n.remove());
      this.dirtyFrom = Math.max(0, this.dirtyFrom - cut);
    }
    this.render();
  },

  put(row, s) {
    const line = this.lines[row];
    const pad = this.col > line.length ? " ".repeat(this.col - line.length) : "";
    this.lines[row] = line.slice(0, this.col) + pad + s + line.slice(this.col + s.length);
    this.col += s.length;
  },

  clear() {
    this.lines = [""]; this.notes = [false]; this.col = 0;
    this.nodes.forEach((n) => n.remove()); this.nodes = []; this.dirtyFrom = 0;
  },

  render() {
    const pre = $("#crt-text");
    const atBottom = pre.scrollHeight - pre.scrollTop - pre.clientHeight < 40;
    for (let r = this.dirtyFrom; r < this.lines.length; r++) {
      let n = this.nodes[r];
      if (!n) { n = document.createElement("span"); this.nodes[r] = n; pre.append(n); }
      const text = this.lines[r] + (r < this.lines.length - 1 ? "\n" : "");
      if (n.textContent !== text) n.textContent = text;
      n.className = this.notes[r] ? "note" : "";
    }
    this.dirtyFrom = this.lines.length - 1;
    if (atBottom) pre.scrollTop = pre.scrollHeight;
  },
};

let conNext = null;
async function consoleLoop() {
  for (;;) {
    try {
      if (conNext === null) {
        const st = S.status || await GET("status");
        conNext = Math.max(0, st.runner.console_end - 65536);
      }
      const r = await GET("console?since=" + conNext + "&wait=20");
      if (r.data) term.write(r.data);
      conNext = r.next;
    } catch (e) {
      await new Promise((res) => setTimeout(res, 2000));
    }
  }
}

const hist = store.get("history", []);
let histPos = hist.length;
$("#crt-form").addEventListener("submit", (ev) => {
  ev.preventDefault();
  const inp = $("#crt-in");
  const line = inp.value;
  sendLine(line);
  if (line.trim() && hist[hist.length - 1] !== line) { hist.push(line); if (hist.length > 100) hist.shift(); store.set("history", hist); }
  histPos = hist.length;
  inp.value = "";
});
$("#crt-in").addEventListener("keydown", (ev) => {
  const inp = ev.target;
  if (ev.key === "ArrowUp" && histPos > 0) { histPos--; inp.value = hist[histPos]; ev.preventDefault(); }
  else if (ev.key === "ArrowDown") { histPos = Math.min(hist.length, histPos + 1); inp.value = hist[histPos] || ""; ev.preventDefault(); }
});
function sendLine(line) {
  if (!isRunning()) { toast("Nothing running", true); return; }
  POST("console", { text: line + "\n" }).catch(fail);
}
$$("#keypad [data-cmd]").forEach((b) => b.addEventListener("click", () => { sendLine(b.dataset.cmd); $("#crt-in").focus(); }));
$("#crt-clear").addEventListener("click", () => { term.clear(); $("#crt-in").focus(); });
$("#crt-help").addEventListener("click", () => sendLine("?"));
$("#crt").addEventListener("click", (ev) => { if (!window.getSelection().toString()) $("#crt-in").focus(); });

function setPhosphor(ph) {
  $("#crt").className = "crt ph-" + ph;
  $$(".phosphor button").forEach((b) => b.setAttribute("aria-checked", b.dataset.ph === ph ? "true" : "false"));
  store.set("phosphor", ph);
}
$$(".phosphor button").forEach((b) => { b.setAttribute("role", "radio"); b.addEventListener("click", () => setPhosphor(b.dataset.ph)); });

// ------------------------------------------------------------------ configure

function dirty() { return S.cur && JSON.stringify(S.cur.cfg) !== S.cur.saved; }

function renderCfgList() {
  const box = $("#cfg-list");
  const runT = S.status && isRunning() ? S.status.runner.target : "";
  const autoT = S.status ? S.status.autostart.target : "";
  if (!S.configs.length) {
    box.replaceChildren(el("p", { class: "hint", text: "No configurations yet." }));
    return;
  }
  box.replaceChildren(...S.configs.map((c) => el("button", {
    class: "cfg-item", "aria-current": S.cur && S.cur.id === c.id ? "true" : "false",
    onclick: () => openConfig(c.id),
  },
  el("b", {}, c.name,
    runT === "config:" + c.id ? el("span", { class: "tag run", text: "RUN" }) : null,
    autoT === "config:" + c.id && S.status.autostart.mode === "fixed" ? el("span", { class: "tag", text: "AUTO" }) : null,
    c.kind === "raw" ? el("span", { class: "tag", text: "RAW" }) : null),
  el("small", { text: c.description || (c.kind === "raw" ? "hand written command file" : "") }))));
}

function normalizeBuilder(cfg) {
  const cat = S.catalog;
  cfg.memory = cfg.memory || { emulate: true, end_addr: "" };
  cfg.boot = cfg.boot || { mode: "auto", file: "" };
  cfg.controllers = cfg.controllers || {};
  cat.controllers.forEach((c) => {
    const cc = cfg.controllers[c.key] = cfg.controllers[c.key] || { enabled: false, drives: [] };
    // a drive card present in the editor is an emulated drive: fill defaults
    cc.drives = (cc.drives || []).map((d) => Object.assign(newDrive(c, Number(d.unit) || 0), d, { enabled: true }));
    cc.drives.forEach((d) => { if (!c.drive_types.includes(d.type)) d.type = c.default_type; });
  });
  cfg.serial = cfg.serial || {};
  ["dl11", "dl11b"].forEach((k) => {
    const def = cat.serial.find((s) => s.key === k);
    cfg.serial[k] = cfg.serial[k] || { enabled: false, port: def.default_port, baud: def.default_baud };
  });
  if (cfg.serial.kw11 === undefined) cfg.serial.kw11 = false;
  return cfg;
}

async function openConfig(id) {
  if (S.cur && dirty() && S.cur.id !== id) {
    if (!(await confirmBox("UNSAVED CHANGES", "Discard the changes to \"" + S.cur.cfg.name + "\"?", "DISCARD"))) return;
  }
  try {
    const cfg = (await GET("configs/" + encodeURIComponent(id))).config;
    delete cfg.id;
    if (cfg.kind !== "raw") normalizeBuilder(cfg);
    S.cur = { id, cfg, saved: JSON.stringify(cfg) };
    store.set("curConfig", id);
    if (S.view === "configure") history.replaceState(null, "", "#configure/" + encodeURIComponent(id));
    renderEditor();
    renderCfgList();
  } catch (e) { fail(e); }
}

// small builders for form widgets bound to an object property
function bindText(obj, key, attrs) {
  return el("input", Object.assign({ value: obj[key] == null ? "" : obj[key], spellcheck: "false",
    oninput: (ev) => { obj[key] = ev.target.type === "number" ? (ev.target.value === "" ? "" : Number(ev.target.value)) : ev.target.value; changed(); } }, attrs || {}));
}
function bindSelect(obj, key, options, onchange) {
  const s = el("select", { onchange: (ev) => { obj[key] = ev.target.value; changed(); if (onchange) onchange(); } },
    options.map((o) => Array.isArray(o) ? el("option", { value: o[0], text: o[1] }) : el("option", { value: o, text: o })));
  s.value = obj[key] == null ? "" : obj[key];
  return s;
}
function paddle(obj, key, label, sub, opts) {
  opts = opts || {};
  const inp = el("input", { type: "checkbox", checked: !!obj[key], disabled: opts.disabled,
    onchange: (ev) => { obj[key] = ev.target.checked; changed(); if (opts.rerender) renderEditor(); } });
  return el("label", { class: "sw" + (opts.violet ? " v" : "") }, inp, el("i", { class: "pad" }),
    el("span", { class: "txt" }, label, sub ? el("small", { text: sub }) : null));
}
function field(label, input, cls) { return el("label", { class: "field" + (cls ? " " + cls : "") }, el("span", { text: label }), input); }

let previewTimer = null;
function changed() {
  const d = $("#cfg-dirty");
  if (d) d.hidden = !dirty();
  clearTimeout(previewTimer);
  previewTimer = setTimeout(updatePreview, 350);
  // drive lamps
  $$(".drive").forEach((dv) => dv._lamps && dv._lamps());
}

async function updatePreview() {
  const pre = $("#cfg-preview");
  if (!pre || !S.cur) return;
  try {
    const r = await POST("preview", { config: S.cur.cfg, id: S.cur.id });
    if (r.error) pre.replaceChildren(el("span", { class: "err", text: "?ERROR: " + r.error }));
    else pre.textContent = r.cmd;
  } catch (e) { pre.textContent = e.message; }
}

function renderEditor() {
  const box = $("#cfg-editor");
  if (!S.cur) {
    box.replaceChildren(el("div", { class: "unit empty-editor" }, el("p", {}, "Select a configuration, or press ", el("b", { text: "NEW" }), ".")));
    return;
  }
  const cfg = S.cur.cfg;
  const parts = [];

  parts.push(el("section", { class: "unit" },
    el("div", { class: "unit-head" }, el("h2", { text: cfg.kind === "raw" ? "RAW COMMAND FILE" : "CONFIGURATION" }),
      el("span", { class: "silk-dim", text: S.cur.id })),
    el("div", { class: "row2" },
      field("Name", bindText(cfg, "name")),
      field("Description", bindText(cfg, "description", { placeholder: "e.g. RT-11 V5.5 for the 11/73" })))));

  if (cfg.kind === "raw") {
    parts.push(el("section", { class: "unit" },
      el("div", { class: "unit-head" }, el("h2", { text: "COMMAND FILE" }), el("span", { class: "silk-dim", text: "demo --cmdfile" })),
      field("Working directory (images and boot loaders are found relative to it)", bindText(cfg, "workdir", { placeholder: "default: " + "~/.qbcontrol/work/" + S.cur.id })),
      field("Commands, executed line by line as typed at the device menu",
        el("textarea", { rows: 22, spellcheck: "false", value: cfg.cmd_text || "", oninput: (ev) => { cfg.cmd_text = ev.target.value; changed(); } })),
      el("p", { class: "hint" }, "First line selects the menu: ", el("code", { text: "d" }), " devices, ",
        el("code", { text: "dc" }), " devices + emulated CPU. Internal commands: ", el("code", { text: ".wait ms" }), ", ",
        el("code", { text: ".print text" }), ", ", el("code", { text: ".end" }), ". After the file, demo reads the console.")));
  } else {
    parts.push(...builderSections(cfg));
  }

  parts.push(el("section", { class: "unit" },
    el("div", { class: "unit-head" }, el("h2", { text: "PRINTOUT" }), el("span", { class: "silk-dim", text: "generated " + S.cur.id + ".cmd" })),
    el("div", { class: "printout tall" }, el("pre", { id: "cfg-preview", text: "…" }))));

  parts.push(el("div", { class: "actionbar" },
    el("button", { class: "key key-start", onclick: saveConfig }, "SAVE"),
    el("button", { class: "key key-start", onclick: () => saveConfig().then((ok) => ok && startTarget("config:" + S.cur.id)) }, "SAVE & START"),
    el("button", { class: "key", onclick: duplicateConfig }, "DUPLICATE"),
    el("span", { class: "dirty", id: "cfg-dirty", hidden: true, text: "● UNSAVED" }),
    el("span", { class: "spacer" }),
    el("button", { class: "key key-kill", onclick: deleteConfig }, "DELETE")));

  box.replaceChildren(...parts);
  updatePreview();
}

function builderSections(cfg) {
  const cat = S.catalog;
  const out = [];
  const qbus = cat.bus === "QBUS";

  // --- system
  const mem = cfg.memory;
  out.push(el("section", { class: "unit" },
    el("div", { class: "unit-head" }, el("h2", { text: "SYSTEM" }), el("span", { class: "silk-dim", text: cat.bus })),
    el("div", { class: "toggle-row" },
      paddle(cfg, "power_cycle", "Power cycle", "reset the PDP-11 on start (pwr)"),
      paddle(mem, "emulate", "Emulate memory", "fill missing memory with BeagleBone DDR (m i)", { rerender: true }),
      paddle(cfg, "cpu20", "Emulate CPU", cat.cpu20 ? "PDP-11/20 on the UniBone; physical CPU must be removed"
        : "not available on QBUS", { disabled: !cat.cpu20, violet: true })),
    el("div", { class: "row3" },
      field("Reset wait (ms)", bindText(cfg, "power_wait_ms", { type: "number", min: 0, step: 500 })),
      mem.emulate ? field("Memory up to (octal, empty = all)", bindText(mem, "end_addr", { placeholder: qbus ? "e.g. 17757776" : "e.g. 757776" })) : el("span"),
      field("Spin-up wait (ms)", bindText(cfg, "spinup_wait_ms", { type: "number", min: 0, step: 500 })))));

  // --- mass storage
  const racks = cat.controllers.map((c) => controllerRack(cfg, c));
  out.push(el("section", { class: "unit" },
    el("div", { class: "unit-head" }, el("h2", { text: "MASS STORAGE" }), el("span", { class: "silk-dim", text: "controllers and drives" })),
    racks));

  // --- serial
  const ser = cfg.serial;
  const serRows = cat.serial.map((s) => {
    const o = ser[s.key];
    return el("div", { class: "rack" + (o.enabled ? " on" : "") },
      el("div", { class: "rack-head" }, el("i", { class: "screw" }),
        el("label", { class: "sw v" }, el("input", { type: "checkbox", checked: o.enabled, onchange: (ev) => { o.enabled = ev.target.checked; changed(); renderEditor(); } }), el("i", { class: "pad" })),
        el("span", { class: "model", text: s.key.toUpperCase() }), el("span", { class: "title", text: s.label }),
        el("span", { class: "addr", text: s.address }), el("i", { class: "screw" })),
      o.enabled ? el("div", { class: "rack-body" }, el("div", { class: "row2" },
        field("BeagleBone serial port", bindText(o, "port", { placeholder: s.default_port })),
        field("Baud rate", bindSelect(o, "baud", [300, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200].map((b) => [b, String(b)]), null)))) : null);
  });
  out.push(el("section", { class: "unit" },
    el("div", { class: "unit-head" }, el("h2", { text: "SERIAL LINES & CLOCK" }), el("span", { class: "silk-dim", text: "emulated DL11 on a BeagleBone UART" })),
    serRows,
    el("div", { class: "toggle-row" }, paddle(ser, "kw11", "KW11 line clock", "50/60 Hz LTC interrupts", { violet: true }))));

  // --- boot
  const boot = cfg.boot;
  const enabledCtl = cat.controllers.filter((c) => cfg.controllers[c.key].enabled);
  const bootRow = [field("Boot loader", bindSelect(boot, "mode", [["auto", "From a controller"], ["file", "Choose file"], ["none", "None"]], renderEditor))];
  if (boot.mode === "auto") {
    bootRow.push(field("Controller", bindSelect(boot, "device",
      [["", "first enabled"]].concat(enabledCtl.map((c) => [c.key, c.label + " – " + c.boot])), null)));
  } else if (boot.mode === "file") {
    bootRow.push(field("Listing in 10.02_devices/5_boot (or a path)",
      el("span", {}, bindText(boot, "file", { list: "bootfiles", placeholder: "dl.lst" }),
        el("datalist", { id: "bootfiles" }, cat.boot_files.map((f) => el("option", { value: f }))))));
  }
  out.push(el("section", { class: "unit" },
    el("div", { class: "unit-head" }, el("h2", { text: "BOOTSTRAP" }), el("span", { class: "silk-dim", text: "m ll – MACRO-11 listing deposited at 10000" })),
    el("div", { class: "row2" }, bootRow),
    el("p", { class: "hint" }, "At the PDP-11 console, start the loader at ", el("code", { text: "10000" }),
      " to boot unit 0, ", el("code", { text: "10010" }), " for unit 1, and so on.")));

  // --- extra
  out.push(el("section", { class: "unit" },
    el("div", { class: "unit-head" }, el("h2", { text: "EXTRA COMMANDS" }), el("span", { class: "silk-dim", text: "appended verbatim" })),
    field("Working directory (relative image paths and shared dirs)", bindText(cfg, "workdir", { placeholder: "default: ~/.qbcontrol/work/" + S.cur.id })),
    field("Device menu commands",
      el("textarea", { rows: 4, spellcheck: "false", value: cfg.extra_commands || "", placeholder: "sd rl0\np", oninput: (ev) => { cfg.extra_commands = ev.target.value; changed(); } }))));
  return out;
}

function controllerRack(cfg, c) {
  const cc = cfg.controllers[c.key];
  const head = el("div", { class: "rack-head" },
    el("i", { class: "screw" }),
    el("label", { class: "sw", title: "Enable " + c.label },
      el("input", { type: "checkbox", checked: cc.enabled, "aria-label": "Enable " + c.label,
        onchange: (ev) => {
          cc.enabled = ev.target.checked;
          if (cc.enabled && !cc.drives.length) cc.drives.push(newDrive(c, 0));
          changed(); renderEditor();
        } }),
      el("i", { class: "pad" })),
    el("span", { class: "model", text: c.label }),
    el("span", { class: "title", text: c.title }),
    el("span", { class: "addr", text: c.address }),
    el("i", { class: "screw" }));
  if (!cc.enabled) return el("div", { class: "rack" }, head);

  const used = new Set(cc.drives.map((d) => Number(d.unit)));
  const free = [];
  for (let u = 0; u < c.drive_count; u++) if (!used.has(u)) free.push(u);
  cc.drives.sort((a, b) => a.unit - b.unit);
  const body = el("div", { class: "rack-body" },
    cc.drives.map((d) => driveCard(c, cc, d)),
    free.length ? el("div", { class: "slots" }, el("span", { class: "label", text: "Add drive" }),
      free.map((u) => el("button", { class: "slot-btn", onclick: () => { cc.drives.push(newDrive(c, u)); changed(); renderEditor(); } }, "+ " + c.drive_prefix + u))) : null);
  return el("div", { class: "rack on" }, head, body);
}

function newDrive(c, unit) {
  return { unit, enabled: true, image: "", type: c.default_type, speed: 10, useimagesize: true, imagetrack0: true, shared_fs: "", shared_dir: "", writeprotect: false };
}

function driveCard(c, cc, d) {
  const f = c.features;
  const lampLoad = el("span", { class: "rocker amber", text: "LOAD" });
  const lampReady = el("span", { class: "rocker white", text: "READY" });
  const lampWp = f.includes("writeprotect") ? el("span", { class: "rocker amber", text: "WP" }) : null;
  const imgInput = bindText(d, "image", { placeholder: "disk image file", "aria-label": c.drive_prefix + d.unit + " image" });
  const rows = [];
  rows.push(el("div", { class: "row" },
    el("div", { class: "imgpick" }, imgInput,
      el("button", { class: "key", onclick: () => pickImage(c, (p) => { d.image = p; imgInput.value = p; changed(); }) }, "BROWSE"))));
  const r2 = [];
  if (f.includes("type")) r2.push(field("Type", bindSelect(d, "type", c.drive_types, null)));
  if (f.includes("speed")) r2.push(field("Spin-up speed ×", bindText(d, "speed", { type: "number", min: 1, max: 50 })));
  if (f.includes("shared")) {
    r2.push(field("Share with Linux dir", bindSelect(d, "shared_fs", [["", "no"]].concat(S.catalog.shared_filesystems.map((x) => [x, x + " filesystem"])), () => renderEditor())));
    if (d.shared_fs) r2.push(field("Shared directory", bindText(d, "shared_dir", { placeholder: "shared_" + c.drive_prefix + d.unit })));
  }
  if (r2.length) rows.push(el("div", { class: "row" }, r2));
  const r3 = [];
  if (f.includes("useimagesize")) r3.push(el("label", { class: "check" }, el("input", { type: "checkbox", checked: d.useimagesize, onchange: (ev) => { d.useimagesize = ev.target.checked; changed(); } }), "Size from image file"));
  if (f.includes("imagetrack0")) r3.push(el("label", { class: "check" }, el("input", { type: "checkbox", checked: d.imagetrack0, onchange: (ev) => { d.imagetrack0 = ev.target.checked; changed(); } }), "Image includes track 0"));
  if (f.includes("writeprotect")) r3.push(el("label", { class: "check" }, el("input", { type: "checkbox", checked: d.writeprotect, onchange: (ev) => { d.writeprotect = ev.target.checked; changed(); } }), "Write protect"));
  r3.push(el("button", { class: "key remove", onclick: () => { cc.drives.splice(cc.drives.indexOf(d), 1); if (!cc.drives.length) cc.enabled = false; changed(); renderEditor(); } }, "REMOVE"));
  rows.push(el("div", { class: "row" }, r3));
  if (d.image && /\s|#/.test(d.image)) rows.push(el("div", { class: "warn-line", text: "Blanks and # are not possible in demo commands; rename the file." }));

  const card = el("div", { class: "drive" },
    el("div", { class: "plate" }, el("div", { class: "unitno", text: c.drive_prefix.toUpperCase() + d.unit }),
      el("div", { class: "btns" }, lampLoad, lampReady, lampWp)),
    el("div", { class: "fields" }, rows));
  card._lamps = () => {
    lampLoad.classList.toggle("on", !d.image);
    lampReady.classList.toggle("on", !!d.image);
    if (lampWp) lampWp.classList.toggle("on", !!d.writeprotect);
  };
  card._lamps();
  return card;
}

async function saveConfig() {
  if (!S.cur) return false;
  try {
    await PUT("configs/" + encodeURIComponent(S.cur.id), { config: S.cur.cfg });
    S.cur.saved = JSON.stringify(S.cur.cfg);
    await loadConfigs();
    renderCfgList(); renderOperate();
    changed();
    toast("SAVED " + S.cur.cfg.name);
    return true;
  } catch (e) { fail(e); return false; }
}

async function newConfig(kind) {
  const name = kind === "raw" ? "Raw command file" : "New configuration";
  try {
    const r = await POST("configs", { name, kind });
    await loadConfigs();
    S.cur = null;
    await openConfig(r.id);
    renderOperate();
    const inp = $("#cfg-editor input");
    if (inp) { inp.focus(); inp.select(); }
  } catch (e) { fail(e); }
}
$("#btn-new").addEventListener("click", () => newConfig("builder"));
$("#btn-new-raw").addEventListener("click", () => newConfig("raw"));

async function duplicateConfig() {
  if (dirty() && !(await saveConfig())) return;
  try {
    const r = await POST("configs", { name: S.cur.cfg.name + " copy", clone: S.cur.id });
    await loadConfigs();
    await openConfig(r.id);
    renderOperate();
    toast("DUPLICATED");
  } catch (e) { fail(e); }
}

async function deleteConfig() {
  if (!(await confirmBox("DELETE", "Delete configuration \"" + S.cur.cfg.name + "\"? This cannot be undone.", "DELETE"))) return;
  try {
    await DEL("configs/" + encodeURIComponent(S.cur.id));
    S.cur = null;
    await loadConfigs();
    renderEditor(); renderCfgList(); renderOperate();
  } catch (e) { fail(e); }
}

// ------------------------------------------------------------------ image picker

async function pickImage(ctrl, cb) {
  const d = $("#dlg-images");
  const list = $("#img-list"), filter = $("#img-filter");
  d.showModal();
  if (!S.images || Date.now() - S.imagesAt > 30000) {
    list.replaceChildren(el("p", { class: "hint", text: "Scanning …" }));
    try { const r = await GET("images"); S.images = r.images; S.imagesAt = Date.now(); $("#img-hint").textContent = "Put your own images into " + r.image_dir + " – .gz images are unpacked by demo on first use."; }
    catch (e) { fail(e); d.close(); return; }
  }
  const pre = ctrl.drive_prefix;
  const hint = { rl: /\.(rl0?[12]|rl)$/i, rk: /\.(rk0?5?|rk|dsk)$|^rt.*rk/i, uda: /\.(dsk|ra\d\d|rd\d\d|r5\d|img|du\d?)$/i, rx: /\.(rx0?1|img|dsk)$/i, ry: /\.(rx0?2|rx2|img|dsk)$/i, rs: /\.(rs|rf|dsk)$/i }[pre];
  const draw = () => {
    const q = filter.value.trim().toLowerCase();
    let imgs = S.images.filter((i) => !q || i.path.toLowerCase().includes(q));
    if (hint) imgs = imgs.slice().sort((a, b) => (hint.test(b.name) ? 1 : 0) - (hint.test(a.name) ? 1 : 0));
    if (!imgs.length) { list.replaceChildren(el("p", { class: "hint", text: "No image files found." })); return; }
    list.replaceChildren(...imgs.slice(0, 400).map((i) => el("button", {
      class: "img-item", onclick: () => { cb(i.path); d.close(); },
    }, el("b", { text: i.name }), el("small", { text: i.dir }),
    el("span", { class: "sz", text: i.compressed_only ? ".gz " + fmtSize(i.gz_size) : fmtSize(i.size) }))));
  };
  filter.oninput = draw;
  filter.value = "";
  draw();
  filter.focus();
}

// ------------------------------------------------------------------ library

function renderLibrary() {
  $("#lib-dir").textContent = S.stockDir;
  const box = $("#lib-list");
  if (!S.stock.length) {
    box.replaceChildren(el("p", {}, "No stock scripts found in ", el("code", { text: S.stockDir }), "."),
      el("p", { class: "hint" }, "On the BeagleBone, run ", el("code", { text: "./update-files.sh" }),
        " in the home directory to download the application scripts and disk images from files.retrocmp.com."));
    return;
  }
  const groups = {};
  S.stock.forEach((s) => { (groups[s.group] = groups[s.group] || []).push(s); });
  box.replaceChildren(...Object.keys(groups).sort().map((g) => el("div", { class: "lib-group" },
    el("h3", { text: g }),
    groups[g].map((s) => el("div", { class: "lib-row" },
      el("i", { class: "lamp white" + (s.ready ? " on" : ""), title: s.ready ? "all files present" : "files missing" }),
      el("div", {}, el("b", { text: s.name }), s.cpu ? el("span", { class: "tag", text: "CPU" }) : null,
        s.mode === "m" ? el("span", { class: "tag", text: "MEMORY TEST" }) : null),
      el("div", { class: "desc" }, s.title || "",
        s.missing.length ? el("span", { class: "miss", text: "missing: " + s.missing.join(", ") }) : null),
      el("div", { class: "acts" },
        el("button", { class: "key small-key key-start", onclick: () => startTarget("stock:" + s.id) }, "START"),
        el("button", { class: "key small-key", onclick: async () => {
          try { const r = await GET("stock/cmd?id=" + encodeURIComponent(s.id)); printout(s.cmdfile, r.cmd); } catch (e) { fail(e); }
        } }, "VIEW"),
        el("button", { class: "key small-key", title: "Copy into an editable raw configuration", onclick: async () => {
          try {
            const r = await POST("stock/copy", { id: s.id });
            await loadConfigs(); renderOperate();
            showView("configure"); S.cur = null; await openConfig(r.id);
            toast("COPIED to " + r.config.name);
          } catch (e) { fail(e); }
        } }, "COPY")))))));
}

// ------------------------------------------------------------------ autostart

let AS = null;
async function loadAutostart() {
  await Promise.all([loadConfigs(), loadStock()]);
  const r = await GET("autostart");
  AS = clone(r.autostart);
  AS.switch_map = AS.switch_map || {};
  renderAutostart(r);
}

function renderAutostart(r) {
  $$("#as-mode .paddle").forEach((b) => {
    b.setAttribute("role", "radio");
    b.setAttribute("aria-checked", b.dataset.mode === AS.mode ? "true" : "false");
    $("i", b).classList.toggle("on", b.dataset.mode === AS.mode);
    b.onclick = () => { AS.mode = b.dataset.mode; renderAutostart(r); };
  });
  $("#as-fixed").hidden = AS.mode !== "fixed";
  $("#as-switch-unit").style.opacity = AS.mode === "switches" ? "1" : ".55";
  $("#as-leds-wrap").hidden = AS.mode !== "switches";
  const sel = $("#as-target");
  fillTargetSelect(sel, AS.target, true);
  sel.onchange = () => { AS.target = sel.value; };
  $("#as-delay").value = AS.delay_s;
  $("#as-delay").oninput = (ev) => { AS.delay_s = Number(ev.target.value || 0); };
  $("#as-leds").checked = AS.leds !== false;
  $("#as-leds").onchange = (ev) => { AS.leds = ev.target.checked; };
  const w = $("#as-would");
  w.replaceChildren("At boot now: ", r.would_start ? el("b", { text: r.would_start_label }) : el("b", { text: "nothing" }),
    el("span", { class: "silk-dim", text: "  (" + r.reason + ", saved settings)" }));

  const map = $("#as-map");
  const rows = [];
  for (let n = 1; n <= 15; n++) {
    const s = el("select", { "aria-label": "Target for switch value " + n, onchange: (ev) => { if (ev.target.value) AS.switch_map[n] = ev.target.value; else delete AS.switch_map[n]; } });
    fillTargetSelect(s, AS.switch_map[n] || "", true);
    rows.push(el("div", { class: "map-row", "data-n": n },
      el("span", { class: "n", text: n }), el("span", { class: "bits", text: n.toString(2).padStart(4, "0") }), s));
  }
  map.replaceChildren(...rows);
  renderDip(r.switches);
}

function renderDip(val) {
  const dip = $("#as-dip");
  if (val === null || val === undefined) {
    dip.replaceChildren(el("span", { class: "val", text: "switches not readable" }));
  } else {
    const kids = [];
    for (let b = 3; b >= 0; b--) {
      kids.push(el("div", { class: "sw1" }, el("div", { class: "slot" + ((val >> b) & 1 ? " on" : "") }), el("small", { text: "SW" + b })));
    }
    kids.push(el("span", { class: "val", text: "= " + val }));
    dip.replaceChildren(...kids);
  }
  $$("#as-map .map-row").forEach((r) => r.classList.toggle("cur", Number(r.dataset.n) === val));
}

$("#as-save").addEventListener("click", async () => {
  if (AS.mode === "fixed" && !AS.target) { toast("Select what to start", true); return; }
  try { await PUT("autostart", AS); toast("AUTOSTART SAVED"); await loadAutostart(); } catch (e) { fail(e); }
});
$("#as-run").addEventListener("click", async () => {
  try {
    await PUT("autostart", AS);
    const r = await POST("autostart/run");
    toast(r.started ? "AUTOSTART " + targetLabel(r.target) : r.reason, !r.started);
    if (r.started) showView("operate");
  } catch (e) { fail(e); }
});

// ------------------------------------------------------------------ system

async function loadSystem() {
  const s = await GET("settings");
  $("#set-platform").value = s.platform || "auto";
  $("#set-aw").value = String(s.address_width || 22);
  $("#set-port").value = s.http_port;
  $("#set-verbose").checked = !!s.verbose;
  $("#set-imgdirs").value = (s.image_dirs || []).join("\n");
  $("#set-pw").value = "";
  $("#set-pw-clear").checked = false;
  $("#set-pw").placeholder = s.password ? "unchanged (password is set)" : "no password set";
  const syncAw = () => { $("#set-aw-wrap").hidden = ($("#set-platform").value === "UNIBUS") || ($("#set-platform").value === "auto" && s.detected === "UNIBUS"); };
  $("#set-platform").onchange = syncAw;
  syncAw();
  const st = S.status || await GET("status");
  const facts = [
    ["Platform", st.platform + (s.platform === "auto" ? " (detected)" : " (forced)")],
    ["QUniBone home", st.home],
    ["demo program", st.demo_bin, st.demo_ok ? "good" : "bad"],
    ["Running as root", st.is_root ? "yes" : "no", st.is_root ? "good" : (st.fake_demo ? "" : "bad")],
    ["Panel data", st.data_dir],
    ["Web address", (st.ips || []).map((ip) => "http://" + ip + ":" + st.port + "/").join("  ")],
    ["DIP switches", st.switches === null ? "not readable" : String(st.switches)],
    ["Terminal menu", "qbmenu  (ssh or serial console)"],
  ];
  $("#sys-facts").replaceChildren(...facts.flatMap(([k, v, cls]) => [el("dt", { text: k }), el("dd", { class: cls || "", text: v })]));
  loadLog();
}
async function loadLog() {
  try { const r = await GET("log"); $("#sys-log").textContent = r.log || "(empty)"; const p = $("#sys-log").parentElement; p.scrollTop = p.scrollHeight; } catch (e) { fail(e); }
}
$("#sys-log-refresh").addEventListener("click", loadLog);
$("#set-save").addEventListener("click", async () => {
  const body = {
    platform: $("#set-platform").value,
    address_width: Number($("#set-aw").value),
    http_port: Number($("#set-port").value),
    verbose: $("#set-verbose").checked,
    image_dirs: $("#set-imgdirs").value.split("\n").map((x) => x.trim()).filter(Boolean),
  };
  if ($("#set-pw-clear").checked) body.password = "";
  else if ($("#set-pw").value) body.password = $("#set-pw").value;
  try {
    await PUT("settings", body);
    toast("SETTINGS SAVED");
    S.catalog = await GET("catalog");
    S.status = await GET("status"); renderStatus();
    loadSystem();
  } catch (e) { fail(e); }
});

// ------------------------------------------------------------------ boot

(async function init() {
  setPhosphor(store.get("phosphor", "amber"));
  try {
    [S.status, S.catalog] = await Promise.all([GET("status"), GET("catalog")]);
    await Promise.all([loadConfigs(), loadStock()]);
  } catch (e) { fail(e); }
  if (S.status) renderStatus();
  renderOperate();
  // deep links: #operate, #library, #configure/<config id>, ...
  const [v, arg] = ((location.hash || "").slice(1) || store.get("view", "operate")).split("/");
  showView(["operate", "configure", "library", "autostart", "system"].includes(v) ? v : "operate");
  const cur = (v === "configure" && arg) ? decodeURIComponent(arg) : store.get("curConfig", "");
  if (cur && S.configs.some((c) => c.id === cur) && S.catalog) openConfig(cur);
  setTimeout(pollStatus, 2000);
  consoleLoop();
})();

window.addEventListener("beforeunload", (ev) => { if (dirty()) { ev.preventDefault(); ev.returnValue = ""; } });
