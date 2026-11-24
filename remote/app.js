/* Dropkick remote. Plain DOM, no build step, no network dependencies. */
"use strict";

const $ = (id) => document.getElementById(id);
const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];

/* ---------- auth ----------
 * Token sources, in order: ?token=..., the URL path (http://host:8080/op1, easiest to
 * type on a phone), then a token entered in this tab. It is sent as a header so it
 * never lands in server logs or Referer headers. */
const TOKEN_KEY = "dk-token";
function tokenFromUrl() {
  const q = new URLSearchParams(location.search).get("token");
  if (q) return q;
  const p = decodeURIComponent(location.pathname.replace(/^\/+|\/+$/g, ""));
  return p && p !== "index.html" ? p : "";
}
const store = (kind, key, val) => {
  try {
    const s = kind === "session" ? sessionStorage : localStorage;
    if (val === undefined) return s.getItem(key);
    s.setItem(key, val);
  } catch (e) { /* storage blocked: work without persistence */ }
  return null;
};
let token = tokenFromUrl() || store("session", TOKEN_KEY) || "";
if (new URLSearchParams(location.search).has("token")) {
  store("session", TOKEN_KEY, token);
  history.replaceState(null, "", location.pathname); // keep the token out of history
}

class AuthError extends Error {}

async function api(url, { method = "GET", body, headers = {} } = {}) {
  const h = { ...headers };
  if (token) h["X-Dropkick-Token"] = token;
  const r = await fetch(url, { method, body, headers: h, cache: "no-store" });
  if (r.status === 401) { setConn("auth"); openAuth(); throw new AuthError("unauthorized"); }
  return r;
}
async function getJSON(url) {
  const r = await api(url);
  if (!r.ok) throw new Error(`${url}: ${r.status}`);
  return r.json();
}
async function post(url, body) {
  const r = await api(url, { method: "POST", body });
  const data = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(data.error || `HTTP ${r.status}`);
  return data;
}
const q = encodeURIComponent;

/* ---------- small UI helpers ---------- */
function h(tag, attrs = {}, ...kids) {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (v === false || v == null) continue;
    if (k === "class") el.className = v;
    else if (k === "text") el.textContent = v;
    else el.setAttribute(k, v === true ? "" : v);
  }
  for (const kid of kids) if (kid != null) el.append(kid);
  return el;
}
const SVG_NS = "http://www.w3.org/2000/svg";
function icon(name) {
  const svg = document.createElementNS(SVG_NS, "svg");
  svg.setAttribute("class", "ic");
  svg.setAttribute("aria-hidden", "true");
  const use = document.createElementNS(SVG_NS, "use");
  use.setAttribute("href", "#i-" + name);
  svg.append(use);
  return svg;
}

function toast(msg, kind = "ok") {
  const t = h("div", { class: "toast " + kind, role: kind === "error" ? "alert" : "status" },
    icon(kind === "error" ? "alert" : "check"), h("span", { text: msg }));
  const host = $("toasts");
  host.append(t);
  while (host.children.length > 3) host.firstElementChild.remove();
  setTimeout(() => { t.classList.add("out"); setTimeout(() => t.remove(), 220); }, kind === "error" ? 4200 : 2400);
}
const fail = (e) => { if (!(e instanceof AuthError)) toast(e.message || "Request failed", "error"); };

const setSwitch = (el, on) => el && el.setAttribute("aria-checked", on ? "true" : "false");
const isOn = (el) => el.getAttribute("aria-checked") === "true";
const pad4 = (n) => String(n).padStart(4, "0");

/* ---------- connection ---------- */
function setConn(state) {
  $("conn").dataset.state = state;
  const label = { ok: "Connected", retry: "Reconnecting", auth: "Access token required" }[state];
  $("connText").textContent = label;
  $("statusPill").title = label;
  if (state !== "ok") $("pillText").textContent = state === "auth" ? "Locked" : "Offline";
}

/* Status pill: one quiet readout in the bar, details in a popover. */
function setTelePop(open) {
  $("telePop").hidden = !open;
  $("statusPill").setAttribute("aria-expanded", String(open));
}
$("statusPill").onclick = (e) => { e.stopPropagation(); setTelePop($("telePop").hidden); };
document.addEventListener("click", (e) => { if (!e.target.closest("#telePop")) setTelePop(false); });

/* ---------- theme ---------- */
let previewReady = false;
function applyTheme(t) {
  if (t === "light") document.documentElement.dataset.theme = "light";
  else delete document.documentElement.dataset.theme;
  $("theme").setAttribute("aria-label", t === "light" ? "Switch to dark theme" : "Switch to light theme");
  document.querySelector('meta[name="theme-color"]').content = t === "light" ? "#eef0f4" : "#070709";
  if (previewReady) { renderBgDither(performance.now()); renderLastFrame(); }
}
applyTheme(store("local", "dk-theme") === "light" ? "light" : "dark");
$("theme").onclick = () => {
  const next = document.documentElement.dataset.theme === "light" ? "dark" : "light";
  store("local", "dk-theme", next);
  applyTheme(next);
};

/* Sticky toolbars sit just under the (variable-height) top bar. */
new ResizeObserver(([e]) => document.documentElement.style.setProperty("--sticky-top", Math.ceil(e.borderBoxSize?.[0]?.blockSize ?? e.target.offsetHeight) + "px"))
  .observe(document.querySelector(".topbar"));

/* ---------- tabs ----------
 * Desktop (>= 960px) keeps Now Playing docked on the left at all times, so "now" there
 * means the workspace's default, Browse. */
const desktop = matchMedia("(min-width: 960px)");
let view = "now";
function showView(name) {
  if (name === "now" && desktop.matches) name = "browse";
  view = name;
  document.body.dataset.view = name;
  $$(".tab").forEach((t) => {
    if (t.dataset.tab === name) t.setAttribute("aria-current", "page");
    else t.removeAttribute("aria-current");
  });
  $$(".view").forEach((v) => { v.hidden = v.dataset.view !== name; });
  window.scrollTo(0, 0);
  if (name === "fav") renderFavs();
  if (name === "set" || name === "now") loadSettings();
  if (previewReady) kickPreview();
}
$$(".tab").forEach((t) => (t.onclick = () => showView(t.dataset.tab)));
$$("[data-goto]").forEach((b) => (b.onclick = () => showView(b.dataset.goto)));
if (desktop.matches) showView("browse"); else document.body.dataset.view = view;
desktop.addEventListener("change", () => { if (view === "now") showView("browse"); if (previewReady) kickPreview(); });

/* ---------- preset library ---------- */
let presets = [];            // [{i, p}]
let byPath = new Map();
let prefix = "";             // common directory prefix (ends with /)
let groups = new Map();      // category -> [preset]
let favorites = new Set();
let currentPath = "";
let lastCatalog;             // catalogRevision (or size on older servers) from /api/status
let presetsLoaded = false;
let catShuffle = "";         // server's active category-shuffle prefix ("" = off)
let presetsEtag = "";

const nameOf = (p) => p.split("/").pop().replace(/\.milk$/i, "");
const relOf = (p) => (p.startsWith(prefix) ? p.slice(prefix.length) : p);
const catOf = (p) => { const parts = relOf(p).split("/"); return parts.length > 1 ? parts[0] : "Presets"; };
const subOf = (p) => { const parts = relOf(p).split("/"); return parts.length > 2 ? parts.slice(1, -1).join(" / ") : ""; };

function commonPrefix(paths) {
  if (!paths.length) return "";
  let pre = paths[0];
  for (const p of paths) {
    while (!p.startsWith(pre)) {
      pre = pre.slice(0, pre.lastIndexOf("/", pre.length - 2) + 1);
      if (!pre) return "";
    }
  }
  return pre.slice(0, pre.lastIndexOf("/") + 1);
}

/* The pack directory, so a pack holding a single folder keeps that folder as its category
 * instead of collapsing it into the prefix. */
function packPrefix(paths) {
  const pre = commonPrefix(paths);
  const segs = pre.split("/");
  const j = segs.findIndex((seg, k) => seg && (packs.includes(seg) || segs[k - 1] === "presets"));
  return j >= 0 ? segs.slice(0, j + 1).join("/") + "/" : pre;
}

let presetsLoading = null, presetsAgain = false;
function loadPresets() {
  // One request at a time; a call made mid-flight reruns afterwards so a newer catalog is not lost.
  if (presetsLoading) { presetsAgain = true; return presetsLoading; }
  presetsLoading = fetchPresets().finally(() => {
    presetsLoading = null;
    if (presetsAgain) { presetsAgain = false; loadPresets(); }
  });
  return presetsLoading;
}

async function fetchPresets() {
  try {
    const r = await api("/api/presets", { headers: presetsEtag ? { "If-None-Match": presetsEtag } : {} });
    if (r.status === 304) { presetsLoaded = true; return; }
    if (!r.ok) throw new Error("presets " + r.status);
    presetsEtag = r.headers.get("ETag") || "";
    presets = await r.json();
    presetsLoaded = true;
    byPath = new Map(presets.map((x) => [x.p, x]));
    prefix = packPrefix(presets.map((x) => x.p));
    groups = new Map();
    for (const x of presets) {
      const c = catOf(x.p);
      if (!groups.has(c)) groups.set(c, []);
      groups.get(c).push(x);
    }
    shown.clear();
    lists.clear();
    renderBrowse();
    renderFavs();
    syncPackSelect();
  } catch (e) { /* refresh() retries until presetsLoaded */ }
}

async function loadFavorites() {
  try { favorites = new Set(await getJSON("/api/favorites")); } catch (e) {}
}

async function toggleFavorite(path) {
  try {
    const r = await post("/api/favorites/toggle?path=" + q(path));
    if (r.favorited) favorites.add(path); else favorites.delete(path);
    $$(`.star[data-i="${byPath.get(path)?.i}"]`).forEach((s) => s.setAttribute("aria-pressed", String(!!r.favorited)));
    if (path === currentPath) setSwitch($("tFav"), r.favorited);
    toast(r.favorited ? "Added to favorites" : "Removed from favorites");
    if (view === "fav") renderFavs();
    return r.favorited;
  } catch (e) { fail(e); }
}

/* ---------- rows (shared by Browse + Favorites) ---------- */
const PAGE = 100;
const shown = new Map();     // list key -> rows rendered
const lists = new Map();     // list key -> [items, showSub], for appending on "Show more"
const moreLabel = (left) => `Show more (${left.toLocaleString()} remaining)`;

function rowEl(x, showSub = true) {
  const full = nameOf(x.p);
  const { author, title } = splitName(full);
  const sub = showSub ? subOf(x.p) : relOf(x.p).split("/").slice(0, -1).join(" / ");
  return h("div", { class: "prow" + (x.p === currentPath ? " is-current" : ""), "data-i": x.i },
    h("button", { type: "button", class: "prow-play", "data-act": "play", "data-i": x.i, title: full },
      h("span", { class: "prow-ix num" }, h("span", { class: "ix-num", text: String(x.i + 1) }), icon("play")),
      h("span", { class: "prow-text" },
        h("span", { class: "prow-name", text: title }),
        sub ? h("span", { class: "prow-sub", text: sub }) : null),
      author ? h("span", { class: "prow-author", text: author }) : null),
    h("button", { type: "button", class: "star", "data-act": "fav", "data-i": x.i,
      "aria-pressed": String(favorites.has(x.p)), "aria-label": "Favorite " + full }, icon("star")));
}

/* Append a chunk of rows plus a "Show more" button; never renders thousands at once. */
function appendRows(host, key, items, showSub) {
  lists.set(key, [items, showSub]);
  const n = Math.min(items.length, shown.get(key) || PAGE);
  const frag = document.createDocumentFragment();
  for (let k = 0; k < n; k++) frag.append(rowEl(items[k], showSub));
  if (items.length > n) {
    frag.append(h("button", { type: "button", class: "more", "data-act": "more", "data-key": key, text: moreLabel(items.length - n) }));
  }
  host.append(frag);
}

/* Insert the next chunk before the button so scroll position and keyboard focus survive. */
function showMore(b) {
  const key = b.dataset.key;
  const [items, showSub] = lists.get(key) || [[]];
  const from = shown.get(key) || PAGE;
  const to = Math.min(items.length, from + PAGE * 2);
  shown.set(key, to);
  const rows = items.slice(from, to).map((x) => rowEl(x, showSub));
  b.before(...rows);
  if (to < items.length) { b.textContent = moreLabel(items.length - to); return; }
  if (document.activeElement === b) rows[0]?.querySelector("button")?.focus();
  b.remove();
}

const openCats = new Set();
function renderBrowse() {
  const host = $("browseList");
  const term = $("search").value.trim().toLowerCase();
  $("searchClear").hidden = !term;
  host.replaceChildren();
  if (!presets.length) { host.append(h("div", { class: "hint", text: "Loading preset list..." })); $("resultCount").textContent = ""; return; }

  if (term) {
    const hits = presets.filter((x) => relOf(x.p).toLowerCase().includes(term));
    $("resultCount").textContent = `${hits.length.toLocaleString()} result${hits.length === 1 ? "" : "s"}`;
    if (!hits.length) { host.append(h("div", { class: "hint", text: "No presets match." })); return; }
    const box = h("div", { class: "flat" });
    appendRows(box, "search:" + term, hits, false);
    host.append(box);
    return;
  }

  $("resultCount").textContent = `${presets.length.toLocaleString()} presets`;
  for (const [cat, items] of groups) {
    const dir = prefix + cat;
    const open = openCats.has(cat);
    const canShuffle = cat !== "Presets"; // loose top-level files have no folder to scope to
    const active = catShuffle === dir + "/";
    const box = h("div", { class: "cat" },
      h("div", { class: "cat-head" },
        h("button", { type: "button", class: "cat-toggle", "data-act": "cat", "data-cat": cat, "aria-expanded": String(open) },
          icon("chevron"), h("span", { class: "cat-name", text: cat }),
          h("span", { class: "cat-count num", text: items.length.toLocaleString() })),
        canShuffle ? h("button", { type: "button", class: "cat-shuffle", "data-act": "catshuffle", "data-cat": cat,
          "aria-pressed": String(active), title: "Shuffle only within " + cat },
          icon("shuffle"), h("span", { text: active ? "Shuffling" : "Shuffle" })) : null));
    if (open) {
      const body = h("div", { class: "cat-body" });
      appendRows(body, "cat:" + cat, items, true);
      box.append(body);
    }
    host.append(box);
  }
}

function renderFavs() {
  const term = $("favSearch").value.trim().toLowerCase();
  const all = [...favorites].map((p) => byPath.get(p)).filter(Boolean).sort((a, b) => a.i - b.i);
  const items = term ? all.filter((x) => relOf(x.p).toLowerCase().includes(term)) : all;
  $("favCount").textContent = term ? `${items.length} of ${all.length}` : `${all.length} starred`;
  const host = $("favList");
  host.replaceChildren();
  $("favEmpty").hidden = all.length > 0;
  if (!all.length) return;
  if (!items.length) { host.append(h("div", { class: "hint", text: "No favorites match." })); return; }
  const box = h("div", { class: "flat" });
  appendRows(box, "fav:" + term, items, false);
  host.append(box);
}

async function playIndex(i) {
  try { await post("/api/preset?index=" + i); setTimeout(refresh, 250); } catch (e) { fail(e); }
}

function syncCatShuffleButtons() {
  $$(".cat-shuffle").forEach((b) => {
    const on = catShuffle === prefix + b.dataset.cat + "/";
    if (b.getAttribute("aria-pressed") !== String(on)) {
      b.setAttribute("aria-pressed", String(on));
      b.lastChild.textContent = on ? "Shuffling" : "Shuffle";
    }
  });
}

async function toggleCategoryShuffle(cat) {
  const dir = prefix + cat;
  try {
    const r = await post("/api/category/shuffle?path=" + q(dir));
    catShuffle = r.categoryShuffle ? dir + "/" : "";
    syncCatShuffleButtons();
    toast(r.categoryShuffle ? `Shuffling within ${cat}` : "Category shuffle off");
    setTimeout(refresh, 300);
  } catch (e) { fail(e); }
}

/* Toggle a category in place so keyboard focus remains on its .cat-toggle button. */
function toggleCategory(btn) {
  const cat = btn.dataset.cat;
  const open = !openCats.has(cat);
  if (open) openCats.add(cat); else openCats.delete(cat);
  btn.setAttribute("aria-expanded", String(open));
  const box = btn.closest(".cat");
  if (!box) return;
  const existing = box.querySelector(".cat-body");
  if (open) {
    if (!existing) {
      const body = h("div", { class: "cat-body" });
      appendRows(body, "cat:" + cat, groups.get(cat) || [], true);
      box.append(body);
    }
  } else {
    existing?.remove();
  }
}

function onListClick(e) {
  const b = e.target.closest("[data-act]");
  if (!b) return;
  const i = Number(b.dataset.i);
  switch (b.dataset.act) {
    case "play": playIndex(i); break;
    case "fav": if (presets[i]) toggleFavorite(presets[i].p); break;
    case "cat": toggleCategory(b); break;
    case "catshuffle": toggleCategoryShuffle(b.dataset.cat); break;
    case "more": showMore(b); break;
  }
}
$("browseList").addEventListener("click", onListClick);
$("favList").addEventListener("click", onListClick);

let searchTimer;
$("search").addEventListener("input", () => { clearTimeout(searchTimer); searchTimer = setTimeout(renderBrowse, 120); });
$("searchClear").onclick = () => { $("search").value = ""; renderBrowse(); $("search").focus(); };
$("favSearch").addEventListener("input", () => { clearTimeout(searchTimer); searchTimer = setTimeout(renderFavs, 120); });

function jumpToCurrent() {
  const x = byPath.get(currentPath);
  if (!x) { toast("Current preset is not in this pack", "error"); return; }
  $("search").value = "";
  const cat = catOf(x.p);
  openCats.add(cat);
  const pos = groups.get(cat).indexOf(x);
  if (pos >= PAGE) shown.set("cat:" + cat, pos + PAGE);
  renderBrowse();
  $("browseList").querySelector(`.prow[data-i="${x.i}"]`)?.scrollIntoView({ block: "center", behavior: "smooth" });
}
$("jumpCurrent").onclick = jumpToCurrent;

function markCurrent() {
  const i = byPath.get(currentPath)?.i;
  $$(".prow.is-current").forEach((r) => r.classList.remove("is-current"));
  if (i != null) $$(`.prow[data-i="${i}"]`).forEach((r) => r.classList.add("is-current"));
}

/* "Author - Title" is the Milkdrop naming convention; anything else is all title. */
function splitName(name) {
  const k = name.indexOf(" - ");
  return k > 0 ? { author: name.slice(0, k).trim(), title: name.slice(k + 3).trim() || name } : { author: "", title: name };
}

/* ---------- ordered fine pixel-dithered background shader ("Titanium Lumen") ----------
 * Fine ~2.25 CSS px Ordered Bayer 8x8 refracted acoustic caustic net inside a brushed-titanium
 * waveguide vault, keeping a calm reading sanctuary down the center column.
 * Runs in WebGL 1.0 at ~20 fps, pausing automatically when hidden or under reduced motion. */
const bgCanvas = $("bgDither");
const reducedMotion = matchMedia("(prefers-reduced-motion: reduce)");
let bgRaf = 0, bgLast = 0;

const BG_VERT = `attribute vec2 aPos; void main() { gl_Position = vec4(aPos, 0.0, 1.0); }`;
const BG_FRAG = `
// Titanium Lumen: refracted acoustic caustic net inside a brushed-titanium waveguide vault
// Palette: obsidian / graphite / titanium / platinum with sapphire depth and ice filaments (light: pearl / silver / graphite)
precision highp float;

uniform vec2 uRes;
uniform float uTime;
uniform float uLight;
uniform float uDpr;

float bayer2(vec2 p2) {
  vec2 m2 = mod(floor(p2), 2.0);
  return 2.0 * abs(m2.x - m2.y) + m2.y;
}

float bayer8(vec2 p8) {
  vec2 q8 = mod(floor(p8), 8.0);
  float b0 = bayer2(q8);
  float b1 = bayer2(floor(q8 * 0.5));
  float b2 = bayer2(floor(q8 * 0.25));
  return (16.0 * b0 + 4.0 * b1 + b2 + 0.5) / 64.0;
}

vec2 fluidWarp(vec2 pw, float tw) {
  vec2 w = vec2(0.0);
  w.x += sin(pw.y * 2.3 + tw * 0.88) * 0.54 + cos((pw.x * 1.7 - pw.y * 1.4) + tw * 0.63) * 0.46;
  w.y += cos(pw.x * 2.1 - tw * 0.76) * 0.54 + sin((pw.x * 1.5 + pw.y * 1.8) - tw * 0.57) * 0.46;
  vec2 rw = mat2(0.7660, -0.6428, 0.6428, 0.7660) * pw * 1.92;
  w.x += sin(rw.y * 2.4 - tw * 1.12 + 1.7) * 0.28;
  w.y += cos(rw.x * 2.2 + tw * 1.02 + 0.9) * 0.28;
  return w;
}

vec3 ramp5(vec3 c0, vec3 c1, vec3 c2, vec3 c3, vec3 c4, float tr) {
  float sr = clamp(tr, 0.0, 1.0) * 4.0;
  vec3 acc = mix(c0, c1, clamp(sr, 0.0, 1.0));
  acc = mix(acc, c2, clamp(sr - 1.0, 0.0, 1.0));
  acc = mix(acc, c3, clamp(sr - 2.0, 0.0, 1.0));
  return mix(acc, c4, clamp(sr - 3.0, 0.0, 1.0));
}

float quantizeDither(float val, float steps, float thresh) {
  float scaled = clamp(val, 0.0, 0.9999) * steps;
  float idx = floor(scaled);
  return clamp((idx + step(thresh, scaled - idx)) / steps, 0.0, 1.0);
}

// Three-source standing wave. x: thin caustic filaments at nodal lines, y: soft antinode body.
vec2 causticField(vec2 pc, vec2 sa, vec2 sb, vec2 sc, float tc) {
  float da = length(pc - sa);
  float db = length(pc - sb);
  float dc = length(pc - sc);
  float sumW = sin(da * 11.2 - tc * 1.40) * 0.40
             + sin(db * 10.3 - tc * 1.22 + 1.30) * 0.40
             + cos(dc * 8.6 - tc * 0.98 + 0.60) * 0.30;
  float filament = exp(-sumW * sumW * 30.0);
  float bodyW = clamp(0.5 + 0.5 * sumW, 0.0, 1.0);
  return vec2(filament, bodyW * bodyW * sqrt(bodyW));
}

void main() {
  float dpr = uDpr > 0.01 ? clamp(uDpr, 0.75, 4.0) : 1.0;
  float pxSize = max(2.0, floor(2.25 * dpr + 0.5));
  vec2 cell = floor(gl_FragCoord.xy / pxSize);
  vec2 samplePos = (cell + 0.5) * pxSize;

  vec2 safeRes = max(uRes, vec2(1.0));
  vec2 uv = samplePos / safeRes;
  float sx = uv.x - 0.5;
  float sy = uv.y - 0.5;

  float rawAspect = safeRes.x / safeRes.y;
  float aspect = clamp(rawAspect, 0.68, 1.75);
  float portrait = smoothstep(1.0, 0.62, rawAspect);
  vec2 q = vec2(sx * aspect, sy);
  float xSpan = mix(sx * 1.55, q.x, 0.5);

  float t = uTime * 0.10;
  vec2 warp = fluidWarp(q, t);
  vec2 qw = q + 0.07 * warp;

  // Resonators sit on the flanks; on portrait they stagger vertically so the narrow sides still get structure.
  float latScale = mix(1.0, aspect, 0.55);
  vec2 resL = vec2(-0.80 * latScale + 0.05 * sin(t * 0.73), mix(-0.04, 0.20, portrait) + 0.08 * cos(t * 0.59));
  vec2 resR = vec2( 0.80 * latScale + 0.05 * cos(t * 0.69), mix(0.04, -0.20, portrait) + 0.08 * sin(t * 0.63));
  vec2 resT = vec2( 0.10 * sin(t * 0.47), 0.64 + 0.05 * cos(t * 0.53));

  // Front caustic plane plus a slower, magnified depth plane seen "through" it (parallax refraction).
  vec2 cFront = causticField(qw, resL, resR, resT, t);
  vec2 qDeep = q * 0.84 + 0.12 * warp.yx + vec2(0.0, 0.03 * sin(t * 0.41));
  vec2 cDeep = causticField(qDeep, resL * 1.08, resR * 1.08, resT * 0.94, t * 0.70 + 2.1);

  float edgeFrame = smoothstep(0.16, 0.54, length(vec2(sx * mix(1.35, 1.10, portrait), sy * mix(1.05, 1.25, portrait))));

  // Lower waveguide: thin primary rim with a refracted inner echo; bed is dimmed under the control dock.
  float bowlCurve = -0.40 + 0.44 * xSpan * xSpan + 0.032 * warp.y + 0.020 * sin(q.x * 3.8 + t * 1.15);
  float dBowl = sy - bowlCurve;
  float bowlRim = exp(-dBowl * dBowl * 140.0);
  float bowlEcho = exp(-(dBowl + 0.048) * (dBowl + 0.048) * 220.0) * 0.50;
  float bowlBed = smoothstep(0.03, -0.18, dBowl) * (0.20 + 0.10 * cos(qw.x * 4.4 - t * 0.82));
  float dockCalm = 1.0 - 0.60 * exp(-sx * sx * 11.0) * smoothstep(-0.08, -0.38, sy);
  float hearth = (bowlRim * 0.82 + bowlEcho + bowlBed) * dockCalm;

  // Upper vault: matching thin arch and echo with a restrained canopy on the shoulders.
  float vaultArch = 0.43 - 0.34 * xSpan * xSpan + 0.030 * warp.y - 0.020 * cos(q.x * 3.4 - t * 0.95);
  float dVault = sy - vaultArch;
  float vaultRim = exp(-dVault * dVault * 120.0) * 0.72;
  float vaultEcho = exp(-(dVault - 0.042) * (dVault - 0.042) * 200.0) * 0.36;
  float vaultCanopy = smoothstep(-0.04, 0.20, dVault) * 0.34;
  float shoulderMask = 0.42 + 0.58 * smoothstep(0.08, 0.42, abs(sx));
  float vault = (vaultRim + vaultEcho + vaultCanopy) * shoulderMask;

  float causticEnergy = cFront.x * 0.40 * edgeFrame
                      + cFront.y * 0.30 * (0.12 + 0.68 * edgeFrame)
                      + (cDeep.y * 0.18 + cDeep.x * 0.12) * edgeFrame;
  float combined = max(hearth * 0.86, vault * 0.74) + 0.36 * min(hearth, vault) + causticEnergy * (0.75 + 0.25 * hearth);
  float shaped = 1.0 - exp(-combined * 1.32);

  vec2 wellPos = vec2(sx / mix(0.30, 0.34, portrait), (sy - 0.02) / mix(0.34, 0.38, portrait));
  float sanctuary = 1.0 - 0.72 * exp(-dot(wellPos, wellPos) * 1.5);
  float cornerContain = 1.0 - 0.18 * smoothstep(0.58, 0.95, length(vec2(sx, sy) * 1.4));
  float level = clamp(shaped * sanctuary * cornerContain, 0.0, 1.0);

  // Tint weights: sapphire pools in the depth plane and bowl bed; ice rides the filaments and rims.
  float phaseTint = 0.5 + 0.5 * sin(q.x * 2.4 - q.y * 1.6 + t * 0.5 + warp.x * 1.2);
  float sapphWeight = clamp(
    cDeep.y * 0.80 * edgeFrame + smoothstep(0.10, -0.40, sy) * 0.30 + bowlBed * 0.60 + vaultCanopy * 0.50 + 0.25 * phaseTint - bowlRim * 0.30,
    0.0,
    1.0
  );
  float iceWeight = clamp(cFront.x * edgeFrame * 1.40 + bowlRim * 0.55 + vaultRim * 0.60 + bowlEcho * 0.40, 0.0, 1.0);

  float bMain = bayer8(cell);
  float bSapph = bayer8(cell.yx + vec2(5.0, 3.0));
  float bIce = bayer8(vec2(7.0 - mod(cell.x, 8.0), cell.y) + vec2(2.0, 6.0));

  float levelScale = uLight > 0.5 ? 0.90 : 1.0;
  float qLevel = quantizeDither(level * levelScale, 4.0, bMain);
  float qSapph = quantizeDither(sapphWeight, 3.0, bSapph);
  float qIce = quantizeDither(iceWeight, 3.0, bIce);

  vec3 col;
  if (uLight > 0.5) {
    vec3 basePearl = vec3(0.952, 0.956, 0.962);
    vec3 titanLight = ramp5(basePearl,
      vec3(0.918, 0.922, 0.930), vec3(0.846, 0.852, 0.866), vec3(0.722, 0.732, 0.754), vec3(0.548, 0.560, 0.588), qLevel);
    vec3 sapphLight = ramp5(basePearl,
      vec3(0.910, 0.922, 0.948), vec3(0.830, 0.852, 0.908), vec3(0.694, 0.736, 0.836), vec3(0.508, 0.572, 0.722), qLevel);
    vec3 iceLight = ramp5(basePearl,
      vec3(0.920, 0.938, 0.946), vec3(0.842, 0.880, 0.896), vec3(0.704, 0.776, 0.806), vec3(0.528, 0.628, 0.672), qLevel);
    col = mix(titanLight, sapphLight, qSapph * 0.85);
    col = mix(col, iceLight, qIce * 0.80);
  } else {
    vec3 baseObsidian = vec3(0.027, 0.031, 0.039);
    vec3 titanDark = ramp5(baseObsidian,
      vec3(0.062, 0.068, 0.080), vec3(0.138, 0.148, 0.168), vec3(0.305, 0.320, 0.350), vec3(0.650, 0.668, 0.698), qLevel);
    vec3 sapphDark = ramp5(baseObsidian,
      vec3(0.050, 0.064, 0.102), vec3(0.092, 0.132, 0.218), vec3(0.184, 0.272, 0.444), vec3(0.430, 0.548, 0.770), qLevel);
    vec3 iceDark = ramp5(baseObsidian,
      vec3(0.058, 0.074, 0.090), vec3(0.122, 0.172, 0.202), vec3(0.302, 0.420, 0.472), vec3(0.770, 0.866, 0.906), qLevel);
    col = mix(titanDark, sapphDark, qSapph * 0.85);
    col = mix(col, iceDark, qIce * 0.80);
  }

  gl_FragColor = vec4(col, 1.0);
}
`;

let bgGl = null, bgProg = null, bgU = null;
function initBgGl() {
  const gl = bgCanvas.getContext("webgl", { alpha: false, antialias: false, depth: false, preserveDrawingBuffer: false });
  if (!gl) return false;
  const compile = (type, src) => {
    const s = gl.createShader(type);
    gl.shaderSource(s, src);
    gl.compileShader(s);
    if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) { gl.deleteShader(s); return null; }
    return s;
  };
  const vs = compile(gl.VERTEX_SHADER, BG_VERT);
  const fs = compile(gl.FRAGMENT_SHADER, BG_FRAG);
  if (!vs || !fs) return false;
  const p = gl.createProgram();
  gl.attachShader(p, vs); gl.attachShader(p, fs); gl.linkProgram(p);
  if (!gl.getProgramParameter(p, gl.LINK_STATUS)) return false;
  const buf = gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER, buf);
  gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 3, -1, -1, 3]), gl.STATIC_DRAW);
  const loc = gl.getAttribLocation(p, "aPos");
  gl.enableVertexAttribArray(loc);
  gl.vertexAttribPointer(loc, 2, gl.FLOAT, false, 0, 0);
  bgGl = gl; bgProg = p;
  bgU = {
    res: gl.getUniformLocation(p, "uRes"),
    time: gl.getUniformLocation(p, "uTime"),
    light: gl.getUniformLocation(p, "uLight"),
    dpr: gl.getUniformLocation(p, "uDpr"),
  };
  return true;
}
initBgGl();

function sizeBgDither() {
  const dpr = Math.min(window.devicePixelRatio || 1, 2);
  const w = Math.max(1, Math.round(innerWidth * dpr));
  const h = Math.max(1, Math.round(innerHeight * dpr));
  if (bgCanvas.width !== w || bgCanvas.height !== h) {
    bgCanvas.width = w;
    bgCanvas.height = h;
    if (bgGl) bgGl.viewport(0, 0, w, h);
  }
}

function renderBgDither(nowMs) {
  sizeBgDither();
  const light = document.documentElement.dataset.theme === "light";
  if (bgGl && bgProg) {
    const dpr = Math.min(window.devicePixelRatio || 1, 2);
    bgGl.useProgram(bgProg);
    bgGl.uniform2f(bgU.res, bgCanvas.width, bgCanvas.height);
    bgGl.uniform1f(bgU.time, (nowMs || 0) * 0.001);
    bgGl.uniform1f(bgU.light, light ? 1.0 : 0.0);
    bgGl.uniform1f(bgU.dpr, dpr);
    bgGl.drawArrays(bgGl.TRIANGLES, 0, 3);
    return;
  }
  bgCanvas.style.display = "none";
}

function bgTick(now) {
  bgRaf = 0;
  if (document.hidden || reducedMotion.matches) return;
  if (now - bgLast >= 48) { // smooth ~20 fps ambient motion, ultra-low GPU/battery draw
    bgLast = now;
    renderBgDither(now);
  }
  bgRaf = requestAnimationFrame(bgTick);
}

function startBgDither() {
  cancelAnimationFrame(bgRaf);
  renderBgDither(performance.now());
  if (!reducedMotion.matches && !document.hidden) bgRaf = requestAnimationFrame(bgTick);
}
addEventListener("resize", () => renderBgDither(performance.now()));
document.addEventListener("visibilitychange", () => { if (!document.hidden) startBgDither(); });

/* ---------- live visualizer preview ----------
 * Polls GET /api/preview (240x150 full-color BMP from the live OpenGL framebuffer) while Now
 * Playing is visible and settings.livePreview is enabled. Pauses automatically when hidden or off. */
const PREVIEW_W = 240, PREVIEW_H = 150;
const plate = $("stageCanvas");
const plateCtx = plate.getContext("2d");
const srcCanvas = document.createElement("canvas");
srcCanvas.width = PREVIEW_W; srcCanvas.height = PREVIEW_H;
const srcCtx = srcCanvas.getContext("2d");

let hasLiveFrame = false;
let previewDisabledByServer = false;
let previewBusy = false;
let previewTimer = 0;

/* Clean subtle standby plate when live preview is turned off or warming up. */
function drawStandby(msg, showEnable) {
  plateCtx.fillStyle = "#0a0908";
  plateCtx.fillRect(0, 0, plate.width, plate.height);
  $("plateOffMsg").textContent = msg;
  $("btnEnablePreview").hidden = !showEnable;
  $("plateOff").hidden = false;
  $("nowLabel").classList.toggle("idle", true);
  if (!lastStatus.workshop) $("nowLabelText").textContent = showEnable ? "Preview off" : "Now playing";
}

function renderLastFrame() {
  if (settings.livePreview === false || previewDisabledByServer) {
    drawStandby("Live preview off", true);
    return;
  }
  if (!hasLiveFrame) {
    drawStandby("Waiting for display\u2026", false);
    return;
  }
  $("plateOff").hidden = true;
  $("nowLabel").classList.remove("idle");
  if (!lastStatus.workshop) $("nowLabelText").textContent = "Live";
  plateCtx.imageSmoothingEnabled = true;
  plateCtx.drawImage(srcCanvas, 0, 0, plate.width, plate.height);
}

function previewVisible() {
  return !document.hidden && settings.livePreview !== false && (desktop.matches || view === "now");
}

function schedulePreview(ms = 160) {
  clearTimeout(previewTimer);
  if (previewVisible()) previewTimer = setTimeout(pollPreview, ms);
}

function kickPreview() {
  clearTimeout(previewTimer);
  if (settings.livePreview === false) {
    previewDisabledByServer = false;
    hasLiveFrame = false;
    renderLastFrame();
    return;
  }
  previewDisabledByServer = false;
  if (previewVisible()) pollPreview();
}

async function pollPreview() {
  if (previewBusy || !previewVisible()) return;
  previewBusy = true;
  try {
    const r = await api("/api/preview");
    if (r.status === 404) {
      previewDisabledByServer = true;
      hasLiveFrame = false;
      drawStandby("Preview unavailable", false);
      return;
    }
    if (r.status === 204) {
      const reason = r.headers.get("X-Dropkick-Preview");
      if (reason === "disabled") {
        previewDisabledByServer = true;
        hasLiveFrame = false;
        renderLastFrame();
        return;
      }
      if (!hasLiveFrame) renderLastFrame();
    } else if (r.ok) {
      previewDisabledByServer = false;
      const blob = await r.blob();
      const bmp = await createImageBitmap(blob);
      srcCtx.drawImage(bmp, 0, 0, PREVIEW_W, PREVIEW_H);
      bmp.close?.();
      hasLiveFrame = true;
      renderLastFrame();
    }
  } catch (e) {
    if (e instanceof AuthError) return;
  } finally {
    previewBusy = false;
    if (previewVisible() && !previewDisabledByServer) schedulePreview(160);
  }
}

$("btnEnablePreview").onclick = () => {
  applySetting("livePreview", true);
  saveSetting("livePreview", true);
};
document.addEventListener("visibilitychange", () => { if (previewVisible()) kickPreview(); });

/* ---------- status ---------- */
let lastStatus = {};
async function refresh() {
  let s;
  try {
    s = await getJSON("/api/status");
    setConn("ok");
  } catch (e) {
    if (!(e instanceof AuthError)) setConn("retry");
    return;
  }
  const prevStatus = lastStatus;
  lastStatus = s;
  // Reload Browse when the catalog changes (pack switch, dislike, another controller) or
  // while the first load is still missing. Older servers only expose the size.
  const catalog = s.catalogRevision ?? "size:" + s.size;
  const catalogChanged = lastCatalog !== undefined && catalog !== lastCatalog;
  lastCatalog = catalog;
  if (catalogChanged || !presetsLoaded) loadPresets();

  const changed = (s.preset || "") !== currentPath;
  currentPath = s.preset || "";
  const { author, title } = splitName(currentPath ? nameOf(currentPath) : "");
  $("pname").textContent = currentPath ? title : "Nothing playing";
  $("pauthor").textContent = author;
  $("pauthor").hidden = !author;
  const tags = [];
  if (currentPath && !s.workshop) {
    tags.push(h("span", { class: "tag cat", text: catOf(currentPath) }));
    const sub = subOf(currentPath);
    if (sub) tags.push(h("span", { class: "tag", text: sub }));
  } else if (s.workshop) tags.push(h("span", { class: "tag", text: "Workshop scratch" }));
  $("ptags").replaceChildren(...tags);
  $("ppath").textContent = currentPath;
  $("pos").textContent = s.size ? `${(s.position ?? 0) + 1} / ${s.size}` : "0 / 0";
  $("miniName").textContent = $("pname").textContent;
  $("miniSub").textContent = s.workshop ? "Workshop · Live" : [author, currentPath && catOf(currentPath)].filter(Boolean).join(" · ");
  if (changed || s.workshop !== prevStatus.workshop) kickPreview();

  $("nowLabel").classList.toggle("workshop", !!s.workshop);
  $("nowLabelText").textContent = s.workshop ? "Workshop · Live" : (settings.livePreview === false || previewDisabledByServer ? "Preview off" : (hasLiveFrame ? "Live" : "Now playing"));
  catShuffle = s.categoryShuffle || "";
  const scope = s.favoritesShuffle ? "Favorites shuffle" : catShuffle ? "Shuffle · " + catShuffle.slice(prefix.length).replace(/\/$/, "") : "";
  $("scopeChip").hidden = !scope;
  $("scopeText").textContent = scope;
  syncCatShuffleButtons();

  setSwitch($("tShuffle"), s.shuffle);
  setSwitch($("tLock"), s.locked);
  setSwitch($("tFav"), s.favorited);
  setSwitch($("favShuffle"), s.favoritesShuffle);
  if (currentPath) {
    // Another controller may have starred or unstarred it.
    const was = favorites.has(currentPath);
    if (s.favorited) favorites.add(currentPath); else favorites.delete(currentPath);
    if (was !== !!s.favorited) {
      $$(`.star[data-i="${byPath.get(currentPath)?.i}"]`).forEach((b) => b.setAttribute("aria-pressed", String(!!s.favorited)));
      if (view === "fav") renderFavs();
    }
  }

  const dis = $("btnDislike");
  dis.disabled = !!s.workshop; // a live Workshop scratch is not a library preset
  dis.title = s.workshop ? "Unavailable while a Workshop preset is live" : "";

  if (s.workshop && edApplyPending) { edApplyPending = false; edStatus("Live on display", "ok"); }

  $("blockedCount").textContent = s.blocked || 0;
  $("dislikedCount").textContent = s.disliked || 0;
  $("btnClearBlock").disabled = !s.blocked;
  $("btnClearDislikes").disabled = !s.disliked;
  $$("[data-audio-name]").forEach((el) => (el.textContent = s.audio || "--"));
  const dev = audioDevices && (s.audioId !== undefined
    ? audioDevices.find((d) => d.id === s.audioId)
    : audioDevices.find((d) => d.name === s.audio));
  if (dev && document.activeElement !== $("audioSel")) $("audioSel").value = String(dev.id);

  // Telemetry. Windows builds have no thermal zone: hide the temperature instead of showing 0.
  if (s.fps !== undefined) { $("mFps").textContent = s.fps; $("pillText").textContent = s.fps + " fps"; }
  if (s.cpu !== undefined) $("mCpu").textContent = s.cpu;
  $("mMem").textContent = s.memTotal ? (s.memUsed / 1024).toFixed(1) + "/" + (s.memTotal / 1024).toFixed(1) + "G" : "--";
  const hasTemp = s.hasTemp !== false && s.temp !== undefined;
  $("mTempWrap").hidden = !hasTemp;
  if (hasTemp) $("mTemp").textContent = s.temp;

  if (changed) markCurrent();
}

/* ---------- now playing controls ---------- */
async function act(url, okMsg, delay = 300) {
  try { await post(url); if (okMsg) toast(okMsg); } catch (e) { fail(e); }
  setTimeout(refresh, delay);
}
const next = () => act("/api/next");
const prev = () => act("/api/prev");
const random = () => act("/api/random");
const lock = () => act("/api/lock");
const favCurrent = () => currentPath && toggleFavorite(currentPath).then(() => setTimeout(refresh, 200));

$("btnPrev").onclick = prev;
$("btnNext").onclick = next;
$("btnRandom").onclick = random;
$("miniNext").onclick = next;
$("miniRandom").onclick = random;
$("miniOpen").onclick = () => showView("now");
$("tShuffle").onclick = () => act("/api/shuffle");
$("tLock").onclick = lock;
$("tFav").onclick = favCurrent;
$("favShuffle").onclick = async () => {
  try {
    const r = await post("/api/favorites/shuffle");
    setSwitch($("favShuffle"), r.favoritesShuffle);
    toast(r.favoritesShuffle ? "Shuffling favorites" : "Favorites shuffle off");
  } catch (e) { fail(e); }
  setTimeout(refresh, 250);
};
$("pathToggle").onclick = () => {
  const open = $("pathToggle").getAttribute("aria-expanded") !== "true";
  $("pathToggle").setAttribute("aria-expanded", String(open));
  $("ppath").hidden = !open;
};
$("btnDislike").onclick = async () => {
  const b = $("btnDislike");
  b.classList.add("flash");
  setTimeout(() => b.classList.remove("flash"), 600);
  await act("/api/dislike", "Disliked — it won't play again", 500);
};
$("btnClearBlock").onclick = () => act("/api/blocklist/clear", "Blocklist cleared");
$("btnClearDislikes").onclick = () => act("/api/dislikes/clear", "Dislikes cleared");

/* ---------- settings ----------
 * Every control carries data-setting="<key>"; the Quick visuals card and the
 * Settings tab share keys and stay in sync through applySetting(). */
let settings = {};
const fmtNum = (n) => String(Math.round(n * 100) / 100);
function fmtValue(key, v) {
  const el = document.querySelector(`input[type=range][data-setting="${key}"]`);
  const unit = el?.dataset.unit;
  if (unit === "%") return Math.round(v * 100) + "%";
  if (unit === "s") return fmtNum(v) + " s";
  return fmtNum(v);
}
function setPct(r) {
  const min = +r.min || 0, max = +r.max || 100;
  r.style.setProperty("--pct", ((r.value - min) / (max - min)) * 100 + "%");
}
const activeEdits = new Set(); // sliders being dragged; server values must not yank them
function applySetting(key, v) {
  settings[key] = v;
  $$(`[data-setting="${key}"]`).forEach((el) => {
    if (el.getAttribute("role") === "switch") setSwitch(el, !!v);
    else if (el.type === "range") { if (!activeEdits.has(el)) el.value = v; setPct(el); }
    else if (el.type === "color") el.value = String(v).startsWith("#") ? v : "#" + v;
  });
  $$(`output[data-out="${key}"]`).forEach((o) => (o.textContent = fmtValue(key, +v)));
  if (key === "livePreview" && !v) kickPreview();
  if (key === "tintColor" || key === "tintEnabled" || key === "tintStrength") { markSwatch(); renderLastFrame(); }
}
async function saveSetting(key, value) {
  try {
    await post(`/api/settings?key=${q(key)}&value=${q(String(value))}`);
    if (key === "livePreview" || key === "brightness" || key === "tintEnabled" || key === "tintColor" || key === "tintStrength" || key === "reduceFlashing") kickPreview();
  } catch (e) {
    fail(e);
    loadSettings(); // put every control back to what the server actually holds
  }
}
async function loadSettings() {
  try {
    const s = await getJSON("/api/settings");
    for (const [k, v] of Object.entries(s)) applySetting(k, v);
    if (s.livePreview !== false) kickPreview();
  } catch (e) {}
}

$$('input[type=range][data-setting]').forEach((r) => {
  setPct(r);
  const end = () => activeEdits.delete(r);
  r.addEventListener("pointerdown", () => activeEdits.add(r));
  r.addEventListener("pointerup", end);
  r.addEventListener("pointercancel", end);
  r.addEventListener("input", () => { activeEdits.add(r); applySetting(r.dataset.setting, +r.value); });
  r.addEventListener("change", () => { end(); saveSetting(r.dataset.setting, r.value); });
});
$$('.switch[data-setting]').forEach((b) => (b.onclick = () => {
  const on = !isOn(b);
  applySetting(b.dataset.setting, on);
  saveSetting(b.dataset.setting, on);
}));
$("tintColor").addEventListener("change", () => setTint($("tintColor").value));

// [value sent to the display, calibrated color shown on screen, name]
const SWATCHES = [["#00ff00", "#5ec47c", "Phosphor green"], ["#0080ff", "#5b95e8", "Blue"], ["#ff40c0", "#d46eb8", "Magenta"],
  ["#ffb000", "#dfa63d", "Amber"], ["#00e5e5", "#4bc0bd", "Cyan"], ["#ff3030", "#e06659", "Red"], ["#ffffff", "#d8d3c9", "White"]];
$$("[data-swatches]").forEach((host) => host.append(...SWATCHES.map(([c, shown, name]) =>
  h("button", { type: "button", class: "swatch", "data-color": c, "aria-label": "Tint " + name,
    "aria-pressed": "false", style: "background:" + shown }))));
$$("[data-swatches]").forEach((host) => host.addEventListener("click", (e) => {
  const sw = e.target.closest(".swatch");
  if (sw) setTint(sw.dataset.color, true);
}));
function setTint(color, enable) {
  applySetting("tintColor", color);
  saveSetting("tintColor", color.replace("#", ""));
  if (enable && !settings.tintEnabled) { applySetting("tintEnabled", true); saveSetting("tintEnabled", true); }
}
function markSwatch() {
  const c = String(settings.tintColor || "").replace(/^#?/, "#").toLowerCase();
  $$(".swatch").forEach((s) => s.setAttribute("aria-pressed", String(!!settings.tintEnabled && s.dataset.color === c)));
  document.documentElement.style.setProperty("--tint-ui", SWATCHES.find(([v]) => v === c)?.[1] || c);
}

/* ---------- packs ---------- */
let packs = [];
async function loadPacks() {
  try {
    packs = await getJSON("/api/packs");
    $("packs").replaceChildren(...packs.map((n) => h("option", { value: n, text: n })));
    syncPackSelect();
  } catch (e) {}
}
function syncPackSelect() {
  const seg = prefix.replace(/\/$/, "").split("/").pop();
  if (packs.includes(seg)) $("packs").value = seg;
}
$("packs").addEventListener("change", async (e) => {
  const name = e.target.value;
  try {
    await post("/api/pack?name=" + q(name));
    toast("Loading pack " + name);
    openCats.clear();
    setTimeout(() => { loadPresets(); refresh(); }, 1200);
  } catch (err) { fail(err); }
});

/* ---------- audio input ---------- */
let audioDevices = null; // null = server has no device list; fall back to "Next input"
async function loadAudio() {
  try {
    const r = await api("/api/audio/devices");
    if (!r.ok) throw new Error("unsupported");
    const data = await r.json();
    const list = Array.isArray(data) ? data : data.devices || [];
    // Backend shape: {current, devices: [{id, name}]}; id -1 is the system default.
    audioDevices = list.map((d, i) => (typeof d === "string" ? { id: i, name: d } : { id: d.id ?? d.index, name: d.name }));
    $("audioSel").replaceChildren(...audioDevices.map((d) => h("option", { value: String(d.id), text: d.name })));
    if (!Array.isArray(data) && data.current !== undefined) $("audioSel").value = String(data.current);
    $("audioSelWrap").hidden = false;
    $("btnAudioNext").hidden = true;
  } catch (e) {
    if (e instanceof AuthError) return;
    audioDevices = null;
    $("audioSelWrap").hidden = true;
    $("btnAudioNext").hidden = false;
  }
}
$("audioSel").addEventListener("change", async (e) => {
  const d = audioDevices?.find((x) => String(x.id) === e.target.value);
  if (!d) return;
  try { await post("/api/audio/select?index=" + d.id); toast("Audio input: " + d.name); } catch (err) { fail(err); }
  setTimeout(refresh, 400);
});
$("btnAudioNext").onclick = () => act("/api/audio/next", "Switched audio input", 600);

/* ---------- workshop editor ---------- */
const edText = $("edText");
function edStatus(msg, kind = "") { $("edMsg").textContent = msg; $("edMsg").className = "edmsg " + kind; }
function edStats() {
  const v = edText.value;
  $("edStats").textContent = `${v ? v.split("\n").length : 0} lines · ${v.length.toLocaleString()} chars`;
}
/* Editing stays off until the source arrives, so a slow load can't overwrite typing
 * and Apply/Save can't send an empty buffer. */
function setEdBusy(busy) {
  edText.disabled = $("edApply").disabled = $("edSave").disabled = busy;
}
let edSeq = 0, edReturnFocus = null, edApplyPending = false;
// The editor is a full-screen overlay; inert keeps Tab and screen readers out of the page behind it.
const edBackground = () => [document.querySelector(".topbar"), document.querySelector(".main")];
async function openEditor() {
  const seq = ++edSeq;
  edReturnFocus = document.activeElement;
  edBackground().forEach((el) => (el.inert = true));
  $("editor").hidden = false;
  document.body.style.overflow = "hidden";
  edApplyPending = false;
  edStatus("Loading...");
  edText.value = "";
  edText.placeholder = "Loading...";
  setEdBusy(true);
  edStats();
  $("edClose").focus();
  try {
    const s = await getJSON("/api/workshop/source");
    if (seq !== edSeq) return; // closed or reopened meanwhile
    edText.value = s.text || "";
    $("edName").textContent = (s.path || "").split("/").pop() || "preset.milk";
    edStats();
    edStatus(s.text ? "Edit, then Apply to see it on screen. Esc closes." : "Couldn't read this preset's source.", s.text ? "" : "error");
    setEdBusy(false);
    edText.focus();
    edText.setSelectionRange(0, 0);
    edText.scrollTop = 0;
  } catch (e) {
    if (seq === edSeq) edStatus("Failed to load source. Close and try again.", "error");
  } finally {
    if (seq === edSeq) edText.placeholder = "";
  }
}
function closeEditor() {
  edSeq++;
  edApplyPending = false;
  $("editor").hidden = true;
  document.body.style.overflow = "";
  edBackground().forEach((el) => (el.inert = false));
  edReturnFocus?.focus?.();
  edReturnFocus = null;
}
async function applyEdit() {
  if (edText.disabled) return;
  edStatus("Applying...");
  try {
    await post("/api/workshop/apply", edText.value);
    // The display loads it on its next frame; refresh() confirms via status.workshop.
    edApplyPending = true;
    edStatus("Sent to display...");
    setTimeout(refresh, 150);
  } catch (e) { edStatus("Apply failed: " + e.message, "error"); }
}

let saveOverwrite = false;
function setOverwrite(on) {
  saveOverwrite = on;
  $("saveWarn").hidden = !on;
  $("saveOk").textContent = on ? "Overwrite" : "Save";
}
function saveEdit() {
  if (edText.disabled) return;
  const cur = $("edName").textContent;
  $("saveName").value = /^_scratch\.milk$/.test(cur) ? "my-preset.milk" : cur;
  setOverwrite(false);
  $("saveDlg").showModal();
  $("saveName").select();
}
$("saveName").addEventListener("input", () => setOverwrite(false));
$("saveCancel").onclick = () => $("saveDlg").close();
$("saveForm").addEventListener("submit", async (e) => {
  e.preventDefault();
  const name = $("saveName").value.trim();
  if (!name) return;
  $("saveOk").disabled = true;
  try {
    const r = await api(`/api/workshop/save?name=${q(name)}&overwrite=${saveOverwrite ? 1 : 0}`,
      { method: "POST", body: edText.value });
    const data = await r.json().catch(() => ({}));
    if (r.status === 409) { setOverwrite(true); return; }
    if (!r.ok) throw new Error(data.error || `HTTP ${r.status}`);
    $("saveDlg").close();
    const file = data.file || name;
    $("edName").textContent = file;
    edStatus("Saved " + file, "ok");
    toast("Saved " + file + " to workshop");
  } catch (err) {
    $("saveDlg").close();
    if (!(err instanceof AuthError)) edStatus("Save failed: " + err.message, "error");
  } finally { $("saveOk").disabled = false; }
});
$("btnEdit").onclick = openEditor;
$("editCurrent").onclick = openEditor;
$("edClose").onclick = closeEditor;
$("edApply").onclick = applyEdit;
$("edSave").onclick = saveEdit;
edText.addEventListener("input", edStats);
edText.addEventListener("keydown", (e) => {
  if (e.key === "Tab" && !e.shiftKey && !e.ctrlKey && !e.metaKey && !e.altKey) {
    e.preventDefault();
    edText.setRangeText("  ", edText.selectionStart, edText.selectionEnd, "end");
    edStats();
  } else if (e.key === "Enter" && (e.ctrlKey || e.metaKey)) {
    e.preventDefault();
    applyEdit();
  }
});

/* ---------- access token gate ---------- */
function openAuth() {
  const d = $("authDlg");
  if (d.open) return;
  $("authToken").value = "";
  d.showModal();
}
$("authDlg").addEventListener("cancel", (e) => e.preventDefault()); // nothing works without it
$("authDlg").addEventListener("close", () => {
  const t = $("authToken").value.trim();
  if (!t) { openAuth(); return; }
  token = t;
  store("session", TOKEN_KEY, t);
  history.replaceState(null, "", "/"); // a stale token path would otherwise win on reload
  boot();
});

/* ---------- keyboard ---------- */
document.addEventListener("keydown", (e) => {
  if (e.key === "Escape" && !$("telePop").hidden) { setTelePop(false); $("statusPill").focus(); return; }
  if (e.key === "Escape" && !$("editor").hidden && !document.querySelector("dialog[open]")) { closeEditor(); return; }
  if (e.ctrlKey || e.metaKey || e.altKey || e.defaultPrevented) return;
  const t = e.target;
  if (t.closest("input, textarea, select, [contenteditable]") || document.querySelector("dialog[open]") || !$("editor").hidden) return;
  if (e.key === " " && t.closest("button")) return; // let Space press the focused button
  const keys = {
    ArrowRight: next, n: next, ArrowLeft: prev, p: prev, r: random, " ": lock, l: lock, f: favCurrent,
    "/": () => { showView("browse"); $("search").focus(); },
    1: () => showView("now"), 2: () => showView("browse"), 3: () => showView("fav"), 4: () => showView("set"),
  };
  const fn = keys[e.key];
  if (fn) { e.preventDefault(); fn(); }
});

/* ---------- polling (paused while the tab is hidden) ---------- */
document.addEventListener("visibilitychange", () => {
  if (!document.hidden) { refresh(); loadSettings(); }
});

let polling = false;
async function boot() {
  previewReady = true;
  startBgDither();
  drawStandby("Waiting for display\u2026", false);
  await Promise.all([loadFavorites(), loadPacks()]); // packPrefix() uses the pack names
  await loadPresets();
  await Promise.all([loadSettings(), loadAudio()]);
  await refresh();
  kickPreview();
  if (!polling) {
    polling = true;
    setInterval(() => { if (!document.hidden && $("conn").dataset.state !== "auth") refresh(); }, 2000);
  }
}
boot();
