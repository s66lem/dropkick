const token = new URLSearchParams(location.search).get("token") || "";

function withToken(url) {
  if (!token) { return url; }
  return url + (url.includes("?") ? "&" : "?") + "token=" + encodeURIComponent(token);
}

async function post(action) {
  await fetch(withToken(`/api/${action}`), { method: "POST" });
  refresh();
}

async function loadPacks() {
  const packs = await (await fetch(withToken("/api/packs"))).json();
  const sel = document.getElementById("packs");
  sel.innerHTML = "";
  packs.forEach((name) => {
    const opt = document.createElement("option");
    opt.value = name; opt.textContent = name;
    sel.appendChild(opt);
  });
}

async function refresh() {
  try {
    const s = await (await fetch(withToken("/api/status"))).json();
    document.getElementById("preset").textContent = s.preset || "(none)";
    document.getElementById("audio").textContent = s.audio ? `Audio: ${s.audio}` : "";
    document.getElementById("shuffle").classList.toggle("on", !!s.shuffle);
    document.getElementById("lock").classList.toggle("on", !!s.locked);
  } catch (e) { /* transient */ }
}

document.querySelectorAll("button[data-action]").forEach((b) =>
  b.addEventListener("click", () => post(b.dataset.action)));

document.getElementById("packs").addEventListener("change", (e) => {
  fetch(withToken(`/api/pack?name=${encodeURIComponent(e.target.value)}`), { method: "POST" }).then(refresh);
});

loadPacks();
refresh();
setInterval(refresh, 2000);
