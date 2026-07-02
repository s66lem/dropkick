/* Dropkick remote v2 */
const token = new URLSearchParams(location.search).get("token") || "";
const withToken = (u) => token ? u + (u.includes("?") ? "&" : "?") + "token=" + encodeURIComponent(token) : u;
const GET = (u) => fetch(withToken(u)).then(r => r.json());
const POST = (u) => fetch(withToken(u), { method: "POST" });
const $ = (id) => document.getElementById(id);
const STAR = '<svg class="st" viewBox="0 0 24 24"><path d="M12 3l2.9 6 6.6.9-4.8 4.6 1.2 6.5L12 17.8 6.1 21l1.2-6.5L2.5 9.9 9 9z"/></svg>';

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

/* ---------- rendering ---------- */
function rowEl(x) {
  const row = document.createElement("div");
  row.className = "row" + (x.p === currentPath ? " now" : "");
  const sub = subOf(x.p);
  row.innerHTML = `<span class="ix">${String(x.i).padStart(4, "0")}</span>` +
    `<span class="n">${nameOf(x.p)}${sub ? `<small>${sub}</small>` : ""}</span>` + STAR;
  const star = row.querySelector(".st");
  star.classList.toggle("on", favorites.has(x.p));
  star.onclick = async (e) => {
    e.stopPropagation();
    const r = await (await POST("/api/favorites/toggle?path=" + encodeURIComponent(x.p))).json();
    if (r.favorited) favorites.add(x.p); else favorites.delete(x.p);
    star.classList.toggle("on", r.favorited);
    refresh();
  };
  row.onclick = () => { POST("/api/preset?index=" + x.i); setTimeout(refresh, 400); };
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
    head.innerHTML = `<span class="c">${openCats.has(cat) ? "▾ " : "▸ "}${cat}</span><span class="ct">${items.length}</span>`;
    head.onclick = () => { openCats.has(cat) ? openCats.delete(cat) : openCats.add(cat); renderBrowse(); };
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
    $("nowLabel").textContent = s.workshop ? "WORKSHOP · LIVE EDIT" : "NOW PLAYING";
    $("nowLabel").classList.toggle("editing", !!s.workshop);
  } catch (e) { /* transient */ }
}

$("screen").onclick = () => $("screen").classList.toggle("open");
$("btnPrev").onclick = () => { POST("/api/prev"); setTimeout(refresh, 400); };
$("btnNext").onclick = () => { POST("/api/next"); setTimeout(refresh, 400); };
$("btnRandom").onclick = () => { POST("/api/random"); setTimeout(refresh, 400); };
$("tShuffle").onclick = () => { POST("/api/shuffle"); setTimeout(refresh, 400); };
$("tLock").onclick = () => { POST("/api/lock"); setTimeout(refresh, 400); };
$("tFav").onclick = async () => {
  if (!currentPath) return;
  const r = await (await POST("/api/favorites/toggle?path=" + encodeURIComponent(currentPath))).json();
  if (r.favorited) favorites.add(currentPath); else favorites.delete(currentPath);
  refresh();
};
$("favShuffle").onclick = () => { POST("/api/favorites/shuffle"); setTimeout(refresh, 300); };
$("btnCapture").onclick = async () => {
  const b = $("btnCapture");
  await POST("/api/workshop/capture");
  b.textContent = "✎ Copied to workshop/ — edit & save to hot-reload";
  b.classList.add("done");
  setTimeout(() => { b.textContent = "✎ Edit this preset in Workshop"; b.classList.remove("done"); }, 4000);
  setTimeout(refresh, 500);
};
$("btnAudio").onclick = () => { POST("/api/audio/next"); setTimeout(refresh, 600); };

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
  } catch (e) {}
}
const valueLabel = { sDur: "vDur", sSoft: "vSoft", sBeat: "vBeat", sHardS: "vHardS", sHardD: "vHardD", sFps: "vFps" };
for (const [sid, vid] of Object.entries(valueLabel)) {
  $(sid).addEventListener("input", () => { $(vid).textContent = fmt1(parseFloat($(sid).value)); });
  $(sid).addEventListener("change", () => POST(`/api/settings?key=${$(sid).dataset.key}&value=${$(sid).value}`));
}
$("tHard").onclick = () => {
  const on = !$("tHard").classList.contains("on");
  $("tHard").classList.toggle("on", on);
  POST(`/api/settings?key=hardCut&value=${on}`);
};
$("tAspect").onclick = () => {
  const on = !$("tAspect").classList.contains("on");
  $("tAspect").classList.toggle("on", on);
  POST(`/api/settings?key=aspectCorrection&value=${on}`);
};

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
