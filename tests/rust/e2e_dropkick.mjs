// End-to-end integration test against the real compiled Rust `dropkick` binary (Lactea + wgpu).
// Usage: node tests/rust/e2e_dropkick.mjs [--presets <dir>] [--bin <exe>]
import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";

const args = process.argv.slice(2);
const arg = (flag, def) => {
  const i = args.indexOf(flag);
  return i >= 0 && args[i + 1] ? args[i + 1] : def;
};

const BIN = arg("--bin", path.resolve("target/release/dropkick" + (process.platform === "win32" ? ".exe" : "")));
const PRESETS = arg(
  "--presets",
  fs.existsSync("C:/Users/s6lem/Downloads/presets-cream-of-the-crop-master/presets-cream-of-the-crop-master")
    ? "C:/Users/s6lem/Downloads/presets-cream-of-the-crop-master/presets-cream-of-the-crop-master"
    : path.resolve("external/projectm/presets")
);
const PORT = 18089;
const TOKEN = "e2e77";
const BASE = `http://127.0.0.1:${PORT}`;
const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), "dropkick-rust-e2e-"));

const call = (method, p, { token = TOKEN, headers = {}, body } = {}) => {
  const h = { ...headers };
  if (token) h["X-Dropkick-Token"] = token;
  return fetch(BASE + p, { method, headers: h, body });
};
const getJson = async (p) => {
  const r = await call("GET", p);
  assert.equal(r.status, 200, `GET ${p} -> ${r.status}`);
  return r.json();
};
const postJson = async (p, body) => {
  const r = await call("POST", p, { body });
  assert.equal(r.status, 200, `POST ${p} -> ${r.status}`);
  return r.json();
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const proc = spawn(
  BIN,
  [
    "--headless",
    "--host", "127.0.0.1",
    "--port", String(PORT),
    "--token", TOKEN,
    "--presets", PRESETS,
    "--preset-root", path.dirname(PRESETS),
    "--data-dir", tmpDir,
    "--web-root", path.resolve("remote"),
    "--width", "640",
    "--height", "360",
  ],
  { stdio: ["ignore", "pipe", "pipe"] }
);

let logs = "";
proc.stdout.on("data", (d) => (logs += d.toString()));
proc.stderr.on("data", (d) => (logs += d.toString()));

async function waitReady() {
  for (let i = 0; i < 100; i++) {
    try {
      const r = await fetch(`${BASE}/api/status?token=${TOKEN}`);
      if (r.ok) return;
    } catch {}
    await sleep(100);
  }
  throw new Error("dropkick failed to become ready:\n" + logs);
}

async function pollPreviewBmp(timeoutMs = 5000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    const r = await call("GET", "/api/preview");
    if (r.status === 200) {
      const buf = Buffer.from(await r.arrayBuffer());
      return { buf, frame: r.headers.get("x-dropkick-frame"), ct: r.headers.get("content-type") };
    }
    await sleep(120);
  }
  throw new Error("timed out waiting for 200 /api/preview");
}

try {
  await waitReady();
  let checks = 0;
  const ok = (name) => { checks++; console.log(`  PASS [${checks}] ${name}`); };

  // 1. Auth & Static UI
  assert.equal((await call("GET", "/api/status", { token: "" })).status, 401);
  assert.equal((await call("GET", "/api/status", { token: "wrong" })).status, 401);
  assert.equal((await fetch(`${BASE}/api/status?token=${TOKEN}`)).status, 200);
  const idx = await fetch(`${BASE}/${TOKEN}`);
  assert.equal(idx.status, 200);
  assert.match(await idx.text(), /Dropkick/);
  const redir = await fetch(`${BASE}/${TOKEN}/`, { redirect: "manual" });
  assert.equal(redir.status, 302);
  assert.notEqual((await fetch(`${BASE}/..%2fCargo.toml`)).status, 200);
  ok("auth (401/200/query) + static entry & path traversal guard");

  // 2. Status + Presets + ETag 304
  const st0 = await getJson("/api/status");
  assert.ok(st0.size >= 1, `expected presets, got ${st0.size}`);
  assert.ok(st0.preset.endsWith(".milk") || st0.preset.endsWith(".milk2"));
  const pr1 = await call("GET", "/api/presets");
  assert.equal(pr1.status, 200);
  const etag = pr1.headers.get("etag");
  assert.ok(etag, "missing ETag");
  const list = await pr1.json();
  assert.equal(list.length, st0.size);
  const pr304 = await call("GET", "/api/presets", { headers: { "If-None-Match": etag } });
  assert.equal(pr304.status, 304);
  ok(`status + presets (${list.length} presets) + ETag/304`);

  // 3. Real GPU /api/preview BMP + disable/enable + tint post-process verification
  const p1 = await pollPreviewBmp();
  assert.equal(p1.ct, "image/bmp");
  assert.equal(p1.buf.toString("ascii", 0, 2), "BM");
  assert.equal(p1.buf.readInt32LE(18), 240);
  assert.equal(p1.buf.readInt32LE(22), 150);
  assert.equal(p1.buf.readUInt16LE(28), 24);
  assert.equal(p1.buf.length, 54 + 240 * 150 * 3);
  ok(`real GPU /api/preview 240x150 24-bit BMP (frame=${p1.frame}, ${p1.buf.length} bytes)`);

  await postJson("/api/settings?key=livePreview&value=false");
  const dis = await call("GET", "/api/preview");
  assert.equal(dis.status, 204);
  assert.equal(dis.headers.get("x-dropkick-preview"), "disabled");
  await postJson("/api/settings?key=livePreview&value=true");
  const p2 = await pollPreviewBmp();
  assert.equal(p2.buf.length, 54 + 240 * 150 * 3);
  ok("livePreview disable (204 disabled) -> re-enable -> fresh 200 BMP");

  // Verify post-process (brightness + reduceFlashing + tint)
  await postJson("/api/settings?key=reduceFlashing&value=true");
  await postJson("/api/settings?key=tintEnabled&value=true");
  await postJson("/api/settings?key=tintColor&value=00ff00");
  await postJson("/api/settings?key=brightness&value=0.85");
  await sleep(300);
  const pTint = await pollPreviewBmp();
  assert.equal(pTint.buf.length, 54 + 240 * 150 * 3);
  await postJson("/api/settings?key=tintEnabled&value=false");
  await postJson("/api/settings?key=reduceFlashing&value=false");
  await postJson("/api/settings?key=brightness&value=1");
  ok("post-processing WGSL pipeline (reduceFlashing + brightness + tint) live");

  // 4. Transport (next / random / preset / history-based prev / lock / shuffle)
  const firstPos = st0.position;
  await postJson("/api/next");
  await sleep(250);
  const stNext = await getJson("/api/status");
  assert.notEqual(stNext.position, firstPos);
  await postJson("/api/prev");
  await sleep(250);
  const stPrev = await getJson("/api/status");
  assert.equal(stPrev.position, firstPos);
  await postJson("/api/lock");
  await sleep(150);
  assert.equal((await getJson("/api/status")).locked, !st0.locked);
  await postJson("/api/lock");
  ok("transport (next -> history prev -> lock toggle)");

  // 5. Favorites + Settings clamping + Audio devices
  const favRes = await postJson(`/api/favorites/toggle?path=${encodeURIComponent(st0.preset)}`);
  assert.equal(favRes.favorited, true);
  assert.ok((await getJson("/api/favorites")).includes(st0.preset));
  assert.equal((await call("POST", "/api/settings?key=unknownKey&value=1")).status, 400);
  assert.equal((await call("POST", "/api/settings?key=fps&value=NaN")).status, 400);
  await postJson("/api/settings?key=fps&value=999");
  assert.equal((await getJson("/api/settings")).fps, 240);
  await postJson("/api/settings?key=fps&value=60");
  const aud = await getJson("/api/audio/devices");
  assert.ok(Array.isArray(aud.devices) && aud.devices.length >= 1);
  assert.equal((await call("POST", "/api/audio/select?index=99999")).status, 400);
  ok(`favorites + settings clamping + audio devices (${aud.devices.length} devices)`);

  // 6. Workshop apply + save + 409 overwrite + 413 >1MiB cap
  const src = await getJson("/api/workshop/source");
  assert.ok(src.text.length > 0);
  const scratchMilk = "[preset00]\nfDecay=0.98\nper_frame_1=wave_r=0.9;\n";
  await postJson("/api/workshop/apply", scratchMilk);
  await sleep(250);
  assert.equal((await getJson("/api/status")).workshop, true);
  const saved = await postJson("/api/workshop/save?name=rust-e2e&overwrite=0", scratchMilk);
  assert.equal(saved.file, "rust-e2e.milk");
  assert.equal((await call("POST", "/api/workshop/save?name=rust-e2e&overwrite=0", { body: scratchMilk })).status, 409);
  assert.equal((await call("POST", "/api/workshop/apply", { body: "x".repeat(1024 * 1024 + 32) })).status, 413);
  ok("workshop apply (live) + save + 409 conflict + 413 >1 MiB cap");

  // 7. Dislike shrinks catalog & bumps ETag, clear restores
  await postJson("/api/next");
  await sleep(200);
  const beforeDis = await getJson("/api/status");
  await postJson("/api/dislike");
  await sleep(250);
  const afterDis = await getJson("/api/status");
  assert.equal(afterDis.size, beforeDis.size - 1);
  assert.notEqual(afterDis.catalogRevision, beforeDis.catalogRevision);
  await postJson("/api/dislikes/clear");
  await sleep(250);
  assert.equal((await getJson("/api/status")).size, beforeDis.size);
  ok("dislike shrinks playlist & bumps catalogRevision + clear restores");

  console.log(`\nALL ${checks}/${checks} RUST E2E CHECKS PASSED!`);
} finally {
  proc.kill();
  await sleep(300);
  fs.rmSync(tmpDir, { recursive: true, force: true });
}
