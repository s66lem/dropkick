// Remote web UI + mock API tests.   node --test tests/remote/test_remote.mjs
import { test, describe, before, after } from "node:test";
import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { createMockServer, PRESET_ROOT, PACKS, XSS_NAME } from "../../scripts/dev-remote.mjs";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..");
const read = (rel) => fs.readFileSync(path.join(ROOT, rel), "utf8");
const html = read("remote/index.html");
const css = read("remote/style.css");
const js = read("remote/app.js");

describe("syntax", () => {
  for (const f of ["remote/app.js", "scripts/dev-remote.mjs"]) {
    test(`node --check ${f}`, () => { execFileSync(process.execPath, ["--check", path.join(ROOT, f)]); });
  }
});

describe("static quality", () => {
  const all = { "index.html": html, "style.css": css, "app.js": js };

  test("no external fonts or CDNs", () => {
    for (const [name, src] of Object.entries(all)) {
      assert.doesNotMatch(src, /fonts\.(googleapis|gstatic)\.com/, name);
      assert.doesNotMatch(src, /(src|href)=["']https?:\/\//, name);
      assert.doesNotMatch(src, /@import\s+url\(\s*["']?https?:/, name);
    }
  });

  test("no prompt() / alert() / confirm()", () => {
    assert.doesNotMatch(js, /\b(prompt|alert|confirm)\s*\(/);
  });

  test("no emoji or pictographic glyphs", () => {
    const glyphs = /[\u{1F000}-\u{1FAFF}\u{2190}-\u{21FF}\u{2300}-\u{23FF}\u{25A0}-\u{27BF}\u{2B00}-\u{2BFF}\u{FE0F}]/u;
    for (const [name, src] of Object.entries(all)) assert.doesNotMatch(src, glyphs, name);
  });

  test("no-referrer meta and svg favicon", () => {
    assert.match(html, /<meta name="referrer" content="no-referrer">/);
    assert.match(html, /<link rel="icon" type="image\/svg\+xml" href="icon\.svg">/);
    assert.equal(read("remote/icon.svg"), read("assets/icon.svg"));
  });

  test("toggles are accessible switches", () => {
    const switches = html.match(/<button type="button"[^>]*role="switch"[^>]*aria-checked="(true|false)"/g) || [];
    assert.ok(switches.length >= 10, `only ${switches.length} switches`);
    assert.doesNotMatch(html, /class="sw"/, "legacy div switches remain");
  });

  test("bottom tab bar is mobile-only", () => {
    const desktop = css.slice(css.indexOf("@media (min-width: 768px)"));
    assert.ok(desktop.length < css.length, "missing desktop breakpoint");
    assert.match(desktop, /\.tabs\s*\{[^}]*position:\s*static/);
  });

  test("ranges get a --pct fill; settings are grouped in four sections", () => {
    assert.match(css, /var\(--pct\)/);
    assert.match(js, /setProperty\("--pct"/);
    assert.equal((html.match(/class="card group"/g) || []).length, 4);
  });

  test("preset names never reach innerHTML", () => {
    assert.doesNotMatch(js, /innerHTML/);
  });

  test("save dialog: Enter submits Save, Cancel is a plain button", () => {
    const dlg = html.slice(html.indexOf('<dialog id="saveDlg"'), html.indexOf("</dialog>", html.indexOf('<dialog id="saveDlg"')));
    assert.match(dlg, /<button type="button"[^>]*id="saveCancel"[^>]*>Cancel</);
    assert.match(dlg, /<button type="submit"[^>]*id="saveOk"[^>]*>Save</);
    assert.equal((dlg.match(/type="submit"/g) || []).length, 1, "Save must be the only submitter");
  });

  test("desktop keeps Now Playing docked beside the workspace", () => {
    const wide = css.slice(css.indexOf("@media (min-width: 960px)"));
    assert.ok(wide.length < css.length, "missing 960px breakpoint");
    assert.match(wide, /\.view\[data-view="now"\]\s*\{[^}]*display:\s*block !important[^}]*position:\s*sticky/);
    assert.match(wide, /\.tab\[data-tab="now"\]\s*\{\s*display:\s*none/);
    assert.match(js, /matchMedia\("\(min-width: 960px\)"\)/);
  });

  test("now playing: plate, author line, status pill, mini player", () => {
    for (const id of ["stageCanvas", "pname", "pauthor", "statusPill", "telePop", "mFps", "mCpu", "mMem", "mTemp",
      "mTempWrap", "conn", "connText", "mini", "miniName", "miniNext", "miniRandom", "qBright"]) {
      assert.match(html, new RegExp(`id="${id}"`), id);
    }
    assert.match(html, /<canvas id="stageCanvas" width="480" height="300"/);
    assert.doesNotMatch(css, /\.switch\[aria-checked="true"\]\s*\{[^}]*--ok/, "switches use the accent, not green");
  });

  test("swatches keep the display's tint values", () => {
    for (const c of ["#00ff00", "#0080ff", "#ff40c0", "#ffb000", "#00e5e5", "#ff3030", "#ffffff"]) assert.ok(js.includes(`["${c}"`), c);
  });

  test("slider bounds match the server clamp ranges", () => {
    const brightness = html.match(/<input type="range"[^>]*data-setting="brightness"[^>]*>/g) || [];
    assert.equal(brightness.length, 2);
    for (const b of brightness) assert.match(b, /min="0\.05" max="1"/);
    assert.match(html, /data-setting="fps" min="20" max="120"/);
  });
});

describe("mock API", () => {
  const TOKEN = "op1";
  let mock, base;
  const call = async (method, url, { token = TOKEN, headers = {}, body } = {}) => {
    const h = { ...headers };
    if (token) h["X-Dropkick-Token"] = token;
    return fetch(base + url, { method, headers: h, body });
  };
  const get = async (url) => (await call("GET", url)).json();
  const post = async (url, body) => {
    const r = await call("POST", url, { body });
    assert.equal(r.status, 200, `${url} -> ${r.status}`);
    return r.json();
  };
  const status = () => get("/api/status");

  before(async () => { mock = createMockServer({ token: TOKEN }); base = (await mock.listen(0)).url; });
  after(() => mock.close());

  test("auth: 401 without token, 200 via header or query", async () => {
    assert.equal((await call("GET", "/api/status", { token: "" })).status, 401);
    assert.equal((await call("GET", "/api/status", { token: "wrong" })).status, 401);
    assert.equal((await call("GET", "/api/status")).status, 200);
    assert.equal((await fetch(`${base}/api/status?token=${TOKEN}`)).status, 200);
  });

  test("static: /<token> and / serve the app, traversal is blocked", async () => {
    for (const p of [`/${TOKEN}`, "/"]) {
      const r = await fetch(base + p);
      assert.equal(r.status, 200);
      assert.match(await r.text(), /Dropkick/);
    }
    const icon = await fetch(base + "/icon.svg");
    assert.equal(icon.headers.get("content-type"), "image/svg+xml");
    assert.notEqual((await fetch(base + "/..%2fpackage.json")).status, 200);
    const slash = await fetch(`${base}/${TOKEN}/`, { redirect: "manual" });
    assert.equal(slash.status, 302);
    assert.equal(slash.headers.get("location"), `/${TOKEN}`);
  });

  test("presets: realistic library with ETag / 304", async () => {
    const r = await call("GET", "/api/presets");
    const etag = r.headers.get("etag");
    const list = await r.json();
    assert.ok(etag);
    assert.ok(list.length >= 600);
    assert.ok(list.some((x) => x.p.endsWith(XSS_NAME)));
    assert.ok(list.some((x) => x.p.split("/").length - PRESET_ROOT.split("/").length >= 4), "nested subcategory");
    assert.equal((await call("GET", "/api/presets", { headers: { "If-None-Match": etag } })).status, 304);
  });

  test("transport: next / random / preset / history-based prev", async () => {
    await post("/api/preset?index=10");
    await post("/api/next");
    assert.equal((await status()).position, 11);
    await post("/api/preset?index=300");
    await post("/api/prev");
    assert.equal((await status()).position, 11);
    await post("/api/prev");
    assert.equal((await status()).position, 10);
    assert.equal((await call("POST", "/api/preset?index=abc")).status, 400);
    const s0 = await status();
    await post("/api/shuffle");
    await post("/api/lock");
    const s1 = await status();
    assert.equal(s1.shuffle, !s0.shuffle);
    assert.equal(s1.locked, !s0.locked);
  });

  test("favorites toggle, favorites shuffle vs category shuffle are exclusive", async () => {
    const s = await status();
    const fav = await post("/api/favorites/toggle?path=" + encodeURIComponent(s.preset));
    assert.equal((await status()).favorited, fav.favorited);
    assert.equal((await get("/api/favorites")).includes(s.preset), fav.favorited);

    assert.equal((await post("/api/favorites/shuffle")).favoritesShuffle, true);
    const dir = `${PRESET_ROOT}/${PACKS[0]}/Liquid`;
    assert.equal((await post("/api/category/shuffle?path=" + encodeURIComponent(dir))).categoryShuffle, true);
    let st = await status();
    assert.equal(st.favoritesShuffle, false);
    assert.equal(st.categoryShuffle, dir + "/");
    assert.ok(st.preset.startsWith(dir + "/"));
    await post("/api/random");
    assert.ok((await status()).preset.startsWith(dir + "/"));

    await post("/api/favorites/shuffle");
    st = await status();
    assert.equal(st.favoritesShuffle, true);
    assert.equal(st.categoryShuffle, "");
    await post("/api/favorites/shuffle");
    assert.equal((await call("POST", "/api/category/shuffle?path=/etc")).status, 400);
  });

  test("settings: update, coerce, reject unknown keys", async () => {
    await post("/api/settings?key=brightness&value=0.45");
    await post("/api/settings?key=reduceFlashing&value=true");
    await post("/api/settings?key=tintColor&value=ff40c0");
    const s = await get("/api/settings");
    assert.equal(s.brightness, 0.45);
    assert.equal(s.reduceFlashing, true);
    assert.equal(s.tintColor, "ff40c0");
    assert.equal((await call("POST", "/api/settings?key=evil&value=1")).status, 400);
  });

  test("settings: C++ parsing, clamping and validation", async () => {
    await post("/api/settings?key=reduceFlashing&value=false");
    await post("/api/settings?key=tintEnabled&value=1");
    await post("/api/settings?key=brightness&value=2");
    await post("/api/settings?key=fps&value=5");
    await post("/api/settings?key=tintColor&value=%2300ff00");
    const s = await get("/api/settings");
    assert.equal(s.reduceFlashing, false);
    assert.equal(s.tintEnabled, true);
    assert.equal(s.brightness, 1);
    assert.equal(s.fps, 10);
    assert.equal(s.tintColor, "#00ff00");
    for (const bad of ["key=fps&value=junk", "key=fps&value=NaN", "key=brightness&value=", "key=hardCut&value=yes",
      "key=tintColor&value=red", "key=tintColor&value=%23ff00f", "key=tintColor&value=gg0000"]) {
      assert.equal((await call("POST", "/api/settings?" + bad)).status, 400, bad);
    }
  });

  test("audio: device list with system default, select by id, next", async () => {
    const { devices, current } = await get("/api/audio/devices");
    assert.ok(devices.length >= 2);
    assert.ok(devices.some((d) => d.id === -1));
    let st = await status();
    assert.equal(st.audioId, current);
    assert.equal(st.audio, devices.find((d) => d.id === current).name);
    await post("/api/audio/select?index=1");
    st = await status();
    assert.equal(st.audioId, 1);
    assert.equal(st.audio, devices.find((d) => d.id === 1).name);
    await post("/api/audio/select?index=-1");
    assert.equal((await status()).audioId, -1);
    await post("/api/audio/next");
    assert.equal((await status()).audioId, devices[(devices.findIndex((d) => d.id === -1) + 1) % devices.length].id);
    assert.equal((await call("POST", "/api/audio/select?index=99")).status, 400);
  });

  test("workshop: source, apply goes live, save sanitizes name", async () => {
    const src = await get("/api/workshop/source");
    assert.match(src.text, /\[preset00\]/);
    await post("/api/workshop/apply", "[preset00]\nzoom=1.2\n");
    const s = await status();
    assert.equal(s.workshop, true);
    assert.match(s.preset, /_scratch\.milk$/);
    assert.equal((await get("/api/workshop/source")).text, "[preset00]\nzoom=1.2\n");
    assert.equal((await post("/api/workshop/save?name=" + encodeURIComponent("../../evil"), "x")).file, "evil.milk");
    assert.equal((await call("POST", "/api/workshop/save?name=..")).status, 400);
    assert.equal((await call("POST", "/api/workshop/save?name=a%3Ab.milk", { body: "x" })).status, 400);

    // Overwrite confirmation.
    assert.equal((await call("POST", "/api/workshop/save?name=keep&overwrite=0", { body: "v1" })).status, 200);
    const again = await call("POST", "/api/workshop/save?name=keep.milk&overwrite=0", { body: "v2" });
    assert.equal(again.status, 409);
    assert.deepEqual(await again.json(), { error: "exists" });
    assert.equal((await call("POST", "/api/workshop/save?name=keep.milk&overwrite=1", { body: "v2" })).status, 200);

    // Dislike while a Workshop scratch is live must not touch the library.
    const before = await status();
    await post("/api/dislike");
    const afterS = await status();
    assert.equal(afterS.size, before.size);
    assert.equal(afterS.disliked, before.disliked);
    assert.equal(afterS.catalogRevision, before.catalogRevision);

    await post("/api/next");
    assert.equal((await status()).workshop, false);
  });

  test("workshop: bodies over 1 MiB get a 413, not a reset", async () => {
    const big = "x".repeat(1024 * 1024 + 1);
    for (const url of ["/api/workshop/apply", "/api/workshop/save?name=big"]) {
      const r = await call("POST", url, { body: big });
      assert.equal(r.status, 413, url);
      assert.deepEqual(await r.json(), { error: "payload too large" });
    }
  });

  test("dislike shrinks the playlist and bumps the ETag; clear resets", async () => {
    const r0 = await call("GET", "/api/presets");
    const etag0 = r0.headers.get("etag");
    const before = await status();
    assert.equal(before.workshop, false);
    await post("/api/dislike");
    const afterS = await status();
    assert.equal(afterS.size, before.size - 1);
    assert.equal(afterS.disliked, before.disliked + 1);
    assert.ok(afterS.catalogRevision > before.catalogRevision);
    const r1 = await call("GET", "/api/presets", { headers: { "If-None-Match": etag0 } });
    assert.equal(r1.status, 200);
    assert.notEqual(r1.headers.get("etag"), etag0);
    await post("/api/dislikes/clear");
    await post("/api/blocklist/clear");
    const st = await status();
    assert.equal(st.disliked, 0);
    assert.equal(st.blocked, 0);
  });

  test("pack switch reloads the library", async () => {
    assert.deepEqual(await get("/api/packs"), PACKS);
    const rev = (await status()).catalogRevision;
    await post("/api/pack?name=" + PACKS[1]);
    assert.ok((await status()).catalogRevision > rev);
    const list = await get("/api/presets");
    assert.ok(list.every((x) => x.p.startsWith(`${PRESET_ROOT}/${PACKS[1]}/`)));
    assert.equal((await status()).categoryShuffle, "");
    assert.equal((await call("POST", "/api/pack?name=../x")).status, 400);
  });

  test("live preview: streams 240x150 24-bit BMP, respects auth and livePreview setting", async () => {
    assert.equal((await call("GET", "/api/preview", { token: "" })).status, 401);
    const r = await call("GET", "/api/preview");
    assert.equal(r.status, 200);
    assert.equal(r.headers.get("content-type"), "image/bmp");
    assert.ok(Number(r.headers.get("x-dropkick-frame")) >= 1);
    const buf = Buffer.from(await r.arrayBuffer());
    assert.equal(buf.toString("ascii", 0, 2), "BM");
    assert.equal(buf.readInt32LE(18), 240);
    assert.equal(buf.readInt32LE(22), 150);
    assert.equal(buf.readUInt16LE(28), 24);
    assert.equal(buf.length, 54 + 240 * 150 * 3);

    // Brightness change affects rendered pixels.
    await post("/api/settings?key=tintEnabled&value=false");
    await post("/api/settings?key=brightness&value=0.05");
    const dim = Buffer.from(await (await call("GET", "/api/preview")).arrayBuffer());
    await post("/api/settings?key=brightness&value=1");
    const bright = Buffer.from(await (await call("GET", "/api/preview")).arrayBuffer());
    const avg = (b) => { let s = 0; for (let i = 54; i < b.length; i += 33) s += b[i]; return s / ((b.length - 54) / 33); };
    assert.ok(avg(bright) > avg(dim) * 3, "brightness should scale preview output");

    // Disabling livePreview pauses streaming cleanly with 204.
    await post("/api/settings?key=livePreview&value=false");
    const off = await call("GET", "/api/preview");
    assert.equal(off.status, 204);
    assert.equal(off.headers.get("x-dropkick-preview"), "disabled");
    await post("/api/settings?key=livePreview&value=true");
    assert.equal((await call("GET", "/api/preview")).status, 200);
  });
});

test("open mode (no token) needs no auth; hasTemp=false reports temp 0", async () => {
  const mock = createMockServer({ hasTemp: false });
  const { url } = await mock.listen(0);
  try {
    const s = await (await fetch(url + "/api/status")).json();
    assert.equal(s.hasTemp, false);
    assert.equal(s.temp, 0);
  } finally { await mock.close(); }
});
