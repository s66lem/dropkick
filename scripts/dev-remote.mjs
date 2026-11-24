#!/usr/bin/env node
// Zero-dependency mock of Dropkick's remote-control HTTP API (see
// external/frontend-sdl-cpp/src/RemoteControl.cpp) for developing remote/ on a laptop.
//
//   node scripts/dev-remote.mjs [--port 8080] [--host 127.0.0.1] [--token op1] [--no-temp]
//
// Tests import createMockServer() and listen on port 0.
import http from "node:http";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const WEB_ROOT = path.resolve(HERE, "..", "remote");
export const PRESET_ROOT = "/home/pi/.local/share/dropkick/presets";
const WORKSHOP_DIR = "/home/pi/.local/share/dropkick/workshop";
export const PACKS = ["cream-of-the-crop", "classic-milkdrop", "workshop-picks"];
export const XSS_NAME = `Shady <script>alert("x")<script> & "Quotes" 'n <b>Tags.milk`;

// Mirrors PresetLists::ClampSettingValue ranges and ParseSetting() in RemoteControl.cpp.
const RANGES = {
  presetDuration: [1, 3600], softCutDuration: [0, 30], hardCut: [0, 1], hardCutDuration: [0, 300],
  hardCutSensitivity: [0, 10], beatSensitivity: [0, 5], fps: [10, 240], aspectCorrection: [0, 1],
  reduceFlashing: [0, 1], flashStrength: [0, 1], brightness: [0.05, 1], tintEnabled: [0, 1],
  tintStrength: [0, 1], livePreview: [0, 1], autoskipEnabled: [0, 1], autoskipFps: [1, 120],
  autoskipStrikes: [0, 50],
};
const BOOL_KEYS = new Set(["hardCut", "aspectCorrection", "reduceFlashing", "tintEnabled", "livePreview", "autoskipEnabled"]);

/** The parsed value, or undefined where the C++ server answers 400. */
function parseSetting(key, value) {
  if (!value) return undefined;
  if (key === "tintColor") return /^#?[0-9a-f]{6}$/i.test(value) ? value : undefined;
  const range = RANGES[key];
  if (!range) return undefined;
  const bool = BOOL_KEYS.has(key);
  if (bool && (value === "true" || value === "false")) return value === "true";
  const n = Number(value);
  if (value.trim() !== value || !Number.isFinite(n)) return undefined;
  return bool ? n !== 0 : Math.min(range[1], Math.max(range[0], n));
}

const CATEGORIES = {
  Abstract: ["", "Organic", "Smoke"],
  Fractal: ["", "Mandelbrot", "Julia/Deep"],
  Geometric: ["", "Tunnels", "Lattice"],
  Liquid: ["", "Plasma"],
  Reaction: ["", "Bass", "Kick/Hard"],
  Waveform: ["", "Scope", "Spectrum"],
};
const AUTHORS = ["Geiss", "Rovastar", "Krash", "Flexi", "Martin", "Eo.S.", "Aderrasi", "Unchained", "Zylot", "Idiot24"];
const WORDS = ["Neon", "Drift", "Pulse", "Velvet", "Static", "Aurora", "Echo", "Prism", "Quartz", "Ember",
  "Signal", "Vortex", "Halo", "Ripple", "Cinder", "Orbit", "Glass", "Bloom", "Shiver", "Cascade"];

// Deterministic PRNG so every run (and every test) sees the same library.
function rng(seed) {
  let a = [...seed].reduce((h, c) => Math.imul(h ^ c.charCodeAt(0), 2654435761) >>> 0, 7);
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

export function buildLibrary(pack, perCategory = 100) {
  const rand = rng(pack);
  const pick = (arr) => arr[Math.floor(rand() * arr.length)];
  const out = [];
  for (const [cat, subs] of Object.entries(CATEGORIES)) {
    for (let n = 0; n < perCategory; n++) {
      const sub = subs[n % subs.length];
      const dir = [PRESET_ROOT, pack, cat, sub].filter(Boolean).join("/");
      const w1 = pick(WORDS);
      const w2 = WORDS[(WORDS.indexOf(w1) + 1 + Math.floor(rand() * (WORDS.length - 1))) % WORDS.length];
      out.push(`${dir}/${pick(AUTHORS)} - ${w1} ${w2} ${String(n).padStart(3, "0")}.milk`);
    }
  }
  out.push(`${PRESET_ROOT}/${pack}/Fractal/${XSS_NAME}`);
  return out.sort();
}

const milkSource = (p) => `[preset00]
; ${path.posix.basename(p)}
fRating=3.000000
fGammaAdj=2.000000
fDecay=0.980000
fVideoEchoZoom=1.006
nWaveMode=6
zoom=1.010000
rot=0.000000
per_frame_1=wave_r = 0.5 + 0.5*sin(time*1.13);
per_frame_2=wave_g = 0.5 + 0.5*sin(time*1.23);
per_frame_3=zoom = zoom + 0.023*bass;
per_frame_4=rot = rot + 0.04*sin(time*0.6);
per_pixel_1=zoom = zoom + 0.03*rad*mid;
`;

function createState({ hasTemp = true } = {}) {
  const s = {
    pack: PACKS[0], items: [], version: 1, position: 0, history: [],
    shuffle: true, locked: false, favorites: new Set(), favShuffle: false, catPrefix: "",
    workshop: false, workshopPath: "", workshopFiles: new Map(),
    blocked: 3, disliked: 0, hasTemp,
    // id -1 is the system default, as in the real device list.
    audioDevices: [{ id: -1, name: "System Default" }, { id: 0, name: "Default (PulseAudio monitor)" },
      { id: 1, name: "USB Audio CODEC" }, { id: 2, name: "HDMI 1 loopback" }],
    audioId: -1,
    settings: {
      presetDuration: 30, softCutDuration: 3, hardCut: false, hardCutDuration: 20, hardCutSensitivity: 2,
      beatSensitivity: 1, fps: 60, aspectCorrection: true, reduceFlashing: false, flashStrength: 0.6,
      brightness: 1, tintEnabled: false, tintColor: "#00ff00", tintStrength: 1, livePreview: true,
      autoskipEnabled: true, autoskipFps: 20, autoskipStrikes: 3,
    },
    previewSeq: 0,
  };
  loadPack(s, s.pack);
  s.favorites.add(s.items[5]).add(s.items[120]).add(s.items.find((p) => p.endsWith(XSS_NAME)));
  return s;
}

function loadPack(s, pack) {
  s.pack = pack;
  s.items = buildLibrary(pack);
  s.version++;
  s.position = 0;
  s.history = [];
  s.catPrefix = "";
  s.workshop = false;
}

function moveTo(s, index) {
  if (index < 0 || index >= s.items.length) return;
  if (index !== s.position) s.history.push(s.position);
  if (s.history.length > 100) s.history.shift();
  s.position = index;
  s.workshop = false;
}

const randomIn = (arr) => arr[Math.floor(Math.random() * arr.length)];
const indicesWhere = (s, fn) => s.items.flatMap((p, i) => (fn(p) ? [i] : []));

function jump(s, inOrder) {
  if (s.favShuffle) {
    const fav = indicesWhere(s, (p) => s.favorites.has(p));
    if (!fav.length) return;
    moveTo(s, inOrder ? (fav.find((i) => i > s.position) ?? fav[0]) : randomIn(fav));
  } else if (s.catPrefix) {
    const cat = indicesWhere(s, (p) => p.startsWith(s.catPrefix));
    if (!cat.length) { s.catPrefix = ""; return; }
    moveTo(s, randomIn(cat));
  } else if (inOrder) {
    moveTo(s, (s.position + 1) % s.items.length);
  } else {
    moveTo(s, Math.floor(Math.random() * s.items.length));
  }
}

function status(s) {
  const current = s.items[s.position] || "";
  const preset = s.workshop ? s.workshopPath : current;
  const t = Date.now() / 1000;
  return {
    preset, position: s.position, size: s.items.length,
    shuffle: s.shuffle, locked: s.locked, favorited: s.favorites.has(preset),
    favoritesShuffle: s.favShuffle, categoryShuffle: s.catPrefix, workshop: s.workshop,
    blocked: s.blocked, disliked: s.disliked, catalogRevision: s.version,
    fps: Math.round(58 + 2 * Math.sin(t)), cpu: Math.round(32 + 9 * Math.sin(t / 3)),
    memUsed: 1480 + Math.round(40 * Math.sin(t / 5)), memTotal: 3794,
    temp: s.hasTemp ? Math.round(52 + 3 * Math.sin(t / 7)) : 0, hasTemp: s.hasTemp,
    audio: audioName(s), audioId: s.audioId,
  };
}

const PREVIEW_W = 240;
const PREVIEW_H = 150;

function hashStr(str) {
  let h = 2166136261;
  for (let i = 0; i < str.length; i++) h = Math.imul(h ^ str.charCodeAt(i), 16777619);
  return h >>> 0;
}

function parseHexRgb(hex) {
  const m = /^#?([0-9a-f]{6})$/i.exec(String(hex || ""));
  if (!m) return [0, 255, 0];
  const n = parseInt(m[1], 16);
  return [(n >> 16) & 255, (n >> 8) & 255, n & 255];
}

// Synthesizes a live animated 240x150 24-bit BMP matching PostProcess::CapturePreview()
// so the remote preview actually streams moving, setting-reactive frames in dev mode.
function renderMockPreviewBmp(s) {
  const w = PREVIEW_W, h = PREVIEW_H;
  const rowStride = (w * 3 + 3) & ~3;
  const imgSize = rowStride * h;
  const buf = Buffer.alloc(54 + imgSize);
  buf.write("BM", 0, 2, "ascii");
  buf.writeUInt32LE(54 + imgSize, 2);
  buf.writeUInt32LE(54, 10);
  buf.writeUInt32LE(40, 14);
  buf.writeInt32LE(w, 18);
  buf.writeInt32LE(h, 22);
  buf.writeUInt16LE(1, 26);
  buf.writeUInt16LE(24, 28);
  buf.writeUInt32LE(imgSize, 34);

  const preset = s.workshop ? s.workshopPath : (s.items[s.position] || "dropkick");
  const seed = hashStr(preset);
  const mode = seed % 3; // 0 = orbital tunnel, 1 = plasma ribbons, 2 = interference rings
  const t = (Date.now() / 1000) * (s.settings.reduceFlashing ? 0.55 : 1.0);
  const ph = (seed & 0xff) * 0.025;
  const f1 = 2.4 + ((seed >> 8) & 7) * 0.45;
  const f2 = 1.6 + ((seed >> 11) & 7) * 0.4;
  const bright = Math.max(0.05, Math.min(1, Number(s.settings.brightness ?? 1)));
  const tintOn = !!s.settings.tintEnabled;
  const tintStr = Math.max(0, Math.min(1, Number(s.settings.tintStrength ?? 1)));
  const [tr, tg, tb] = parseHexRgb(s.settings.tintColor);
  const pulse = 0.85 + 0.15 * Math.sin(t * 2.4 + ph);
  const halfW = w * 0.5, halfH = h * 0.5;

  for (let y = 0; y < h; y++) {
    const ny = (y - halfH) / halfH;
    const rowOff = 54 + y * rowStride;
    for (let x = 0; x < w; x++) {
      const nx = (x - halfW) / halfH; // aspect-corrected x
      let v, c2;
      if (mode === 0) {
        const rad = Math.hypot(nx, ny);
        const ang = Math.atan2(ny, nx);
        const ring = Math.sin(rad * (7.5 + f1) - t * 3.1 + Math.sin(ang * 3 + t * 0.9) * 0.9);
        const spokes = Math.cos(ang * (3 + (seed % 4)) + rad * 2.8 - t * 1.4);
        v = Math.max(0, (ring * 0.55 + spokes * 0.45) * 0.5 + 0.5) * Math.exp(-rad * 0.42);
        c2 = 0.5 + 0.5 * Math.sin(rad * 4.0 - t * 1.6 + ph);
      } else if (mode === 1) {
        const wv = Math.sin(nx * f1 + t * 1.5 + ph) + Math.cos(ny * f2 - t * 1.2) +
          Math.sin((nx + ny) * 2.2 + t * 1.1) + Math.cos(Math.hypot(nx * 1.2, ny) * 3.8 - t * 2.0);
        v = Math.max(0, Math.min(1, (wv * 0.25 + 0.5) * 1.15 - 0.12));
        c2 = 0.5 + 0.5 * Math.sin(wv * 1.1 + t * 0.7);
      } else {
        const cx1 = Math.sin(t * 0.7 + ph) * 0.45, cy1 = Math.cos(t * 0.55) * 0.32;
        const cx2 = -cx1 * 0.9, cy2 = -cy1 * 0.85;
        const d1 = Math.hypot(nx - cx1, ny - cy1);
        const d2 = Math.hypot(nx - cx2, ny - cy2);
        const wave = 0.5 + 0.5 * Math.sin(d1 * 12.0 - t * 3.4) * Math.cos(d2 * 10.0 - t * 2.6);
        v = wave * Math.exp(-Math.min(d1, d2) * 0.55);
        c2 = 0.5 + 0.5 * Math.sin((d1 - d2) * 5.0 + t);
      }

      const peak = Math.pow(Math.max(0, Math.min(1, v * pulse)), 1.35);
      // Warm copper/amber + cyan/emerald Milkdrop spectrum (no muddy grey)
      let r = (255 * Math.min(1, peak * (1.15 - 0.55 * c2) + (peak > 0.72 ? (peak - 0.72) * 1.4 : 0))) * bright;
      let g = (255 * Math.min(1, peak * (0.48 + 0.46 * c2) + (peak > 0.78 ? (peak - 0.78) * 1.1 : 0))) * bright;
      let b = (255 * Math.min(1, peak * (0.22 + 0.78 * c2))) * bright;

      if (tintOn) {
        const lum = (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255;
        r = r * (1 - tintStr) + lum * tr * tintStr;
        g = g * (1 - tintStr) + lum * tg * tintStr;
        b = b * (1 - tintStr) + lum * tb * tintStr;
      }

      const o = rowOff + x * 3;
      buf[o] = b < 0 ? 0 : b > 255 ? 255 : b | 0;
      buf[o + 1] = g < 0 ? 0 : g > 255 ? 255 : g | 0;
      buf[o + 2] = r < 0 ? 0 : r > 255 ? 255 : r | 0;
    }
  }
  return buf;
}

const audioName = (s) => s.audioDevices.find((d) => d.id === s.audioId)?.name || "";

const TYPES = { ".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8",
  ".css": "text/css; charset=utf-8", ".svg": "image/svg+xml", ".json": "application/json" };

function sendFile(res, file) {
  fs.readFile(file, (err, buf) => {
    if (err) { res.writeHead(404, { "Content-Type": "text/plain" }); res.end("not found"); return; }
    res.writeHead(200, { "Content-Type": TYPES[path.extname(file)] || "application/octet-stream", "Cache-Control": "no-cache" });
    res.end(buf);
  });
}

function readBody(req, limit = 1024 * 1024) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    let size = 0;
    // Drain an oversized body instead of destroying the socket so the client still gets the 413.
    req.on("data", (c) => { size += c.length; if (size <= limit) chunks.push(c); });
    req.on("end", () => {
      if (size > limit) reject(Object.assign(new Error("payload too large"), { status: 413 }));
      else resolve(Buffer.concat(chunks).toString("utf8"));
    });
    req.on("error", reject);
  });
}

export function createMockServer({ token = "", hasTemp = true, webRoot = WEB_ROOT } = {}) {
  const state = createState({ hasTemp });

  const server = http.createServer(async (req, res) => {
    const url = new URL(req.url, "http://localhost");
    const q = (k) => url.searchParams.get(k) ?? "";
    const json = (code, obj, headers = {}) => {
      res.writeHead(code, { "Content-Type": "application/json", "Cache-Control": "no-store", ...headers });
      res.end(JSON.stringify(obj));
    };
    const ok = (extra = {}) => json(200, { ok: true, ...extra });
    const bad = (error, code = 400) => json(code, { error });

    if (!url.pathname.startsWith("/api/")) {
      if (req.method !== "GET" && req.method !== "HEAD") return bad("method not allowed", 405);
      let pathname;
      try { pathname = decodeURIComponent(url.pathname); } catch { return bad("bad path"); }
      if (token && pathname === "/" + token) return sendFile(res, path.join(webRoot, "index.html"));
      if (token && pathname === "/" + token + "/") {
        res.writeHead(302, { Location: "/" + encodeURIComponent(token) });
        return res.end();
      }
      const rel = pathname === "/" ? "index.html" : pathname.slice(1);
      const file = path.resolve(webRoot, rel);
      if (!file.startsWith(webRoot + path.sep)) return bad("forbidden", 403);
      return sendFile(res, file);
    }

    const supplied = req.headers["x-dropkick-token"] || q("token");
    if (token && supplied !== token) return bad("unauthorized", 401);

    const s = state;
    const route = `${req.method} ${url.pathname}`;
    try {
      switch (route) {
        case "GET /api/status": return json(200, status(s));
        case "GET /api/settings": return json(200, s.settings);
        case "GET /api/packs": return json(200, PACKS);
        case "GET /api/favorites": return json(200, [...s.favorites]);
        case "GET /api/presets": {
          const etag = `W/"presets-${s.version}"`;
          if (req.headers["if-none-match"] === etag) { res.writeHead(304, { ETag: etag }); return res.end(); }
          return json(200, s.items.map((p, i) => ({ i, p })), { ETag: etag });
        }
        case "GET /api/audio/devices":
          return json(200, { current: s.audioId, devices: s.audioDevices });
        case "GET /api/preview": {
          if (!s.settings.livePreview) {
            res.writeHead(204, { "Cache-Control": "no-store", "X-Dropkick-Preview": "disabled" });
            return res.end();
          }
          const bmp = renderMockPreviewBmp(s);
          res.writeHead(200, {
            "Content-Type": "image/bmp",
            "Cache-Control": "no-store",
            "X-Dropkick-Frame": String(++s.previewSeq),
          });
          return res.end(bmp);
        }
        case "GET /api/workshop/source": {
          const p = s.workshop ? s.workshopPath : s.items[s.position] || "";
          return json(200, { path: p, text: s.workshopFiles.get(p) ?? (p ? milkSource(p) : "") });
        }

        case "POST /api/next": jump(s, true); return ok();
        case "POST /api/random": jump(s, false); return ok();
        case "POST /api/prev": {
          s.workshop = false;
          if (s.history.length) s.position = s.history.pop();
          else s.position = (s.position - 1 + s.items.length) % s.items.length;
          return ok();
        }
        case "POST /api/shuffle": s.shuffle = !s.shuffle; return ok();
        case "POST /api/lock": s.locked = !s.locked; return ok();
        case "POST /api/preset": {
          const idx = q("index");
          if (!/^\d+$/.test(idx)) return bad("invalid index");
          const i = Number(idx);
          if (s.catPrefix && s.items[i] && !s.items[i].startsWith(s.catPrefix)) s.catPrefix = "";
          moveTo(s, i);
          return ok();
        }
        case "POST /api/favorites/toggle": {
          const p = q("path");
          if (!p) return bad("missing path");
          const on = !s.favorites.delete(p);
          if (on) s.favorites.add(p);
          return ok({ favorited: on });
        }
        case "POST /api/favorites/shuffle":
          s.favShuffle = !s.favShuffle;
          if (s.favShuffle) s.catPrefix = "";
          return ok({ favoritesShuffle: s.favShuffle });
        case "POST /api/category/shuffle": {
          let p = q("path");
          if (!p || p.includes("..") || !p.startsWith(PRESET_ROOT)) return bad("invalid category path");
          if (!p.endsWith("/")) p += "/";
          const on = s.catPrefix !== p;
          s.catPrefix = on ? p : "";
          if (on) { s.favShuffle = false; jump(s, false); }
          return ok({ categoryShuffle: on });
        }
        case "POST /api/settings": {
          const key = q("key"), value = q("value");
          const v = parseSetting(key, value);
          if (v === undefined) return bad("invalid setting");
          s.settings[key] = v;
          return ok();
        }
        case "POST /api/pack": {
          const name = q("name");
          if (!name || name.includes("..") || name.includes("/")) return bad("invalid pack name");
          if (!PACKS.includes(name)) return bad("unknown pack", 404);
          loadPack(s, name);
          return ok();
        }
        case "POST /api/audio/next": {
          const i = s.audioDevices.findIndex((d) => d.id === s.audioId);
          s.audioId = s.audioDevices[(i + 1) % s.audioDevices.length].id;
          return ok();
        }
        case "POST /api/audio/select": {
          const idx = q("index");
          if (!/^-?\d+$/.test(idx) || !s.audioDevices.some((d) => d.id === Number(idx))) return bad("invalid index");
          s.audioId = Number(idx);
          return ok({ audio: audioName(s) });
        }
        case "POST /api/workshop/capture": {
          const p = s.items[s.position];
          s.workshopPath = `${WORKSHOP_DIR}/${path.posix.basename(p)}`;
          s.workshopFiles.set(s.workshopPath, milkSource(p));
          s.workshop = true;
          return ok();
        }
        case "POST /api/workshop/apply": {
          s.workshopPath = `${WORKSHOP_DIR}/_scratch.milk`;
          s.workshopFiles.set(s.workshopPath, await readBody(req));
          s.workshop = true;
          return ok();
        }
        case "POST /api/workshop/save": {
          let name = q("name").split(/[/\\]/).pop();
          if (!name || name.includes("..") || name.includes(":")) return bad("invalid name");
          if (!name.endsWith(".milk")) name += ".milk";
          const dest = `${WORKSHOP_DIR}/${name}`;
          const body = await readBody(req);
          if (q("overwrite") === "0" && s.workshopFiles.has(dest)) return bad("exists", 409);
          s.workshopFiles.set(dest, body);
          return ok({ file: name });
        }
        case "POST /api/dislike": {
          // The real app quarantines the file, shrinking the playlist.
          if (s.workshop || !s.items.length) return ok();
          s.items.splice(s.position, 1);
          s.history = [];
          s.position = Math.min(s.position, s.items.length - 1);
          s.disliked++;
          s.version++;
          return ok();
        }
        case "POST /api/dislikes/clear": s.disliked = 0; return ok();
        case "POST /api/blocklist/clear": s.blocked = 0; return ok();
        default: return bad("not found", 404);
      }
    } catch (err) {
      return bad(err.message, err.status || 500);
    }
  });

  return {
    server,
    state,
    listen(port = 0, host = "127.0.0.1") {
      return new Promise((resolve) => server.listen(port, host, () => {
        const a = server.address();
        resolve({ port: a.port, url: `http://${host}:${a.port}` });
      }));
    },
    close: () => new Promise((resolve) => { server.closeAllConnections?.(); server.close(() => resolve()); }),
  };
}

if (process.argv[1] && import.meta.url === pathToFileURL(path.resolve(process.argv[1])).href) {
  const argv = process.argv.slice(2);
  const arg = (name, fallback) => { const i = argv.indexOf(name); return i >= 0 && argv[i + 1] ? argv[i + 1] : fallback; };
  const port = Number(arg("--port", 8080));
  const host = arg("--host", "127.0.0.1");
  const token = arg("--token", process.env.DROPKICK_DEV_TOKEN || "");
  const mock = createMockServer({ token, hasTemp: !argv.includes("--no-temp") });
  const { url } = await mock.listen(port, host);
  console.log(`Dropkick mock remote on ${url}${token ? `/${encodeURIComponent(token)}  (token required)` : "  (open, no token)"}`);
}
