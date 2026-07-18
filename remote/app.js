/* Dropkick remote v2 */
// The token comes either from ?token=... or from the path (http://host:8080/op1),
// the latter being much easier to type on a phone.
const token = new URLSearchParams(location.search).get("token")
  || decodeURIComponent(location.pathname.replace(/^\/+|\/+$/g, ""))
  || "";
// Send the token in a header, not the query string, so it doesn't leak into
// server access logs, browser history, or Referer headers.
const authHeaders = token ? { "X-Dropkick-Token": token } : {};
const GET = (u) => fetch(u, { headers: authHeaders }).then(r => r.json());
const POST = (u, body) => fetch(u, { method: "POST", headers: authHeaders, body });
const act = (u, delay = 400) => { POST(u); setTimeout(refresh, delay); };
const $ = (id) => document.getElementById(id);
// Escape untrusted text (preset names come from the filesystem) before it goes
// into innerHTML, to prevent a crafted preset filename from injecting markup.
const esc = (s) => String(s).replace(/[&<>"']/g, (c) => (
  { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
const STAR = '<svg class="st" viewBox="0 0 24 24"><path d="M12 3l2.9 6 6.6.9-4.8 4.6 1.2 6.5L12 17.8 6.1 21l1.2-6.5L2.5 9.9 9 9z"/></svg>';
const SHUF = '<svg class="csh" viewBox="0 0 24 24"><path d="M10.59 9.17L5.41 4 4 5.41l5.17 5.17 1.42-1.41zM14.5 4l2.04 2.04L4 18.59 5.41 20 17.96 7.46 20 9.5V4h-5.5zm.33 9.41l-1.41 1.41 3.13 3.13L14.5 20H20v-5.5l-2.04 2.04-3.13-3.13z"/></svg>';

/* ---------- theme ---------- */
const themePref = localStorage.getItem("theme");
if (themePref) document.documentElement.dataset.theme = themePref;
else if (matchMedia("(prefers-color-scheme: dark)").matches) document.documentElement.dataset.theme = "dark";
$("theme").onclick = () => {
  const next = document.documentElement.dataset.theme === "dark" ? "light" : "dark";
  document.documentElement.dataset.theme = next;
  localStorage.setItem("theme", next);
};

/* ---------- tabs ---------- */
document.querySelectorAll(".tab").forEach(t => t.onclick = () => {
  document.querySelectorAll(".tab").forEach(x => x.classList.remove("on"));
  t.classList.add("on");
  document.querySelectorAll(".view").forEach(v => v.classList.toggle("on", v.dataset.view === t.dataset.t));
  window.scrollTo(0, 0);
  if (t.dataset.t === "fav") renderFavs();
  if (t.dataset.t === "set") loadSettings();
});

/* ---------- preset data ---------- */
let presets = [];          // [{i,p}]
let byPath = new Map();
let prefix = "";           // common dir prefix
let groups = new Map();    // category -> [preset]
let favorites = new Set();
let currentPath = "";
let catShuffleActive = "";  // server's active category-shuffle prefix (incl. trailing /), "" = off

const nameOf = (p) => p.split("/").pop().replace(/\.milk$/i, "");
const relOf = (p) => p.startsWith(prefix) ? p.slice(prefix.length) : p;
const catOf = (p) => { const parts = relOf(p).split("/"); return parts.length > 1 ? parts[0] : "Presets"; };
const subOf = (p) => { const parts = relOf(p).split("/"); return parts.length > 2 ? parts.slice(1, -1).join(" / ") : ""; };

function commonPrefix(paths) {
  if (!paths.length) return "";
  let pre = paths[0];
  for (const p of paths) { while (!p.startsWith(pre)) { pre = pre.slice(0, pre.lastIndexOf("/", pre.length - 2) + 1); if (!pre) return ""; } }
  return pre.slice(0, pre.lastIndexOf("/") + 1);
}

async function loadPresets() {
  try {
    presets = await GET("/api/presets");
    byPath = new Map(presets.map(x => [x.p, x]));
    prefix = commonPrefix(presets.map(x => x.p));
    groups = new Map();
    for (const x of presets) {
      const c = catOf(x.p);
      if (!groups.has(c)) groups.set(c, []);
      groups.get(c).push(x);
    }
    renderBrowse();
  } catch (e) { /* retried by next poll */ }
}

async function loadFavorites() {
  try { favorites = new Set(await GET("/api/favorites")); } catch (e) {}
}

async function toggleFavorite(path) {
  const r = await (await POST("/api/favorites/toggle?path=" + encodeURIComponent(path))).json();
  if (r.favorited) favorites.add(path); else favorites.delete(path);
  refresh();
  return r.favorited;
}

async function toggleCategoryShuffle(dir) {
  const r = await (await POST("/api/category/shuffle?path=" + encodeURIComponent(dir))).json();
  catShuffleActive = r.categoryShuffle ? dir + "/" : "";
  renderBrowse();
  setTimeout(refresh, 300);
}

/* ---------- rendering ---------- */
function rowEl(x) {
  const row = document.createElement("div");
  row.className = "row" + (x.p === currentPath ? " now" : "");
  const sub = subOf(x.p);
  row.innerHTML = `<span class="ix">${String(x.i).padStart(4, "0")}</span>` +
    `<span class="n">${esc(nameOf(x.p))}${sub ? `<small>${esc(sub)}</small>` : ""}</span>` + STAR;
  const star = row.querySelector(".st");
  star.classList.toggle("on", favorites.has(x.p));
  star.onclick = async (e) => {
    e.stopPropagation();
    star.classList.toggle("on", await toggleFavorite(x.p));
  };
  row.onclick = () => act("/api/preset?index=" + x.i);
  return row;
}

const openCats = new Set();
function renderBrowse() {
  const host = $("browseList");
  host.innerHTML = "";
  const q = $("search").value.trim().toLowerCase();
  if (q.length >= 2) {
    const hits = presets.filter(x => x.p.toLowerCase().includes(q)).slice(0, 300);
    if (!hits.length) { host.innerHTML = '<div class="hint">No presets match.</div>'; return; }
    hits.forEach(x => host.appendChild(rowEl(x)));
    return;
  }
  for (const [cat, items] of groups) {
    const head = document.createElement("div");
    head.className = "chead";
    const dir = prefix + cat;                       // real folder this group came from
    const canShuffle = cat !== "Presets";           // flat packs have no folder to scope to
    const shufActive = catShuffleActive === dir + "/";
    head.innerHTML = `<span class="c">${openCats.has(cat) ? "▾ " : "▸ "}${esc(cat)}</span>` +
      `<span class="cr">${canShuffle ? SHUF : ""}<span class="ct">${items.length}</span></span>`;
    head.onclick = () => { openCats.has(cat) ? openCats.delete(cat) : openCats.add(cat); renderBrowse(); };
    const shuf = head.querySelector(".csh");
    if (shuf) {
      shuf.classList.toggle("on", shufActive);
      shuf.onclick = (e) => { e.stopPropagation(); toggleCategoryShuffle(dir); };
    }
    host.appendChild(head);
    if (openCats.has(cat)) items.forEach(x => host.appendChild(rowEl(x)));
  }
  if (!groups.size) host.innerHTML = '<div class="hint">Loading preset list…</div>';
}
let searchTimer;
$("search").addEventListener("input", () => { clearTimeout(searchTimer); searchTimer = setTimeout(renderBrowse, 200); });

function renderFavs() {
  const host = $("favList");
  host.innerHTML = "";
  const items = [...favorites].map(p => byPath.get(p)).filter(Boolean).sort((a, b) => a.i - b.i);
  $("favCount").textContent = items.length;
  if (!items.length) { host.innerHTML = '<div class="hint">No favorites yet — tap the star on any preset.</div>'; return; }
  items.forEach(x => host.appendChild(rowEl(x)));
}

/* ---------- status ---------- */
async function refresh() {
  try {
    const s = await GET("/api/status");
    currentPath = s.preset || "";
    $("pname").textContent = currentPath ? nameOf(currentPath) : "—";
    $("ppath").textContent = currentPath;
    $("pcat").textContent = currentPath ? (catOf(currentPath) + (subOf(currentPath) ? " · " + subOf(currentPath) : "")) : "—";
    $("pos").textContent = `${s.position} / ${s.size}`;
    $("posIdx").textContent = `▸ POS ${s.position}`;
    $("audioSrc").textContent = s.audio || "—";
    $("audioName").textContent = s.audio || "—";
    $("tShuffle").classList.toggle("on", !!s.shuffle);
    $("tLock").classList.toggle("on", !!s.locked);
    $("tFav").classList.toggle("on", !!s.favorited);
    $("favShuffle").classList.toggle("on", !!s.favoritesShuffle);
    const cs = s.categoryShuffle || "";
    if (cs !== catShuffleActive) { catShuffleActive = cs; renderBrowse(); }
    $("nowLabel").textContent = s.workshop ? "WORKSHOP · LIVE EDIT" : "NOW PLAYING";
    $("nowLabel").classList.toggle("editing", !!s.workshop);
    const blocked = s.blocked || 0;
    $("blockedRow").style.display = blocked > 0 ? "" : "none";
    $("blockedCount").textContent = blocked;
    const disliked = s.disliked || 0;
    $("dislikedRow").style.display = disliked > 0 ? "" : "none";
    $("dislikedCount").textContent = disliked;
    // System monitor
    if (s.fps !== undefined) $("stFps").textContent = s.fps;
    if (s.cpu !== undefined) $("stCpu").textContent = s.cpu;
    if (s.temp !== undefined) $("stTemp").textContent = s.temp;
    if (s.memTotal) $("stMem").textContent = (s.memUsed / 1024).toFixed(1) + "/" + (s.memTotal / 1024).toFixed(1) + "G";
  } catch (e) { /* transient */ }
}

$("screen").onclick = () => $("screen").classList.toggle("open");
$("btnPrev").onclick = () => act("/api/prev");
$("btnNext").onclick = () => act("/api/next");
$("btnRandom").onclick = () => act("/api/random");
$("tShuffle").onclick = () => act("/api/shuffle");
$("tLock").onclick = () => act("/api/lock");
$("tFav").onclick = () => { if (currentPath) toggleFavorite(currentPath); };
$("favShuffle").onclick = () => act("/api/favorites/shuffle", 300);
async function openEditor() {
  const msg = $("edMsg"); msg.textContent = "Loading…";
  $("editor").classList.add("on");
  try {
    const s = await GET("/api/workshop/source");
    $("edText").value = s.text || "";
    $("edName").textContent = (s.path || "").split("/").pop() || "preset";
    msg.textContent = s.text ? "" : "Couldn't read source.";
  } catch (e) { msg.textContent = "Failed to load source."; }
}
async function applyEdit() {
  $("edMsg").textContent = "Applying…";
  await POST("/api/workshop/apply", $("edText").value);
  $("edMsg").textContent = "Applied — showing on screen.";
  setTimeout(refresh, 600);
}
async function saveEdit() {
  const name = prompt("Save as (in workshop/):", $("edName").textContent.replace(/^_scratch\.milk$/, "my-preset.milk"));
  if (!name) return;
  await POST("/api/workshop/save?name=" + encodeURIComponent(name), $("edText").value);
  $("edMsg").textContent = "Saved " + name;
}
$("btnCapture").onclick = openEditor;
$("edClose").onclick = () => $("editor").classList.remove("on");
$("edApply").onclick = applyEdit;
$("edSave").onclick = saveEdit;
$("btnAudio").onclick = () => act("/api/audio/next", 600);
$("btnClearBlock").onclick = () => act("/api/blocklist/clear", 500);
$("btnDislike").onclick = (e) => {
  e.stopPropagation();               // don't also toggle the screen's path-reveal
  const b = $("btnDislike");
  b.classList.add("flash");
  setTimeout(() => b.classList.remove("flash"), 600);
  act("/api/dislike", 500);
};
$("btnClearDislikes").onclick = () => act("/api/dislikes/clear", 500);

/* ---------- settings ---------- */
const fmt1 = (n) => Math.round(n * 100) / 100;
async function loadSettings() {
  try {
    const s = await GET("/api/settings");
    $("sDur").value = s.presetDuration; $("vDur").textContent = fmt1(s.presetDuration);
    $("sSoft").value = s.softCutDuration; $("vSoft").textContent = fmt1(s.softCutDuration);
    $("sBeat").value = s.beatSensitivity; $("vBeat").textContent = fmt1(s.beatSensitivity);
    $("sHardS").value = s.hardCutSensitivity; $("vHardS").textContent = fmt1(s.hardCutSensitivity);
    $("sHardD").value = s.hardCutDuration; $("vHardD").textContent = fmt1(s.hardCutDuration);
    $("sFps").value = s.fps; $("vFps").textContent = s.fps;
    $("tHard").classList.toggle("on", !!s.hardCut);
    $("tAspect").classList.toggle("on", !!s.aspectCorrection);
    $("sFlash").value = s.flashStrength; $("vFlash").textContent = fmt1(s.flashStrength);
    $("tFlash").classList.toggle("on", !!s.reduceFlashing);
    $("sBright").value = s.brightness; $("vBright").textContent = Math.round(s.brightness * 100);
    $("tTint").classList.toggle("on", !!s.tintEnabled);
    const tc = s.tintColor || "#00ff00";
    $("tintColor").value = tc.charAt(0) === "#" ? tc : "#" + tc; // input[type=color] needs #rrggbb
    $("sTintS").value = s.tintStrength; $("vTintS").textContent = Math.round(s.tintStrength * 100);
    $("tAutoskip").classList.toggle("on", !!s.autoskipEnabled);
    $("sAskFps").value = s.autoskipFps; $("vAskFps").textContent = s.autoskipFps;
    $("sAskStr").value = s.autoskipStrikes; $("vAskStr").textContent = s.autoskipStrikes;
  } catch (e) {}
}
function bindSlider(sid, vid, fmt) {
  $(sid).addEventListener("input", () => { $(vid).textContent = fmt(parseFloat($(sid).value)); });
  $(sid).addEventListener("change", () => POST(`/api/settings?key=${$(sid).dataset.key}&value=${$(sid).value}`));
}
const valueLabel = { sDur: "vDur", sSoft: "vSoft", sBeat: "vBeat", sHardS: "vHardS", sHardD: "vHardD", sFps: "vFps", sFlash: "vFlash", sAskFps: "vAskFps", sAskStr: "vAskStr" };
for (const [sid, vid] of Object.entries(valueLabel)) bindSlider(sid, vid, fmt1);
// Percent-labelled sliders (0..1 shown as 0..100%).
bindSlider("sBright", "vBright", (v) => Math.round(v * 100));
bindSlider("sTintS", "vTintS", (v) => Math.round(v * 100));

function bindToggle(id, key) {
  $(id).onclick = () => {
    const on = !$(id).classList.contains("on");
    $(id).classList.toggle("on", on);
    POST(`/api/settings?key=${key}&value=${on}`);
  };
}
bindToggle("tHard", "hardCut");
bindToggle("tAspect", "aspectCorrection");
bindToggle("tFlash", "reduceFlashing");
bindToggle("tTint", "tintEnabled");
bindToggle("tAutoskip", "autoskipEnabled");
$("tintColor").addEventListener("change", () => {
  POST(`/api/settings?key=tintColor&value=${encodeURIComponent($("tintColor").value.replace("#", ""))}`);
});
document.querySelectorAll("#tintSwatches .swatch").forEach(sw => sw.onclick = () => {
  const col = sw.dataset.color;
  $("tintColor").value = col;
  $("tTint").classList.add("on");
  POST(`/api/settings?key=tintColor&value=${encodeURIComponent(col.replace("#", ""))}`);
  POST(`/api/settings?key=tintEnabled&value=true`);
});

/* ---------- packs ---------- */
async function loadPacks() {
  try {
    const packs = await GET("/api/packs");
    const sel = $("packs");
    sel.innerHTML = "";
    packs.forEach(name => {
      const o = document.createElement("option");
      o.value = name; o.textContent = name;
      sel.appendChild(o);
    });
    if (prefix) {
      const seg = prefix.replace(/\/$/, "").split("/").pop();
      if (packs.includes(seg)) { sel.value = seg; $("packName").textContent = seg; }
    }
  } catch (e) {}
}
$("packs").addEventListener("change", async (e) => {
  await POST("/api/pack?name=" + encodeURIComponent(e.target.value));
  $("packName").textContent = e.target.value;
  setTimeout(() => { loadPresets(); refresh(); }, 1500);
});

/* ---------- boot ---------- */
(async () => {
  await loadFavorites();
  await loadPresets();
  await loadPacks();
  refresh();
  setInterval(refresh, 2000);
})();
