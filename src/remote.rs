//! Embedded axum HTTP server implementing Dropkick's complete remote-control REST API,
//! live `/api/preview` BMP stream, constant-time token auth, 1 MiB body limit, and
//! embedded/disk static remote web app serving.

use crate::state::{Command, SharedState, normalize_path, write_file_atomic};
use axum::Router;
use axum::body::Body;
use axum::extract::{DefaultBodyLimit, Query, State};
use axum::http::{HeaderMap, HeaderValue, StatusCode, Uri, header};
use axum::response::{IntoResponse, Response};
use axum::routing::{get, post};
use serde::Serialize;
use serde_json::json;
use std::collections::HashMap;
use std::fs;
use std::path::PathBuf;
use std::sync::Arc;
use std::time::Instant;

const MAX_BODY_BYTES: usize = 1024 * 1024; // 1 MiB

// Embedded fallback so a standalone `dropkick` binary always serves the web remote even
// without a separate `remote/` directory next to it.
const EMBED_INDEX: &str = include_str!("../remote/index.html");
const EMBED_STYLE: &str = include_str!("../remote/style.css");
const EMBED_APP: &str = include_str!("../remote/app.js");
const EMBED_ICON: &str = include_str!("../remote/icon.svg");

pub fn build_router(state: Arc<SharedState>) -> Router {
    let api = Router::new()
        .route("/status", get(api_status))
        .route("/presets", get(api_presets))
        .route("/packs", get(api_packs))
        .route("/settings", get(api_get_settings).post(api_post_settings))
        .route("/favorites", get(api_favorites))
        .route("/audio/devices", get(api_audio_devices))
        .route("/workshop/source", get(api_workshop_source))
        .route("/preview", get(api_preview))
        .route("/next", post(api_next))
        .route("/prev", post(api_prev))
        .route("/random", post(api_random))
        .route("/shuffle", post(api_shuffle))
        .route("/lock", post(api_lock))
        .route("/preset", post(api_preset))
        .route("/pack", post(api_pack))
        .route("/favorites/toggle", post(api_favorites_toggle))
        .route("/favorites/shuffle", post(api_favorites_shuffle))
        .route("/category/shuffle", post(api_category_shuffle))
        .route("/dislike", post(api_dislike))
        .route("/dislikes/clear", post(api_dislikes_clear))
        .route("/blocklist/clear", post(api_blocklist_clear))
        .route("/audio/select", post(api_audio_select))
        .route("/audio/next", post(api_audio_next))
        .route("/workshop/capture", post(api_workshop_capture))
        .route("/workshop/apply", post(api_workshop_apply))
        .route("/workshop/save", post(api_workshop_save))
        .layer(DefaultBodyLimit::max(MAX_BODY_BYTES));

    Router::new()
        .nest("/api", api)
        .fallback(get(serve_static))
        .with_state(state)
}

pub fn spawn_http_server(
    state: Arc<SharedState>,
    host: String,
    port: u16,
) -> std::io::Result<(std::net::SocketAddr, std::thread::JoinHandle<()>)> {
    let addr: std::net::SocketAddr = format!("{host}:{port}")
        .parse()
        .map_err(|e| std::io::Error::new(std::io::ErrorKind::InvalidInput, e))?;
    let std_listener = std::net::TcpListener::bind(addr)?;
    std_listener.set_nonblocking(true)?;
    let local_addr = std_listener.local_addr()?;

    let handle = std::thread::Builder::new()
        .name("dropkick-http".to_string())
        .spawn(move || {
            let rt = tokio::runtime::Builder::new_current_thread()
                .enable_all()
                .build()
                .expect("Failed to create single-thread tokio runtime");
            rt.block_on(async move {
                let listener = tokio::net::TcpListener::from_std(std_listener)
                    .expect("Failed to adopt TcpListener");
                let app = build_router(state);
                if let Err(e) = axum::serve(listener, app).await {
                    log::error!("HTTP server error: {e}");
                }
            });
        })?;
    Ok((local_addr, handle))
}

fn ct_eq(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() {
        return false;
    }
    let mut diff = 0u8;
    for (&x, &y) in a.iter().zip(b.iter()) {
        diff |= x ^ y;
    }
    diff == 0
}

fn check_auth(state: &SharedState, headers: &HeaderMap, query: &HashMap<String, String>) -> Result<(), Response> {
    if state.remote_token.is_empty() {
        return Ok(());
    }
    let supplied = headers
        .get("x-dropkick-token")
        .and_then(|v| v.to_str().ok())
        .or_else(|| query.get("token").map(String::as_str))
        .unwrap_or("");
    if ct_eq(supplied.as_bytes(), state.remote_token.as_bytes()) {
        Ok(())
    } else {
        Err(err_json(StatusCode::UNAUTHORIZED, "unauthorized"))
    }
}

fn ok_json<T: Serialize>(val: &T) -> Response {
    let body = serde_json::to_string(val).unwrap_or_else(|_| "{}".to_string());
    (
        StatusCode::OK,
        [
            (header::CONTENT_TYPE, HeaderValue::from_static("application/json")),
            (header::CACHE_CONTROL, HeaderValue::from_static("no-store")),
        ],
        body,
    )
        .into_response()
}

fn err_json(code: StatusCode, msg: &str) -> Response {
    let body = json!({ "error": msg }).to_string();
    (
        code,
        [
            (header::CONTENT_TYPE, HeaderValue::from_static("application/json")),
            (header::CACHE_CONTROL, HeaderValue::from_static("no-store")),
        ],
        body,
    )
        .into_response()
}

async fn serve_static(State(state): State<Arc<SharedState>>, uri: Uri) -> Response {
    let raw = uri.path();
    if !state.remote_token.is_empty() {
        let tok_path = format!("/{}", state.remote_token);
        if raw == tok_path {
            return serve_asset(&state, "index.html");
        }
        if raw == format!("{tok_path}/") {
            return (
                StatusCode::FOUND,
                [(header::LOCATION, HeaderValue::from_str(&tok_path).unwrap_or(HeaderValue::from_static("/")))],
                Body::empty(),
            )
                .into_response();
        }
    }
    let rel = if raw == "/" { "index.html" } else { raw.trim_start_matches('/') };
    if rel.contains("..") || rel.contains('\\') || rel.contains('\0') {
        return err_json(StatusCode::FORBIDDEN, "forbidden");
    }
    serve_asset(&state, rel)
}

fn serve_asset(state: &SharedState, rel: &str) -> Response {
    let mime = match rel.rsplit('.').next().unwrap_or("") {
        "html" => "text/html; charset=utf-8",
        "css" => "text/css; charset=utf-8",
        "js" => "text/javascript; charset=utf-8",
        "svg" => "image/svg+xml",
        "json" => "application/json",
        _ => "application/octet-stream",
    };

    // Prefer on-disk `web_root` when present (allows live editing remote/ without rebuild)
    if state.web_root.is_dir() {
        let candidate = state.web_root.join(rel);
        if let (Ok(root_canon), Ok(file_canon)) = (state.web_root.canonicalize(), candidate.canonicalize()) {
            if file_canon.starts_with(&root_canon) && file_canon.is_file() {
                if let Ok(bytes) = fs::read(&file_canon) {
                    return (
                        StatusCode::OK,
                        [
                            (header::CONTENT_TYPE, HeaderValue::from_static(mime)),
                            (header::CACHE_CONTROL, HeaderValue::from_static("no-cache")),
                        ],
                        bytes,
                    )
                        .into_response();
                }
            }
        }
    }

    let embedded = match rel {
        "index.html" => Some(EMBED_INDEX.as_bytes()),
        "style.css" => Some(EMBED_STYLE.as_bytes()),
        "app.js" => Some(EMBED_APP.as_bytes()),
        "icon.svg" => Some(EMBED_ICON.as_bytes()),
        _ => None,
    };
    if let Some(bytes) = embedded {
        (
            StatusCode::OK,
            [
                (header::CONTENT_TYPE, HeaderValue::from_static(mime)),
                (header::CACHE_CONTROL, HeaderValue::from_static("no-cache")),
            ],
            bytes.to_vec(),
        )
            .into_response()
    } else {
        (StatusCode::NOT_FOUND, "not found").into_response()
    }
}

async fn api_status(
    State(state): State<Arc<SharedState>>,
    headers: HeaderMap,
    Query(q): Query<HashMap<String, String>>,
) -> Response {
    if let Err(r) = check_auth(&state, &headers, &q) { return r; }
    ok_json(&state.status_snapshot())
}

async fn api_presets(
    State(state): State<Arc<SharedState>>,
    headers: HeaderMap,
    Query(q): Query<HashMap<String, String>>,
) -> Response {
    if let Err(r) = check_auth(&state, &headers, &q) { return r; }
    let pl = state.playlist.lock().unwrap();
    let etag = format!("W/\"presets-{}\"", pl.catalog_revision);
    if headers.get(header::IF_NONE_MATCH).and_then(|v| v.to_str().ok()) == Some(etag.as_str()) {
        return (
            StatusCode::NOT_MODIFIED,
            [(header::ETAG, HeaderValue::from_str(&etag).unwrap())],
            Body::empty(),
        )
            .into_response();
    }
    #[derive(Serialize)]
    struct Item<'a> { i: usize, p: &'a str }
    let list: Vec<Item<'_>> = pl.items.iter().enumerate().map(|(i, p)| Item { i, p }).collect();
    let body = serde_json::to_string(&list).unwrap_or_else(|_| "[]".to_string());
    (
        StatusCode::OK,
        [
            (header::CONTENT_TYPE, HeaderValue::from_static("application/json")),
            (header::CACHE_CONTROL, HeaderValue::from_static("no-store")),
            (header::ETAG, HeaderValue::from_str(&etag).unwrap()),
        ],
        body,
    )
        .into_response()
}

async fn api_packs(
    State(state): State<Arc<SharedState>>,
    headers: HeaderMap,
    Query(q): Query<HashMap<String, String>>,
) -> Response {
    if let Err(r) = check_auth(&state, &headers, &q) { return r; }
    let packs = state.playlist.lock().unwrap().list_packs();
    ok_json(&packs)
}

async fn api_get_settings(
    State(state): State<Arc<SharedState>>,
    headers: HeaderMap,
    Query(q): Query<HashMap<String, String>>,
) -> Response {
    if let Err(r) = check_auth(&state, &headers, &q) { return r; }
    let s = state.settings.lock().unwrap().clone();
    ok_json(&s)
}

async fn api_post_settings(
    State(state): State<Arc<SharedState>>,
    headers: HeaderMap,
    Query(q): Query<HashMap<String, String>>,
) -> Response {
    if let Err(r) = check_auth(&state, &headers, &q) { return r; }
    let key = q.get("key").map(String::as_str).unwrap_or("");
    let val = q.get("value").map(String::as_str).unwrap_or("");
    {
        let mut s = state.settings.lock().unwrap();
        if let Err(e) = s.apply_kv(key, val) {
            return err_json(StatusCode::BAD_REQUEST, e);
        }
        if key == "livePreview" && !s.live_preview {
            state.preview.lock().unwrap().clear();
        }
    }
    state.push_cmd(Command::ApplySettings);
    ok_json(&json!({ "ok": true }))
}

async fn api_favorites(
    State(state): State<Arc<SharedState>>,
    headers: HeaderMap,
    Query(q): Query<HashMap<String, String>>,
) -> Response {
    if let Err(r) = check_auth(&state, &headers, &q) { return r; }
    let mut favs: Vec<String> = state.playlist.lock().unwrap().favorites.iter().cloned().collect();
    favs.sort();
    ok_json(&favs)
}

async fn api_audio_devices(
    State(state): State<Arc<SharedState>>,
    headers: HeaderMap,
    Query(q): Query<HashMap<String, String>>,
) -> Response {
    if let Err(r) = check_auth(&state, &headers, &q) { return r; }
    let (current, devices) = state.audio_devices.lock().unwrap().clone();
    ok_json(&json!({ "current": current, "devices": devices }))
}

async fn api_preview(
    State(state): State<Arc<SharedState>>,
    headers: HeaderMap,
    Query(q): Query<HashMap<String, String>>,
) -> Response {
    if let Err(r) = check_auth(&state, &headers, &q) { return r; }
    let enabled = state.settings.lock().unwrap().live_preview;
    if !enabled {
        return (
            StatusCode::NO_CONTENT,
            [
                (header::CACHE_CONTROL, HeaderValue::from_static("no-store")),
                (header::HeaderName::from_static("x-dropkick-preview"), HeaderValue::from_static("disabled")),
            ],
            Body::empty(),
        )
            .into_response();
    }
    let mut slot = state.preview.lock().unwrap();
    match slot.take_fresh(Instant::now()) {
        Some((bmp, seq)) => (
            StatusCode::OK,
            [
                (header::CONTENT_TYPE, HeaderValue::from_static("image/bmp")),
                (header::CACHE_CONTROL, HeaderValue::from_static("no-store")),
                (header::HeaderName::from_static("x-dropkick-frame"), HeaderValue::from_str(&seq.to_string()).unwrap()),
            ],
            bmp,
        )
            .into_response(),
        None => (
            StatusCode::NO_CONTENT,
            [
                (header::CACHE_CONTROL, HeaderValue::from_static("no-store")),
                (header::HeaderName::from_static("x-dropkick-preview"), HeaderValue::from_static("warming")),
            ],
            Body::empty(),
        )
            .into_response(),
    }
}

async fn api_workshop_source(
    State(state): State<Arc<SharedState>>,
    headers: HeaderMap,
    Query(q): Query<HashMap<String, String>>,
) -> Response {
    if let Err(r) = check_auth(&state, &headers, &q) { return r; }
    let path = state.playlist.lock().unwrap().current_path();
    let text = if path.is_empty() {
        String::new()
    } else {
        fs::read_to_string(&path).unwrap_or_default()
    };
    ok_json(&json!({ "path": path, "text": text }))
}

async fn api_next(State(s): State<Arc<SharedState>>, h: HeaderMap, Query(q): Query<HashMap<String, String>>) -> Response {
    if let Err(r) = check_auth(&s, &h, &q) { return r; }
    s.push_cmd(Command::Next { hard: false });
    ok_json(&json!({ "ok": true }))
}

async fn api_prev(State(s): State<Arc<SharedState>>, h: HeaderMap, Query(q): Query<HashMap<String, String>>) -> Response {
    if let Err(r) = check_auth(&s, &h, &q) { return r; }
    s.push_cmd(Command::Prev { hard: false });
    ok_json(&json!({ "ok": true }))
}

async fn api_random(State(s): State<Arc<SharedState>>, h: HeaderMap, Query(q): Query<HashMap<String, String>>) -> Response {
    if let Err(r) = check_auth(&s, &h, &q) { return r; }
    s.push_cmd(Command::Random { hard: false });
    ok_json(&json!({ "ok": true }))
}

async fn api_shuffle(State(s): State<Arc<SharedState>>, h: HeaderMap, Query(q): Query<HashMap<String, String>>) -> Response {
    if let Err(r) = check_auth(&s, &h, &q) { return r; }
    s.push_cmd(Command::ToggleShuffle);
    ok_json(&json!({ "ok": true }))
}

async fn api_lock(State(s): State<Arc<SharedState>>, h: HeaderMap, Query(q): Query<HashMap<String, String>>) -> Response {
    if let Err(r) = check_auth(&s, &h, &q) { return r; }
    s.push_cmd(Command::ToggleLock);
    ok_json(&json!({ "ok": true }))
}

async fn api_preset(State(s): State<Arc<SharedState>>, h: HeaderMap, Query(q): Query<HashMap<String, String>>) -> Response {
    if let Err(r) = check_auth(&s, &h, &q) { return r; }
    let Some(idx) = q.get("index").and_then(|v| v.parse::<usize>().ok()) else {
        return err_json(StatusCode::BAD_REQUEST, "invalid index");
    };
    s.push_cmd(Command::SelectIndex { index: idx, hard: false });
    ok_json(&json!({ "ok": true }))
}

async fn api_pack(State(s): State<Arc<SharedState>>, h: HeaderMap, Query(q): Query<HashMap<String, String>>) -> Response {
    if let Err(r) = check_auth(&s, &h, &q) { return r; }
    let name = q.get("name").cloned().unwrap_or_default();
    if name.is_empty() || name.contains("..") || name.contains('/') || name.contains('\\') {
        return err_json(StatusCode::BAD_REQUEST, "invalid pack name");
    }
    let exists = s.playlist.lock().unwrap().list_packs().iter().any(|p| p == &name);
    if !exists {
        return err_json(StatusCode::NOT_FOUND, "unknown pack");
    }
    s.push_cmd(Command::SwitchPack(name));
    ok_json(&json!({ "ok": true }))
}

async fn api_favorites_toggle(State(s): State<Arc<SharedState>>, h: HeaderMap, Query(q): Query<HashMap<String, String>>) -> Response {
    if let Err(r) = check_auth(&s, &h, &q) { return r; }
    let path = q.get("path").cloned().unwrap_or_default();
    if path.is_empty() {
        return err_json(StatusCode::BAD_REQUEST, "missing path");
    }
    let on = s.playlist.lock().unwrap().toggle_favorite(&path);
    ok_json(&json!({ "ok": true, "favorited": on }))
}

async fn api_favorites_shuffle(State(s): State<Arc<SharedState>>, h: HeaderMap, Query(q): Query<HashMap<String, String>>) -> Response {
    if let Err(r) = check_auth(&s, &h, &q) { return r; }
    let on = {
        let mut pl = s.playlist.lock().unwrap();
        pl.favorites_shuffle = !pl.favorites_shuffle;
        if pl.favorites_shuffle {
            pl.category_shuffle.clear();
        }
        pl.favorites_shuffle
    };
    s.push_cmd(Command::ToggleFavoritesShuffle);
    ok_json(&json!({ "ok": true, "favoritesShuffle": on }))
}

async fn api_category_shuffle(State(s): State<Arc<SharedState>>, h: HeaderMap, Query(q): Query<HashMap<String, String>>) -> Response {
    if let Err(r) = check_auth(&s, &h, &q) { return r; }
    let mut path = q.get("path").cloned().unwrap_or_default();
    if path.is_empty() || path.contains("..") {
        return err_json(StatusCode::BAD_REQUEST, "invalid category path");
    }
    if !path.ends_with('/') {
        path.push('/');
    }
    let on = {
        let mut pl = s.playlist.lock().unwrap();
        let enable = pl.category_shuffle != path;
        pl.category_shuffle = if enable { path.clone() } else { String::new() };
        if enable {
            pl.favorites_shuffle = false;
        }
        enable
    };
    if on {
        s.push_cmd(Command::ToggleCategoryShuffle);
    }
    ok_json(&json!({ "ok": true, "categoryShuffle": on }))
}

async fn api_dislike(State(s): State<Arc<SharedState>>, h: HeaderMap, Query(q): Query<HashMap<String, String>>) -> Response {
    if let Err(r) = check_auth(&s, &h, &q) { return r; }
    s.push_cmd(Command::DislikeCurrent);
    ok_json(&json!({ "ok": true }))
}

async fn api_dislikes_clear(State(s): State<Arc<SharedState>>, h: HeaderMap, Query(q): Query<HashMap<String, String>>) -> Response {
    if let Err(r) = check_auth(&s, &h, &q) { return r; }
    s.push_cmd(Command::ClearDislikes);
    ok_json(&json!({ "ok": true }))
}

async fn api_blocklist_clear(State(s): State<Arc<SharedState>>, h: HeaderMap, Query(q): Query<HashMap<String, String>>) -> Response {
    if let Err(r) = check_auth(&s, &h, &q) { return r; }
    s.push_cmd(Command::ClearBlocklist);
    ok_json(&json!({ "ok": true }))
}

async fn api_audio_select(State(s): State<Arc<SharedState>>, h: HeaderMap, Query(q): Query<HashMap<String, String>>) -> Response {
    if let Err(r) = check_auth(&s, &h, &q) { return r; }
    let Some(id) = q.get("index").and_then(|v| v.parse::<i32>().ok()) else {
        return err_json(StatusCode::BAD_REQUEST, "invalid index");
    };
    let audio_name = {
        let (_, ref devs) = *s.audio_devices.lock().unwrap();
        match devs.iter().find(|d| d.id == id) {
            Some(d) => d.name.clone(),
            None => return err_json(StatusCode::BAD_REQUEST, "invalid index"),
        }
    };
    s.push_cmd(Command::SelectAudio(id));
    ok_json(&json!({ "ok": true, "audio": audio_name }))
}

async fn api_audio_next(State(s): State<Arc<SharedState>>, h: HeaderMap, Query(q): Query<HashMap<String, String>>) -> Response {
    if let Err(r) = check_auth(&s, &h, &q) { return r; }
    s.push_cmd(Command::NextAudio);
    ok_json(&json!({ "ok": true }))
}

async fn api_workshop_capture(State(s): State<Arc<SharedState>>, h: HeaderMap, Query(q): Query<HashMap<String, String>>) -> Response {
    if let Err(r) = check_auth(&s, &h, &q) { return r; }
    let (cur, wdir) = {
        let pl = s.playlist.lock().unwrap();
        (pl.current_path(), pl.workshop_dir.clone())
    };
    if cur.is_empty() {
        return err_json(StatusCode::BAD_REQUEST, "no active preset");
    }
    let Ok(bytes) = fs::read(&cur) else {
        return err_json(StatusCode::INTERNAL_SERVER_ERROR, "failed to read preset");
    };
    let fname = PathBuf::from(&cur)
        .file_name()
        .map(|f| f.to_string_lossy().to_string())
        .unwrap_or_else(|| "captured.milk".to_string());
    let dest = wdir.join(&fname);
    if write_file_atomic(&dest, &bytes).is_err() {
        return err_json(StatusCode::INTERNAL_SERVER_ERROR, "failed to write workshop file");
    }
    s.push_cmd(Command::LoadWorkshopFile(dest));
    ok_json(&json!({ "ok": true, "file": fname }))
}

async fn api_workshop_apply(
    State(s): State<Arc<SharedState>>,
    h: HeaderMap,
    Query(q): Query<HashMap<String, String>>,
    body: String,
) -> Response {
    if let Err(r) = check_auth(&s, &h, &q) { return r; }
    let wdir = s.playlist.lock().unwrap().workshop_dir.clone();
    let dest = wdir.join("_scratch.milk");
    if write_file_atomic(&dest, body.as_bytes()).is_err() {
        return err_json(StatusCode::INTERNAL_SERVER_ERROR, "failed to write scratch");
    }
    s.push_cmd(Command::ApplyWorkshop { path: dest, text: body });
    ok_json(&json!({ "ok": true }))
}

async fn api_workshop_save(
    State(s): State<Arc<SharedState>>,
    h: HeaderMap,
    Query(q): Query<HashMap<String, String>>,
    body: String,
) -> Response {
    if let Err(r) = check_auth(&s, &h, &q) { return r; }
    let raw_name = q.get("name").map(String::as_str).unwrap_or("");
    let base = raw_name.rsplit(['/', '\\']).next().unwrap_or("").trim();
    if base.is_empty() || base.contains("..") || base.contains(':') || base.contains('\0') {
        return err_json(StatusCode::BAD_REQUEST, "invalid name");
    }
    let file = if base.ends_with(".milk") || base.ends_with(".milk2") {
        base.to_string()
    } else {
        format!("{base}.milk")
    };
    let wdir = s.playlist.lock().unwrap().workshop_dir.clone();
    let dest = wdir.join(&file);
    let overwrite = q.get("overwrite").map(String::as_str) == Some("1");
    if !overwrite && dest.exists() {
        return err_json(StatusCode::CONFLICT, "file exists");
    }
    if write_file_atomic(&dest, body.as_bytes()).is_err() {
        return err_json(StatusCode::INTERNAL_SERVER_ERROR, "failed to save preset");
    }
    let norm = normalize_path(&dest);
    ok_json(&json!({ "ok": true, "file": file, "path": norm }))
}
