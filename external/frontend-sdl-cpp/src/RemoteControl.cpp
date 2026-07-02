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
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>

namespace
{
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
    _presetRoot = config->getString("presetRoot",
                     Poco::Path::expand("~/.local/share/dropkick/presets"));
    _webRoot = config->getString("webRoot",
                     Poco::Path::expand("~/.local/share/dropkick/remote"));
    _favoritesFile = config->getString("favoritesFile",
                     Poco::Path::expand("~/.local/share/dropkick/favorites.json"));
    _workshopDir = config->getString("workshopDir",
                     Poco::Path::expand("~/.local/share/dropkick/workshop"));

    LoadFavorites();

    _server = std::make_unique<httplib::Server>();
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
    return _token.empty() || token == _token;
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

    _server->set_mount_point("/", _webRoot); // serves index.html, app.js, style.css

    _server->Get("/api/status", [this, guard](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) { return; }
        res.set_content(StatusJson(), "application/json");
    });

    _server->Get("/api/packs", [this, guard](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) { return; }
        res.set_content(PacksJson(), "application/json");
    });

    _server->Get("/api/presets", [this, guard](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) { return; }
        res.set_content(PresetsJson(), "application/json");
    });

    _server->Get("/api/settings", [this, guard](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) { return; }
        res.set_content(SettingsJson(), "application/json");
    });

    _server->Get("/api/favorites", [this, guard](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) { return; }
        res.set_content(FavoritesJson(), "application/json");
    });

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
            "reduceFlashing", "flashStrength"};
        if (!kKeys.count(key) || value.empty())
        {
            res.status = 400;
            res.set_content("{\"error\":\"invalid setting\"}", "application/json");
            return;
        }
        Enqueue(Command{CommandType::SetSetting, key, value});
        res.set_content("{\"ok\":true}", "application/json");
    });

    auto post = [this, guard](const char* path, CommandType type) {
        _server->Post(path, [this, guard, type](const httplib::Request& req, httplib::Response& res) {
            if (!guard(req, res)) { return; }
            Enqueue(Command{type, "", ""});
            res.set_content("{\"ok\":true}", "application/json");
        });
    };

    _server->Post("/api/workshop/capture", [this, guard](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) { return; }
        Enqueue(Command{CommandType::CaptureWorkshop, "", ""});
        res.set_content("{\"ok\":true}", "application/json");
    });

    _server->Post("/api/blocklist/clear", [this, guard](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) { return; }
        Enqueue(Command{CommandType::ClearBlocklist, "", ""});
        res.set_content("{\"ok\":true}", "application/json");
    });

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
        ::mkdir(_workshopDir.c_str(), 0755);
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
        ::mkdir(_workshopDir.c_str(), 0755);
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

    post("/api/next", CommandType::Next);
    post("/api/prev", CommandType::Previous);
    post("/api/random", CommandType::Random);
    post("/api/shuffle", CommandType::ToggleShuffle);
    post("/api/lock", CommandType::ToggleLock);
    post("/api/audio/next", CommandType::NextAudio);

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
        _pathToIndex[items[i]] = i;
        if (i) { json << ","; }
        json << "{\"i\":" << i << ",\"p\":\"" << JsonEscape(items[i]) << "\"}";
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
}

void RemoteControl::PollWorkshop()
{
    long now = static_cast<long>(::time(nullptr));
    if (now == _lastWorkshopPoll) { return; } // throttle to ~1 Hz
    _lastWorkshopPoll = now;

    DIR* dir = opendir(_workshopDir.c_str());
    if (!dir) { return; }

    std::string newest;
    long newestMtime = 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr)
    {
        std::string nm = entry->d_name;
        if (nm.size() < 5 || nm.compare(nm.size() - 5, 5, ".milk") != 0) { continue; }
        std::string full = _workshopDir + "/" + nm;
        struct stat st{};
        if (stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) { continue; }
        long mtime = static_cast<long>(st.st_mtime);

        auto it = _workshopSeen.find(full);
        bool changed = (it == _workshopSeen.end() || it->second != mtime);
        _workshopSeen[full] = mtime;
        if (changed && mtime >= newestMtime)
        {
            newestMtime = mtime;
            newest = full;
        }
    }
    closedir(dir);

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

    ::mkdir(_workshopDir.c_str(), 0755); // no-op if it exists

    std::string base = _currentPath.substr(_currentPath.find_last_of('/') + 1);
    std::string stem = base;
    std::string ext;
    auto dot = base.rfind(".milk");
    if (dot != std::string::npos) { stem = base.substr(0, dot); ext = ".milk"; }

    std::string dest = _workshopDir + "/" + stem + ext;
    struct stat st{};
    int n = 1;
    while (stat(dest.c_str(), &st) == 0)
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
    struct stat dst{};
    if (stat(dest.c_str(), &dst) == 0) { _workshopSeen[dest] = static_cast<long>(dst.st_mtime); }
    ProjectMSDLApplication::instance().getSubsystem<ProjectMWrapper>().LoadPresetFile(dest);
    _workshopActive = true;
    _workshopPath = dest;
    poco_information_f1(_logger, "Workshop: captured current preset to %s", dest);
}

void RemoteControl::PublishStatus(const ProjectMWrapper::PlaybackStatus& status, const std::string& audioDevice)
{
    _currentPath = status.presetName;
    std::string reportedPreset = _workshopActive ? _workshopPath : status.presetName;
    bool favorited;
    {
        std::lock_guard<std::mutex> lock(_favMutex);
        favorited = _favorites.count(reportedPreset) > 0;
    }

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
         << "\"blocked\":" << ProjectMSDLApplication::instance().getSubsystem<ProjectMWrapper>().BlockedCount() << ","
         << "\"audio\":\"" << JsonEscape(audioDevice) << "\""
         << "}";

    // Settings snapshot (render thread — safe to query projectM here).
    std::ostringstream settings;
    auto pm = ProjectMSDLApplication::instance().getSubsystem<ProjectMWrapper>().ProjectM();
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
                 << "\"reduceFlashing\":" << (ProjectMSDLApplication::instance().config().getBool("projectM.reduceFlashing", false) ? "true" : "false") << ","
                 << "\"flashStrength\":" << ProjectMSDLApplication::instance().config().getDouble("projectM.flashStrength", 0.6)
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
    bool nowFavorite;
    {
        std::lock_guard<std::mutex> lock(_favMutex);
        auto it = _favorites.find(path);
        if (it != _favorites.end()) { _favorites.erase(it); nowFavorite = false; }
        else { _favorites.insert(path); nowFavorite = true; }
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
    _favorites.insert(paths.begin(), paths.end());
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
    DIR* dir = opendir(_presetRoot.c_str());
    bool first = true;
    if (dir)
    {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr)
        {
            std::string entryName = entry->d_name;
            if (entryName == "." || entryName == "..") { continue; }
            std::string full = _presetRoot + "/" + entryName;
            struct stat st{};
            if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
            {
                if (!first) { json << ","; }
                json << "\"" << JsonEscape(entryName) << "\"";
                first = false;
            }
        }
        closedir(dir);
    }
    json << "]";
    return json.str();
}
