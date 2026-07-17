#include "RemoteControl.h"

#include "AudioCapture.h"
#include "ProjectMSDLApplication.h"
#include "notifications/PlaybackControlNotification.h"

#include <httplib.h>

#include <Poco/NotificationCenter.h>
#include <Poco/Path.h>
#include <Poco/Util/Application.h>

#include <projectM-4/projectM.h>
#include <projectM-4/playlist.h>

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace
{
/*
 * Playlist items on Windows arrive with mixed separators (the configured preset
 * root uses '/', the scanned tail '\'), which breaks the web app's '/'-based
 * category parsing and favorite matching. Serve and store everything with forward
 * slashes — Windows file APIs accept '/' fine. No-op elsewhere ('\' is a legal
 * character in POSIX filenames, so never rewrite it there).
 */
std::string NormalizePathSeparators(std::string path)
{
#ifdef _WIN32
    std::replace(path.begin(), path.end(), '\\', '/');
#endif
    return path;
}

std::string JsonEscape(const std::string& in)
{
    std::string out;
    for (char c : in)
    {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') { out += "\\n"; }
        else if (c == '\r') { out += "\\r"; }
        else if (c == '\t') { out += "\\t"; }
        else if (static_cast<unsigned char>(c) < 0x20) { /* drop other control bytes */ }
        else { out += c; }
    }
    return out;
}

/**
 * Minimal parser for a JSON array of strings as written by SaveFavorites().
 * Scans for double-quoted strings, honoring backslash escapes.
 */
std::vector<std::string> ParseStringArray(const std::string& text)
{
    std::vector<std::string> out;
    std::string cur;
    bool inString = false;
    for (size_t i = 0; i < text.size(); ++i)
    {
        char c = text[i];
        if (!inString)
        {
            if (c == '"') { inString = true; cur.clear(); }
            continue;
        }
        if (c == '\\' && i + 1 < text.size())
        {
            char n = text[++i];
            if (n == 'n') { cur += '\n'; }
            else if (n == 'r') { cur += '\r'; }
            else if (n == 't') { cur += '\t'; }
            else { cur += n; }
        }
        else if (c == '"') { inString = false; out.push_back(cur); }
        else { cur += c; }
    }
    return out;
}
} // namespace

RemoteControl::RemoteControl() = default;
RemoteControl::~RemoteControl() = default;

const char* RemoteControl::name() const
{
    return "RemoteControl";
}

void RemoteControl::initialize(Poco::Util::Application& app)
{
    auto config = app.config().createView("remote");
    _port = static_cast<uint16_t>(config->getInt("port", 8080));
    _token = config->getString("token", "");
    // Poco::Path::expand() does not resolve "~" on Windows; home() works everywhere.
    const std::string dataDir = Poco::Path::home() + ".local/share/dropkick/";
    _presetRoot = config->getString("presetRoot", dataDir + "presets");
    _webRoot = config->getString("webRoot", dataDir + "remote");
    _favoritesFile = config->getString("favoritesFile", dataDir + "favorites.json");
    _workshopDir = config->getString("workshopDir", dataDir + "workshop");

    if (_token.empty())
    {
        poco_warning(_logger, "Remote control is running WITHOUT a token: anyone on the network can "
                              "control Dropkick and upload presets. Set remote.token "
                              "(DROPKICK_REMOTE_TOKEN) to require authentication.");
    }

    LoadFavorites();

    _server = std::make_unique<httplib::Server>();
    // Presets are small text files; cap request bodies so an unauthenticated client can't
    // exhaust memory/disk via /api/workshop/apply or /save.
    _server->set_payload_max_length(1 * 1024 * 1024); // 1 MiB
    RegisterRoutes();

    _running = true;
    _serverThread = std::thread([this]() {
        poco_information_f1(_logger, "Remote control starting on port %u.", static_cast<unsigned>(_port));
        if (!_server->listen("0.0.0.0", _port))
        {
            poco_error_f1(_logger, "Remote control failed to bind port %u (already in use?).", static_cast<unsigned>(_port));
        }
    });
}

void RemoteControl::uninitialize()
{
    _running = false;
    if (_server) { _server->stop(); }
    if (_serverThread.joinable()) { _serverThread.join(); }
    _server.reset();
}

bool RemoteControl::Authorized(const std::string& token) const
{
    if (_token.empty()) { return true; }
    // Constant-time comparison to avoid leaking the token through response timing.
    // (The length is allowed to leak, as is standard for fixed-secret comparisons.)
    if (token.size() != _token.size()) { return false; }
    unsigned char diff = 0;
    for (size_t i = 0; i < _token.size(); ++i)
    {
        diff |= static_cast<unsigned char>(token[i]) ^ static_cast<unsigned char>(_token[i]);
    }
    return diff == 0;
}

void RemoteControl::Enqueue(const Command& command)
{
    std::lock_guard<std::mutex> lock(_queueMutex);
    _queue.push_back(command);
}

void RemoteControl::RegisterRoutes()
{
    auto guard = [this](const httplib::Request& req, httplib::Response& res) -> bool {
        std::string token = req.get_header_value("X-Dropkick-Token");
        if (token.empty()) { token = req.get_param_value("token"); }
        if (!Authorized(token))
        {
            res.status = 401;
            res.set_content("{\"error\":\"unauthorized\"}", "application/json");
            return false;
        }
        return true;
    };

    // Guarded GET returning the JSON produced by a member function.
    auto getJson = [this, guard](const char* path, std::string (RemoteControl::*fn)() const) {
        _server->Get(path, [this, guard, fn](const httplib::Request& req, httplib::Response& res) {
            if (!guard(req, res)) { return; }
            res.set_content((this->*fn)(), "application/json");
        });
    };

    // Guarded POST that just enqueues an argument-less command.
    auto post = [this, guard](const char* path, CommandType type) {
        _server->Post(path, [this, guard, type](const httplib::Request& req, httplib::Response& res) {
            if (!guard(req, res)) { return; }
            Enqueue(Command{type, "", ""});
            res.set_content("{\"ok\":true}", "application/json");
        });
    };

    _server->set_mount_point("/", _webRoot); // serves index.html, app.js, style.css

    getJson("/api/status", &RemoteControl::StatusJson);
    getJson("/api/packs", &RemoteControl::PacksJson);
    getJson("/api/presets", &RemoteControl::PresetsJson);
    getJson("/api/settings", &RemoteControl::SettingsJson);
    getJson("/api/favorites", &RemoteControl::FavoritesJson);

    _server->Post("/api/favorites/toggle", [this, guard](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) { return; }
        std::string path = req.get_param_value("path");
        if (path.empty())
        {
            res.status = 400;
            res.set_content("{\"error\":\"missing path\"}", "application/json");
            return;
        }
        bool nowFavorite = ToggleFavorite(path);
        res.set_content(std::string("{\"ok\":true,\"favorited\":") + (nowFavorite ? "true" : "false") + "}", "application/json");
    });

    _server->Post("/api/favorites/shuffle", [this, guard](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) { return; }
        bool on = !_favShuffle.load();
        _favShuffle = on;
        res.set_content(std::string("{\"ok\":true,\"favoritesShuffle\":") + (on ? "true" : "false") + "}", "application/json");
    });

    _server->Post("/api/preset", [this, guard](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) { return; }
        std::string index = req.get_param_value("index");
        if (index.empty() || index.find_first_not_of("0123456789") != std::string::npos)
        {
            res.status = 400;
            res.set_content("{\"error\":\"invalid index\"}", "application/json");
            return;
        }
        Enqueue(Command{CommandType::SetPosition, index, ""});
        res.set_content("{\"ok\":true}", "application/json");
    });

    _server->Post("/api/settings", [this, guard](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) { return; }
        std::string key = req.get_param_value("key");
        std::string value = req.get_param_value("value");
        static const std::set<std::string> kKeys{
            "presetDuration", "softCutDuration", "hardCut", "hardCutDuration",
            "hardCutSensitivity", "beatSensitivity", "fps", "aspectCorrection",
            "reduceFlashing", "flashStrength",
            "brightness", "tintEnabled", "tintColor", "tintStrength",
            "autoskipEnabled", "autoskipFps", "autoskipStrikes"};
        if (!kKeys.count(key) || value.empty())
        {
            res.status = 400;
            res.set_content("{\"error\":\"invalid setting\"}", "application/json");
            return;
        }
        Enqueue(Command{CommandType::SetSetting, key, value});
        res.set_content("{\"ok\":true}", "application/json");
    });

    post("/api/workshop/capture", CommandType::CaptureWorkshop);
    post("/api/blocklist/clear", CommandType::ClearBlocklist);
    post("/api/dislike", CommandType::DislikeCurrent);
    post("/api/dislikes/clear", CommandType::ClearDislikes);
    post("/api/next", CommandType::Next);
    post("/api/prev", CommandType::Previous);
    post("/api/random", CommandType::Random);
    post("/api/shuffle", CommandType::ToggleShuffle);
    post("/api/lock", CommandType::ToggleLock);
    post("/api/audio/next", CommandType::NextAudio);

    // In-remote preset editor.
    _server->Get("/api/workshop/source", [this, guard](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) { return; }
        std::string path;
        {
            std::lock_guard<std::mutex> lock(_statusMutex);
            path = _editPath;
        }
        std::string text;
        std::ifstream in(path, std::ios::binary);
        if (in)
        {
            std::stringstream buf;
            buf << in.rdbuf();
            text = buf.str();
        }
        std::ostringstream json;
        json << "{\"path\":\"" << JsonEscape(path) << "\",\"text\":\"" << JsonEscape(text) << "\"}";
        res.set_content(json.str(), "application/json");
    });

    _server->Post("/api/workshop/apply", [this, guard](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) { return; }
        std::string scratch = _workshopDir + "/_scratch.milk";
        fs::create_directories(_workshopDir);
        std::ofstream out(scratch, std::ios::trunc | std::ios::binary);
        if (!out)
        {
            res.status = 500;
            res.set_content("{\"error\":\"cannot write scratch\"}", "application/json");
            return;
        }
        out << req.body;
        out.close();
        Enqueue(Command{CommandType::LoadWorkshopPath, scratch, ""});
        res.set_content("{\"ok\":true}", "application/json");
    });

    _server->Post("/api/workshop/save", [this, guard](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) { return; }
        std::string name = req.get_param_value("name");
        // sanitize: strip any directory parts and enforce .milk
        auto slash = name.find_last_of("/\\");
        if (slash != std::string::npos) { name = name.substr(slash + 1); }
        if (name.empty() || name.find("..") != std::string::npos)
        {
            res.status = 400;
            res.set_content("{\"error\":\"invalid name\"}", "application/json");
            return;
        }
        if (name.size() < 5 || name.compare(name.size() - 5, 5, ".milk") != 0) { name += ".milk"; }
        std::string dest = _workshopDir + "/" + name;
        fs::create_directories(_workshopDir);
        std::ofstream out(dest, std::ios::trunc | std::ios::binary);
        if (!out)
        {
            res.status = 500;
            res.set_content("{\"error\":\"cannot save\"}", "application/json");
            return;
        }
        out << req.body;
        res.set_content(std::string("{\"ok\":true,\"file\":\"") + JsonEscape(name) + "\"}", "application/json");
    });

    _server->Post("/api/pack", [this, guard](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) { return; }
        std::string pack = req.get_param_value("name");
        if (pack.empty() || pack.find("..") != std::string::npos || pack.find('/') != std::string::npos)
        {
            res.status = 400;
            res.set_content("{\"error\":\"invalid pack name\"}", "application/json");
            return;
        }
        Enqueue(Command{CommandType::LoadPack, pack, ""});
        res.set_content("{\"ok\":true}", "application/json");
    });
}

void RemoteControl::DrainCommands()
{
    if (_presetsDirty.exchange(false))
    {
        RebuildPresetCache();
    }

    PollWorkshop();

    std::deque<Command> pending;
    {
        std::lock_guard<std::mutex> lock(_queueMutex);
        pending.swap(_queue);
    }

    auto& center = Poco::NotificationCenter::defaultCenter();
    auto& app = ProjectMSDLApplication::instance();

    for (const auto& command : pending)
    {
        switch (command.type)
        {
            case CommandType::Next:
                _workshopActive = false;
                if (_favShuffle.load()) { JumpToFavorite(true); }
                else
                {
                    center.postNotification(new PlaybackControlNotification(PlaybackControlNotification::Action::NextPreset));
                }
                break;
            case CommandType::Previous:
                _workshopActive = false;
                center.postNotification(new PlaybackControlNotification(PlaybackControlNotification::Action::PreviousPreset));
                break;
            case CommandType::Random:
                _workshopActive = false;
                if (_favShuffle.load()) { JumpToFavorite(false); }
                else
                {
                    center.postNotification(new PlaybackControlNotification(PlaybackControlNotification::Action::RandomPreset));
                }
                break;
            case CommandType::ToggleShuffle:
                center.postNotification(new PlaybackControlNotification(PlaybackControlNotification::Action::ToggleShuffle));
                break;
            case CommandType::ToggleLock:
                center.postNotification(new PlaybackControlNotification(PlaybackControlNotification::Action::TogglePresetLocked));
                break;
            case CommandType::NextAudio:
                app.getSubsystem<AudioCapture>().NextAudioDevice();
                break;
            case CommandType::LoadPack:
            {
                std::string path = _presetRoot + "/" + command.arg;
                app.getSubsystem<ProjectMWrapper>().LoadPresetPack(path);
                _presetsDirty = true;
                break;
            }
            case CommandType::SetPosition:
            {
                auto playlist = app.getSubsystem<ProjectMWrapper>().Playlist();
                if (playlist)
                {
                    uint32_t index = static_cast<uint32_t>(std::strtoul(command.arg.c_str(), nullptr, 10));
                    if (index < projectm_playlist_size(playlist))
                    {
                        _workshopActive = false;
                        projectm_playlist_set_position(playlist, index, true);
                    }
                }
                break;
            }
            case CommandType::SetSetting:
                ApplySetting(command.arg, command.arg2);
                break;
            case CommandType::CaptureWorkshop:
                CaptureToWorkshop();
                break;
            case CommandType::ClearBlocklist:
                app.getSubsystem<ProjectMWrapper>().ClearBlocklist();
                break;
            case CommandType::LoadWorkshopPath:
                app.getSubsystem<ProjectMWrapper>().LoadPresetFile(command.arg);
                _workshopActive = true;
                _workshopPath = command.arg;
                break;
            case CommandType::DislikeCurrent:
                _workshopActive = false;
                app.getSubsystem<ProjectMWrapper>().DislikeCurrent();
                break;
            case CommandType::ClearDislikes:
                app.getSubsystem<ProjectMWrapper>().ClearDislikes();
                break;
        }
    }
}

void RemoteControl::RebuildPresetCache()
{
    auto& wrapper = ProjectMSDLApplication::instance().getSubsystem<ProjectMWrapper>();
    auto items = wrapper.PlaylistItems();

    _pathToIndex.clear();
    std::ostringstream json;
    json << "[";
    for (uint32_t i = 0; i < items.size(); ++i)
    {
        const std::string item = NormalizePathSeparators(items[i]);
        _pathToIndex[item] = i;
        if (i) { json << ","; }
        json << "{\"i\":" << i << ",\"p\":\"" << JsonEscape(item) << "\"}";
    }
    json << "]";

    {
        std::lock_guard<std::mutex> lock(_dataMutex);
        _presetsJson = json.str();
    }
    poco_information_f1(_logger, "Preset cache rebuilt (%z items).", items.size());
}

void RemoteControl::JumpToFavorite(bool nextInOrder)
{
    auto playlist = ProjectMSDLApplication::instance().getSubsystem<ProjectMWrapper>().Playlist();
    if (!playlist) { return; }

    std::vector<uint32_t> favoriteIndices;
    {
        std::lock_guard<std::mutex> lock(_favMutex);
        for (const auto& path : _favorites)
        {
            auto it = _pathToIndex.find(path);
            if (it != _pathToIndex.end()) { favoriteIndices.push_back(it->second); }
        }
    }
    if (favoriteIndices.empty()) { return; }
    std::sort(favoriteIndices.begin(), favoriteIndices.end());

    uint32_t target;
    if (nextInOrder)
    {
        uint32_t current = projectm_playlist_get_position(playlist);
        target = favoriteIndices.front();
        for (uint32_t idx : favoriteIndices)
        {
            if (idx > current) { target = idx; break; }
        }
    }
    else
    {
        target = favoriteIndices[static_cast<size_t>(std::rand()) % favoriteIndices.size()];
    }
    projectm_playlist_set_position(playlist, target, true);
}

void RemoteControl::ApplySetting(const std::string& key, const std::string& value)
{
    auto& app = ProjectMSDLApplication::instance();
    auto pm = app.getSubsystem<ProjectMWrapper>().ProjectM();
    if (!pm) { return; }

    double v = std::atof(value.c_str());
    bool on = (value == "true" || v != 0.0);

    if (key == "presetDuration") { projectm_set_preset_duration(pm, v); }
    else if (key == "softCutDuration") { projectm_set_soft_cut_duration(pm, v); }
    else if (key == "hardCut") { projectm_set_hard_cut_enabled(pm, on); }
    else if (key == "hardCutDuration") { projectm_set_hard_cut_duration(pm, v); }
    else if (key == "hardCutSensitivity") { projectm_set_hard_cut_sensitivity(pm, static_cast<float>(v)); }
    else if (key == "beatSensitivity") { projectm_set_beat_sensitivity(pm, static_cast<float>(v)); }
    else if (key == "aspectCorrection") { projectm_set_aspect_correction(pm, on); }
    else if (key == "fps")
    {
        projectm_set_fps(pm, static_cast<int32_t>(v));
        // The frontend's FPS limiter reads projectM.fps from the user config.
        app.UserConfiguration()->setInt("projectM.fps", static_cast<int>(v));
    }
    // Strobe damper settings are frontend-side (read by RenderLoop from the user config).
    else if (key == "reduceFlashing") { app.UserConfiguration()->setBool("projectM.reduceFlashing", on); }
    else if (key == "flashStrength") { app.UserConfiguration()->setDouble("projectM.flashStrength", v); }
    else if (key == "brightness") { app.UserConfiguration()->setDouble("projectM.brightness", v); }
    else if (key == "tintEnabled") { app.UserConfiguration()->setBool("projectM.tintEnabled", on); }
    else if (key == "tintColor") { app.UserConfiguration()->setString("projectM.tintColor", value); }
    else if (key == "tintStrength") { app.UserConfiguration()->setDouble("projectM.tintStrength", v); }
    else if (key == "autoskipEnabled") { app.UserConfiguration()->setBool("projectM.autoskipEnabled", on); }
    else if (key == "autoskipFps") { app.UserConfiguration()->setDouble("projectM.autoskipFps", v); }
    else if (key == "autoskipStrikes") { app.UserConfiguration()->setInt("projectM.autoskipStrikes", static_cast<int>(v)); }
}

void RemoteControl::PollWorkshop()
{
    long now = static_cast<long>(::time(nullptr));
    if (now == _lastWorkshopPoll) { return; } // throttle to ~1 Hz
    _lastWorkshopPoll = now;

    std::error_code ec;
    if (!fs::is_directory(_workshopDir, ec)) { return; }

    std::string newest;
    long long newestMtime = 0;
    for (const auto& entry : fs::directory_iterator(_workshopDir, ec))
    {
        if (ec) { break; }
        if (!entry.is_regular_file() || entry.path().extension() != ".milk") { continue; }
        std::string full = entry.path().string();
        long long mtime = 0;
        auto t = fs::last_write_time(entry.path(), ec);
        if (!ec) { mtime = t.time_since_epoch().count(); }

        auto it = _workshopSeen.find(full);
        bool changed = (it == _workshopSeen.end() || it->second != mtime);
        _workshopSeen[full] = mtime;
        if (changed && mtime >= newestMtime)
        {
            newestMtime = mtime;
            newest = full;
        }
    }

    if (!_workshopSeeded)
    {
        _workshopSeeded = true; // first pass just records state; don't hijack the current preset
        return;
    }

    if (!newest.empty())
    {
        ProjectMSDLApplication::instance().getSubsystem<ProjectMWrapper>().LoadPresetFile(newest);
        _workshopActive = true;
        _workshopPath = newest;
        poco_information_f1(_logger, "Workshop: live-loaded %s", newest);
    }
}

void RemoteControl::CaptureToWorkshop()
{
    if (_currentPath.empty()) { return; }

    fs::create_directories(_workshopDir);

    std::string base = _currentPath.substr(_currentPath.find_last_of('/') + 1);
    std::string stem = base;
    std::string ext;
    auto dot = base.rfind(".milk");
    if (dot != std::string::npos) { stem = base.substr(0, dot); ext = ".milk"; }

    std::string dest = _workshopDir + "/" + stem + ext;
    int n = 1;
    while (fs::exists(dest))
    {
        dest = _workshopDir + "/" + stem + " (" + std::to_string(n++) + ")" + ext;
    }

    std::ifstream in(_currentPath, std::ios::binary);
    std::ofstream out(dest, std::ios::binary);
    if (!in || !out)
    {
        poco_error_f1(_logger, "Workshop capture failed for %s", _currentPath);
        return;
    }
    out << in.rdbuf();
    out.close();

    // Load the copy live and register it so the watcher doesn't reload it.
    std::error_code ec;
    auto t = fs::last_write_time(dest, ec);
    if (!ec) { _workshopSeen[dest] = t.time_since_epoch().count(); }
    ProjectMSDLApplication::instance().getSubsystem<ProjectMWrapper>().LoadPresetFile(dest);
    _workshopActive = true;
    _workshopPath = dest;
    poco_information_f1(_logger, "Workshop: captured current preset to %s", dest);
}

void RemoteControl::SampleSystemStats()
{
    // CPU % from /proc/stat delta.
    {
        std::ifstream in("/proc/stat");
        std::string cpu;
        unsigned long long u = 0, n = 0, s = 0, i = 0, io = 0, irq = 0, sirq = 0, st = 0;
        if (in >> cpu >> u >> n >> s >> i >> io >> irq >> sirq >> st)
        {
            unsigned long long idle = i + io;
            unsigned long long total = u + n + s + i + io + irq + sirq + st;
            if (_prevCpuTotal != 0 && total > _prevCpuTotal)
            {
                double dTotal = static_cast<double>(total - _prevCpuTotal);
                double dIdle = static_cast<double>(idle - _prevCpuIdle);
                _cpuPct = static_cast<float>((1.0 - dIdle / dTotal) * 100.0);
            }
            _prevCpuTotal = total;
            _prevCpuIdle = idle;
        }
    }
    // Memory from /proc/meminfo.
    {
        std::ifstream in("/proc/meminfo");
        std::string key, unit;
        long value = 0, memTotal = 0, memAvail = 0;
        while (in >> key >> value >> unit)
        {
            if (key == "MemTotal:") { memTotal = value; }
            else if (key == "MemAvailable:") { memAvail = value; break; }
        }
        if (memTotal > 0)
        {
            _memTotalMB = memTotal / 1024;
            _memUsedMB = (memTotal - memAvail) / 1024;
        }
    }
    // Temperature from the thermal zone (°C).
    {
        std::ifstream in("/sys/class/thermal/thermal_zone0/temp");
        long milli = 0;
        if (in >> milli) { _tempC = milli / 1000.0f; }
    }
}

void RemoteControl::PublishStatus(const ProjectMWrapper::PlaybackStatus& status, const std::string& audioDevice, float fps)
{
    _currentPath = status.presetName;
    std::string reportedPreset = NormalizePathSeparators(_workshopActive ? _workshopPath : status.presetName);

    _fps = fps;
    long nowSec = static_cast<long>(::time(nullptr));
    if (nowSec != _lastStatsSample) { _lastStatsSample = nowSec; SampleSystemStats(); }
    bool favorited;
    {
        std::lock_guard<std::mutex> lock(_favMutex);
        favorited = _favorites.count(reportedPreset) > 0;
    }

    auto& app = ProjectMSDLApplication::instance();
    auto& wrapper = app.getSubsystem<ProjectMWrapper>();
    auto& config = app.config();

    std::ostringstream json;
    json << "{"
         << "\"preset\":\"" << JsonEscape(reportedPreset) << "\","
         << "\"position\":" << status.position << ","
         << "\"size\":" << status.playlistSize << ","
         << "\"shuffle\":" << (status.shuffle ? "true" : "false") << ","
         << "\"locked\":" << (status.locked ? "true" : "false") << ","
         << "\"favorited\":" << (favorited ? "true" : "false") << ","
         << "\"favoritesShuffle\":" << (_favShuffle.load() ? "true" : "false") << ","
         << "\"workshop\":" << (_workshopActive ? "true" : "false") << ","
         << "\"blocked\":" << wrapper.BlockedCount() << ","
         << "\"disliked\":" << wrapper.DislikedCount() << ","
         << "\"fps\":" << static_cast<int>(_fps + 0.5f) << ","
         << "\"cpu\":" << static_cast<int>(_cpuPct + 0.5f) << ","
         << "\"memUsed\":" << _memUsedMB << ","
         << "\"memTotal\":" << _memTotalMB << ","
         << "\"temp\":" << static_cast<int>(_tempC + 0.5f) << ","
         << "\"audio\":\"" << JsonEscape(audioDevice) << "\""
         << "}";

    // Settings snapshot (render thread — safe to query projectM here).
    std::ostringstream settings;
    auto pm = wrapper.ProjectM();
    if (pm)
    {
        settings << "{"
                 << "\"presetDuration\":" << projectm_get_preset_duration(pm) << ","
                 << "\"softCutDuration\":" << projectm_get_soft_cut_duration(pm) << ","
                 << "\"hardCut\":" << (projectm_get_hard_cut_enabled(pm) ? "true" : "false") << ","
                 << "\"hardCutDuration\":" << projectm_get_hard_cut_duration(pm) << ","
                 << "\"hardCutSensitivity\":" << projectm_get_hard_cut_sensitivity(pm) << ","
                 << "\"beatSensitivity\":" << projectm_get_beat_sensitivity(pm) << ","
                 << "\"fps\":" << projectm_get_fps(pm) << ","
                 << "\"aspectCorrection\":" << (projectm_get_aspect_correction(pm) ? "true" : "false") << ","
                 << "\"reduceFlashing\":" << (config.getBool("projectM.reduceFlashing", false) ? "true" : "false") << ","
                 << "\"flashStrength\":" << config.getDouble("projectM.flashStrength", 0.6) << ","
                 << "\"brightness\":" << config.getDouble("projectM.brightness", 1.0) << ","
                 << "\"tintEnabled\":" << (config.getBool("projectM.tintEnabled", false) ? "true" : "false") << ","
                 << "\"tintColor\":\"" << JsonEscape(config.getString("projectM.tintColor", "#00ff00")) << "\","
                 << "\"tintStrength\":" << config.getDouble("projectM.tintStrength", 1.0) << ","
                 << "\"autoskipEnabled\":" << (config.getBool("projectM.autoskipEnabled", true) ? "true" : "false") << ","
                 << "\"autoskipFps\":" << config.getDouble("projectM.autoskipFps", 20.0) << ","
                 << "\"autoskipStrikes\":" << config.getInt("projectM.autoskipStrikes", 3)
                 << "}";
    }
    else
    {
        settings << "{}";
    }

    std::lock_guard<std::mutex> lock(_statusMutex);
    _statusJson = json.str();
    _settingsJson = settings.str();
    _editPath = _workshopActive ? _workshopPath : _currentPath;
}

std::string RemoteControl::StatusJson() const
{
    std::lock_guard<std::mutex> lock(_statusMutex);
    return _statusJson;
}

std::string RemoteControl::SettingsJson() const
{
    std::lock_guard<std::mutex> lock(_statusMutex);
    return _settingsJson;
}

std::string RemoteControl::PresetsJson() const
{
    std::lock_guard<std::mutex> lock(_dataMutex);
    return _presetsJson;
}

std::string RemoteControl::FavoritesJson() const
{
    std::ostringstream json;
    json << "[";
    std::lock_guard<std::mutex> lock(_favMutex);
    bool first = true;
    for (const auto& path : _favorites)
    {
        if (!first) { json << ","; }
        json << "\"" << JsonEscape(path) << "\"";
        first = false;
    }
    json << "]";
    return json.str();
}

bool RemoteControl::ToggleFavorite(const std::string& path)
{
    const std::string normalized = NormalizePathSeparators(path);
    bool nowFavorite;
    {
        std::lock_guard<std::mutex> lock(_favMutex);
        auto it = _favorites.find(normalized);
        if (it != _favorites.end()) { _favorites.erase(it); nowFavorite = false; }
        else { _favorites.insert(normalized); nowFavorite = true; }
    }
    SaveFavorites();
    return nowFavorite;
}

void RemoteControl::LoadFavorites()
{
    std::ifstream in(_favoritesFile);
    if (!in) { return; }
    std::stringstream buffer;
    buffer << in.rdbuf();
    auto paths = ParseStringArray(buffer.str());
    std::lock_guard<std::mutex> lock(_favMutex);
    // Normalize on load: migrates favorites saved with '\' separators by older builds.
    for (const auto& path : paths)
    {
        _favorites.insert(NormalizePathSeparators(path));
    }
}

void RemoteControl::SaveFavorites()
{
    std::string json = FavoritesJson();
    std::ofstream out(_favoritesFile, std::ios::trunc);
    if (!out)
    {
        poco_error_f1(_logger, "Could not write favorites file %s.", _favoritesFile);
        return;
    }
    out << json;
}

std::string RemoteControl::PacksJson() const
{
    std::ostringstream json;
    json << "[";
    std::error_code ec;
    bool first = true;
    if (fs::is_directory(_presetRoot, ec))
    {
        for (const auto& entry : fs::directory_iterator(_presetRoot, ec))
        {
            if (ec) { break; }
            if (!entry.is_directory()) { continue; } // follows symlinks-to-dirs
            if (!first) { json << ","; }
            json << "\"" << JsonEscape(entry.path().filename().string()) << "\"";
            first = false;
        }
    }
    json << "]";
    return json.str();
}
