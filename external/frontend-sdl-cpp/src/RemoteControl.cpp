#include "RemoteControl.h"

#include "AudioCapture.h"
#include "JsonUtil.h"
#include "PresetLists.h"
#include "ProjectMSDLApplication.h"
#include "notifications/PlaybackControlNotification.h"

#include <httplib.h>

#include <Poco/NotificationCenter.h>
#include <Poco/Path.h>
#include <Poco/Util/Application.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <locale>
#include <sstream>
#include <utility>

namespace fs = std::filesystem;
using JsonUtil::EscapeJson;
using PresetLists::NormalizePathSeparators;
using PresetLists::PathFromUtf8;
using PresetLists::PathToUtf8;
using namespace std::chrono_literals;

namespace
{
bool ParseSetting(const std::string& key, const std::string& value, double& number)
{
    if (!PresetLists::IsValidSettingKey(key) || value.empty()) { return false; }
    if (key == "tintColor")
    {
        float r, g, b;
        return PresetLists::ParseHexColor(value, r, g, b);
    }

    const bool boolean = key == "hardCut" || key == "aspectCorrection" || key == "reduceFlashing" ||
                         key == "tintEnabled" || key == "autoskipEnabled" || key == "livePreview";
    if (boolean && (value == "true" || value == "false"))
    {
        number = value == "true" ? 1.0 : 0.0;
        return true;
    }

    char* end = nullptr;
    number = std::strtod(value.c_str(), &end);
    if (end != value.data() + value.size() || !std::isfinite(number)) { return false; }
    number = boolean ? (number != 0.0 ? 1.0 : 0.0) : PresetLists::ClampSettingValue(key, number);
    return true;
}

template <typename Integer>
bool ParseIndex(std::string_view text, Integer& index)
{
    if (text.empty()) { return false; }
    const auto end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, index);
    return result.ec == std::errc{} && result.ptr == end;
}

bool EtagMatches(std::string_view header, std::string_view etag)
{
    for (;;)
    {
        const auto comma = header.find(',');
        auto tag = header.substr(0, comma);
        const auto first = tag.find_first_not_of(" \t");
        if (first != tag.npos)
        {
            tag = tag.substr(first, tag.find_last_not_of(" \t") - first + 1);
            if (tag.substr(0, 2) == "W/") { tag.remove_prefix(2); }
            if (tag == "*" || tag == etag) { return true; }
        }
        if (comma == header.npos) { return false; }
        header.remove_prefix(comma + 1);
    }
}

std::string SerializeFavorites(const std::set<std::string>& favorites)
{
    std::string json{"["};
    bool first = true;
    for (const auto& path : favorites)
    {
        if (!first) { json += ','; }
        json += '"';
        JsonUtil::AppendJsonEscaped(json, path);
        json += '"';
        first = false;
    }
    json += ']';
    return json;
}
}

RemoteControl::RemoteControl() = default;
RemoteControl::~RemoteControl()
{
    uninitialize();
}

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
    _presetRoot = NormalizePathSeparators(config->getString("presetRoot", dataDir + "presets"));
    if (!_presetRoot.empty() && _presetRoot.back() != '/') { _presetRoot += '/'; }
    _webRoot = config->getString("webRoot", dataDir + "remote");
    _favoritesFile = config->getString("favoritesFile", dataDir + "favorites.json");
    _workshopDir = PathFromUtf8(config->getString("workshopDir", dataDir + "workshop"));

    if (_token.empty())
    {
        poco_warning(_logger, "Remote control is running WITHOUT a token: anyone on the network can "
                              "control Dropkick and upload presets. Set remote.token "
                              "(DROPKICK_REMOTE_TOKEN) to require authentication.");
    }

    LoadFavorites();
    RefreshAudioDevices(app.getSubsystem<AudioCapture>());
    RefreshSettings();
    RefreshPresetCache(app.getSubsystem<ProjectMWrapper>());

    _server = std::make_unique<httplib::Server>();
    // Presets are small text files; cap request bodies so an unauthenticated client can't
    // exhaust memory/disk via /api/workshop/apply or /save.
    _server->set_payload_max_length(1 * 1024 * 1024); // 1 MiB
    RegisterRoutes();

    _monitorStopping = false;
    _monitorThread = std::thread(&RemoteControl::Monitor, this);
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
    {
        std::lock_guard<std::mutex> lock(_monitorMutex);
        _monitorStopping = true;
    }
    _monitorWake.notify_one();
    if (_monitorThread.joinable()) { _monitorThread.join(); }
    if (_serverThread.joinable())
    {
        _server->wait_until_ready();
        _server->stop();
        _serverThread.join();
    }
    SaveFavorites();
    _server.reset();
}

bool RemoteControl::Authorized(const std::string& token) const
{
    return _token.empty() || PresetLists::ConstantTimeEquals(token, _token);
}

void RemoteControl::Enqueue(Command command)
{
    std::lock_guard<std::mutex> lock(_queueMutex);
    _queue.push_back(std::move(command));
}

void RemoteControl::RegisterRoutes()
{
    _server->set_post_routing_handler([](const httplib::Request& req, httplib::Response& res) {
        if (req.path.rfind("/api/", 0) == 0) { res.set_header("Cache-Control", "no-store"); }
    });

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

    if (!_token.empty())
    {
        // Short-URL entry: http://<host>:<port>/<token> loads the app, which takes
        // the token from the path (see app.js) — much easier to type on a phone
        // than a ?token= query string.
        _server->set_pre_routing_handler([this, tokenPath = "/" + _token, tokenDir = "/" + _token + "/"]
                                        (const httplib::Request& req, httplib::Response& res) {
            using HandlerResponse = httplib::Server::HandlerResponse;
            if (req.method != "GET") { return HandlerResponse::Unhandled; }
            if (PresetLists::ConstantTimeEquals(req.path, tokenPath))
            {
                res.set_header("Cache-Control", "no-store");
                std::ifstream in(PathFromUtf8(_webRoot + "/index.html"), std::ios::binary);
                if (!in)
                {
                    res.status = 500;
                    res.set_content("index.html missing", "text/plain");
                    return HandlerResponse::Handled;
                }
                std::stringstream buffer;
                buffer << in.rdbuf();
                res.set_content(buffer.str(), "text/html; charset=utf-8");
                return HandlerResponse::Handled;
            }
            if (PresetLists::ConstantTimeEquals(req.path, tokenDir))
            {
                res.status = 302;
                res.set_header("Location", tokenPath);
                res.set_header("Cache-Control", "no-store");
                return HandlerResponse::Handled;
            }
            return HandlerResponse::Unhandled;
        });
    }

    getJson("/api/status", &RemoteControl::StatusJson);
    getJson("/api/packs", &RemoteControl::PacksJson);
    getJson("/api/settings", &RemoteControl::SettingsJson);
    _server->Get("/api/preview", [this, guard](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) { return; }

        std::lock_guard<std::mutex> lock(_previewMutex);
        if (!_livePreview)
        {
            _previewFrame.clear();
            res.status = 204;
            res.set_header("X-Dropkick-Preview", "disabled");
            return;
        }

        _lastPreviewRequest = Clock::now();
        if (!_previewFrame.empty() && _lastPreviewRequest - _lastPreviewCapture <= 5s)
        {
            res.status = 200;
            res.set_header("X-Dropkick-Frame", std::to_string(_previewSeq));
            res.set_content(reinterpret_cast<const char*>(_previewFrame.data()), _previewFrame.size(), "image/bmp");
            return;
        }
        res.status = 204;
        res.set_header("X-Dropkick-Preview", "warming");
    });
    getJson("/api/favorites", &RemoteControl::FavoritesJson);
    getJson("/api/audio/devices", &RemoteControl::AudioDevicesJson);

    _server->Get("/api/presets", [this, guard](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) { return; }
        std::lock_guard<std::mutex> lock(_dataMutex);
        res.set_header("ETag", _presetsEtag);
        if (EtagMatches(req.get_header_value("If-None-Match"), _presetsEtag))
        {
            res.status = 304;
            return;
        }
        res.set_content(_presetsJson, "application/json");
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
        bool on;
        {
            std::lock_guard<std::mutex> lock(_catMutex);
            on = !_favShuffle.load();
            if (on)
            {
                _catShuffle = false;
                _catShufflePrefix.clear();
            }
            _favShuffle = on;
        }
        res.set_content(std::string("{\"ok\":true,\"favoritesShuffle\":") + (on ? "true" : "false") + "}", "application/json");
    });

    _server->Post("/api/category/shuffle", [this, guard](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) { return; }
        std::string path = NormalizePathSeparators(req.get_param_value("path"));
        if (path.empty() || path.find("..") != std::string::npos)
        {
            res.status = 400;
            res.set_content("{\"error\":\"invalid category path\"}", "application/json");
            return;
        }
        if (path.back() != '/') { path += '/'; }
        if (path.rfind(_presetRoot, 0) != 0)
        {
            res.status = 400;
            res.set_content("{\"error\":\"invalid category path\"}", "application/json");
            return;
        }
        bool on;
        {
            std::lock_guard<std::mutex> lock(_catMutex);
            if (_catShufflePrefix == path) { _catShufflePrefix.clear(); on = false; } // toggle off
            else { _catShufflePrefix = path; on = true; }
            if (on) { _favShuffle = false; }
            _catShuffle = on;
        }
        if (on)
        {
            Enqueue(Command{CommandType::Random, "", ""}); // jump into the category right away
        }
        res.set_content(std::string("{\"ok\":true,\"categoryShuffle\":") + (on ? "true" : "false") + "}", "application/json");
    });

    _server->Post("/api/preset", [this, guard](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) { return; }
        std::string index = req.get_param_value("index");
        uint32_t parsedIndex;
        if (!ParseIndex(index, parsedIndex))
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
        double number = 0.0;
        if (!ParseSetting(key, value, number))
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

    _server->Post("/api/audio/select", [this, guard](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) { return; }
        int index = 0;
        bool available = false;
        if (ParseIndex(req.get_param_value("index"), index))
        {
            std::lock_guard<std::mutex> lock(_statusMutex);
            available = std::find(_audioDeviceIds.begin(), _audioDeviceIds.end(), index) != _audioDeviceIds.end();
        }
        if (!available)
        {
            res.status = 400;
            res.set_content("{\"error\":\"invalid index\"}", "application/json");
            return;
        }
        Enqueue(Command{CommandType::SelectAudio, "", "", index});
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
        std::ifstream in(PathFromUtf8(path), std::ios::binary);
        if (in)
        {
            std::stringstream buf;
            buf << in.rdbuf();
            text = buf.str();
        }
        std::ostringstream json;
        json << "{\"path\":\"" << EscapeJson(path) << "\",\"text\":\"" << EscapeJson(text) << "\"}";
        res.set_content(json.str(), "application/json");
    });

    _server->Post("/api/workshop/apply", [this, guard](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) { return; }
        const auto scratch = _workshopDir / "_scratch.milk";
        std::lock_guard<std::mutex> lock(_workshopMutex);
        if (_workshopDir.empty() ||
            PresetLists::WriteFileAtomic(scratch, req.body, true) != PresetLists::WriteFileResult::Written)
        {
            res.status = 500;
            res.set_content("{\"error\":\"cannot write scratch\"}", "application/json");
            return;
        }
        RememberWorkshopFile(scratch);
        Enqueue(Command{CommandType::LoadWorkshopPath, PathToUtf8(scratch), ""});
        res.set_content("{\"ok\":true}", "application/json");
    });

    _server->Post("/api/workshop/save", [this, guard](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) { return; }
        std::string name = req.get_param_value("name");
        // sanitize: strip any directory parts and enforce .milk
        auto slash = name.find_last_of("/\\");
        if (slash != std::string::npos) { name = name.substr(slash + 1); }
        const bool validName = PresetLists::IsValidWorkshopFilename(name);
        if (name.size() < 5 || name.compare(name.size() - 5, 5, ".milk") != 0) { name += ".milk"; }
        const auto filename = PathFromUtf8(name);
        if (!validName || filename.empty())
        {
            res.status = 400;
            res.set_content("{\"error\":\"invalid name\"}", "application/json");
            return;
        }
        const auto overwrite = req.get_param_value("overwrite");
        const auto dest = _workshopDir / filename;
        std::lock_guard<std::mutex> lock(_workshopMutex);
        using PresetLists::WriteFileResult;
        const auto result = _workshopDir.empty() ? WriteFileResult::Failed :
            PresetLists::WriteFileAtomic(dest, req.body, overwrite != "0" && overwrite != "false");
        if (result == WriteFileResult::Exists)
        {
            res.status = 409;
            res.set_content("{\"error\":\"exists\"}", "application/json");
            return;
        }
        if (result != WriteFileResult::Written)
        {
            res.status = 500;
            res.set_content("{\"error\":\"cannot save\"}", "application/json");
            return;
        }
        res.set_content(std::string("{\"ok\":true,\"file\":\"") + EscapeJson(name) + "\"}", "application/json");
    });

    _server->Post("/api/pack", [this, guard](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) { return; }
        std::string pack = req.get_param_value("name");
        if (pack.empty() || pack == "." || pack.find("..") != std::string::npos ||
            pack.find_first_of("/\\") != std::string::npos || fs::path(pack).has_root_path())
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
    auto& app = ProjectMSDLApplication::instance();
    auto& wrapper = app.getSubsystem<ProjectMWrapper>();
    auto& audio = app.getSubsystem<AudioCapture>();
    RefreshPresetCache(wrapper);
    if (audio.AudioDeviceIndex() != _audioDeviceIndex) { RefreshAudioDevices(audio); }

    std::vector<Command> pending;
    {
        std::lock_guard<std::mutex> lock(_queueMutex);
        if (_queue.empty()) { return; }
        pending.swap(_queue);
    }

    auto& center = Poco::NotificationCenter::defaultCenter();
    for (const auto& command : pending)
    {
        switch (command.type)
        {
            case CommandType::Next:
                _workshopActive = false;
                if (_favShuffle.load()) { JumpToFavorite(true); }
                else if (_catShuffle.load()) { JumpToCategory(); }
                else
                {
                    center.postNotification(new PlaybackControlNotification(PlaybackControlNotification::Action::NextPreset));
                }
                break;
            case CommandType::Previous:
                _workshopActive = false;
                // History-based: go to what actually played before (matters with shuffle),
                // not the previous playlist position. Falls back to position-previous when
                // the history is empty.
                center.postNotification(new PlaybackControlNotification(PlaybackControlNotification::Action::LastPreset));
                break;
            case CommandType::Random:
                _workshopActive = false;
                if (_favShuffle.load()) { JumpToFavorite(false); }
                else if (_catShuffle.load()) { JumpToCategory(); }
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
                audio.NextAudioDevice();
                RefreshAudioDevices(audio);
                break;
            case CommandType::SelectAudio:
                audio.AudioDeviceIndex(command.audioIndex);
                RefreshAudioDevices(audio);
                break;
            case CommandType::LoadPack:
            {
                const auto revision = wrapper.PlaylistRevision();
                wrapper.LoadPresetPack((fs::path(_presetRoot) / command.arg).string());
                if (wrapper.PlaylistRevision() != revision)
                {
                    std::lock_guard<std::mutex> lock(_catMutex);
                    _catShufflePrefix.clear();
                    _catShuffle = false;
                }
                break;
            }
            case CommandType::SetPosition:
            {
                auto playlist = wrapper.Playlist();
                if (playlist)
                {
                    uint32_t index;
                    if (ParseIndex(command.arg, index) && index < projectm_playlist_size(playlist))
                    {
                        _workshopActive = false;
                        // Explicitly picking a preset outside the shuffled category ends
                        // category shuffle — otherwise the enforcement would yank it away.
                        if (_catShuffle.load())
                        {
                            auto* item = projectm_playlist_item(playlist, index);
                            if (item != nullptr)
                            {
                                const std::string itemPath = NormalizePathSeparators(item);
                                projectm_playlist_free_string(item);
                                std::lock_guard<std::mutex> lock(_catMutex);
                                if (itemPath.rfind(_catShufflePrefix, 0) != 0)
                                {
                                    _catShufflePrefix.clear();
                                    _catShuffle = false;
                                }
                            }
                        }
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
                wrapper.ClearBlocklist();
                break;
            case CommandType::LoadWorkshopPath:
                wrapper.LoadPresetFile(command.arg);
                _workshopActive = true;
                _workshopPath = command.arg;
                _workshopPreset = NormalizePathSeparators(_workshopPath);
                poco_information_f1(_logger, "Workshop: live-loaded %s", command.arg);
                break;
            case CommandType::DislikeCurrent:
                if (!_workshopActive) { wrapper.DislikeCurrent(); }
                break;
            case CommandType::ClearDislikes:
                wrapper.ClearDislikes();
                break;
        }
        RefreshPresetCache(wrapper);
    }
}

void RemoteControl::RefreshPresetCache(ProjectMWrapper& wrapper)
{
    const auto size = projectm_playlist_size(wrapper.Playlist());
    if (_lastPlaylistRevision == wrapper.PlaylistRevision() && _lastPlaylistSize == size) { return; }
    RebuildPresetCache(wrapper);
    _lastPlaylistRevision = wrapper.PlaylistRevision();
    _lastPlaylistSize = size;

    std::lock_guard<std::mutex> lock(_statusMutex);
    _statusSnapshot.catalogRevision = _lastPlaylistRevision;
    _statusDirty = true;
}

void RemoteControl::RebuildPresetCache(ProjectMWrapper& wrapper)
{
    auto items = wrapper.PlaylistItems();
    _pathToIndex.clear();
    _pathToIndex.reserve(items.size());
    _categoryIndices.clear();
    std::string json;
    json.reserve(items.size() * 96);
    json += '[';
    for (uint32_t i = 0; i < items.size(); ++i)
    {
        const auto item = NormalizePathSeparators(std::move(items[i]));
        _pathToIndex[item] = i;
        if (item.rfind(_presetRoot, 0) == 0)
        {
            auto dirEnd = _presetRoot.empty() ? item.find('/') : _presetRoot.size() - 1;
            for (; dirEnd != item.npos; dirEnd = item.find('/', dirEnd + 1))
            {
                _categoryIndices[item.substr(0, dirEnd + 1)].push_back(i);
            }
        }
        if (i) { json += ','; }
        json += "{\"i\":";
        char index[10]; // UINT32_MAX needs ten digits.
        const auto result = std::to_chars(index, index + sizeof(index), i);
        json.append(index, result.ptr);
        json += ",\"p\":\"";
        JsonUtil::AppendJsonEscaped(json, item);
        json += "\"}";
    }
    json += ']';

    {
        std::lock_guard<std::mutex> lock(_dataMutex);
        if (json != _presetsJson || _presetsEtag.empty())
        {
            _presetsJson = std::move(json);
            _presetsEtag = "\"" + std::to_string(_rng()) + "-" + std::to_string(++_presetCacheVersion) + "\"";
        }
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
        target = favoriteIndices[std::uniform_int_distribution<size_t>(0, favoriteIndices.size() - 1)(_rng)];
    }
    projectm_playlist_set_position(playlist, target, true);
}

void RemoteControl::JumpToCategory()
{
    auto& wrapper = ProjectMSDLApplication::instance().getSubsystem<ProjectMWrapper>();
    auto playlist = wrapper.Playlist();
    if (!playlist) { return; }
    RefreshPresetCache(wrapper);

    uint32_t target;
    {
        std::lock_guard<std::mutex> lock(_catMutex);
        if (_catShufflePrefix.empty()) { return; }
        const auto category = _categoryIndices.find(_catShufflePrefix);
        if (category == _categoryIndices.end())
        {
            _catShufflePrefix.clear();
            _catShuffle = false;
            return;
        }
        const auto& indices = category->second;
        target = indices[std::uniform_int_distribution<size_t>(0, indices.size() - 1)(_rng)];
    }
    projectm_playlist_set_position(playlist, target, true);
}

bool RemoteControl::WantsPreview() const
{
    std::lock_guard<std::mutex> lock(_previewMutex);
    if (!_livePreview || _lastPreviewRequest == Clock::time_point{}) { return false; }

    const auto now = Clock::now();
    return now - _lastPreviewRequest <= 3s &&
           (_previewSeq == 0 || now - _lastPreviewCapture >= 160ms);
}

void RemoteControl::PublishPreviewFrame(std::vector<uint8_t> bmp)
{
    std::lock_guard<std::mutex> lock(_previewMutex);
    if (!_livePreview) { return; }

    _previewFrame = std::move(bmp);
    ++_previewSeq;
    _lastPreviewCapture = Clock::now();
}

void RemoteControl::ApplySetting(const std::string& key, const std::string& value)
{
    double number = 0.0;
    if (!ParseSetting(key, value, number)) { return; }

    auto config = ProjectMSDLApplication::instance().UserConfiguration();
    if (key == "presetDuration") { config->setDouble("projectM.displayDuration", number); }
    else if (key == "softCutDuration") { config->setDouble("projectM.transitionDuration", number); }
    else if (key == "hardCut") { config->setBool("projectM.hardCutsEnabled", number != 0.0); }
    else if (key == "aspectCorrection") { config->setBool("projectM.aspectCorrectionEnabled", number != 0.0); }
    else if (key == "tintColor") { config->setString("projectM.tintColor", value); }
    else if (key == "fps" || key == "autoskipStrikes") { config->setInt("projectM." + key, static_cast<int>(number)); }
    else if (key == "reduceFlashing" || key == "tintEnabled" || key == "autoskipEnabled" || key == "livePreview")
    {
        config->setBool("projectM." + key, number != 0.0);
    }
    else
    {
        config->setDouble("projectM." + key, number);
    }
    RefreshSettings();
}

void RemoteControl::Monitor()
{
    std::unique_lock<std::mutex> lock(_monitorMutex);
    while (!_monitorStopping)
    {
        const auto next = Clock::now() + 1s;
        lock.unlock();
        const auto stats = _systemMonitor.Sample();
        {
            std::lock_guard<std::mutex> statusLock(_statusMutex);
            _systemStats = stats;
            _statusDirty = true;
        }
        PollWorkshop();
        SaveFavorites();
        lock.lock();
        _monitorWake.wait_until(lock, next, [this] { return _monitorStopping; });
    }
}

void RemoteControl::PollWorkshop()
{
    std::lock_guard<std::mutex> lock(_workshopMutex);
    std::error_code ec;
    fs::directory_iterator entry(_workshopDir, ec);
    const fs::directory_iterator end;
    if (ec == std::errc::no_such_file_or_directory)
    {
        _workshopSeen.clear();
        _workshopSeeded = true;
        return;
    }
    if (ec) { return; }

    decltype(_workshopSeen) seen;
    std::string newest;
    fs::file_time_type newestMtime{};
    for (; entry != end; entry.increment(ec))
    {
        if (ec) { return; }
        std::error_code entryError;
        if (entry->path().extension() != ".milk" || !entry->is_regular_file(entryError)) { continue; }
        const auto mtime = entry->last_write_time(entryError);
        if (entryError) { continue; }

        auto path = NormalizePathSeparators(PathToUtf8(entry->path()));
        const auto previous = _workshopSeen.find(path);
        if ((previous == _workshopSeen.end() || previous->second != mtime) && (newest.empty() || mtime >= newestMtime))
        {
            newestMtime = mtime;
            newest = path;
        }
        seen.emplace(std::move(path), mtime);
    }
    if (ec) { return; }

    _workshopSeen.swap(seen);
    if (_workshopSeeded && !newest.empty())
    {
        Enqueue(Command{CommandType::LoadWorkshopPath, std::move(newest), ""});
    }
    _workshopSeeded = true;
}

void RemoteControl::RememberWorkshopFile(const fs::path& path)
{
    std::error_code ec;
    const auto mtime = fs::last_write_time(path, ec);
    if (!ec) { _workshopSeen[NormalizePathSeparators(PathToUtf8(path))] = mtime; }
}

void RemoteControl::CaptureToWorkshop()
{
    auto& wrapper = ProjectMSDLApplication::instance().getSubsystem<ProjectMWrapper>();
    const auto source = PathFromUtf8(wrapper.CurrentStatus().presetName);
    if (source.empty() || _workshopDir.empty()) { return; }

    std::string dest;
    {
        std::lock_guard<std::mutex> lock(_workshopMutex);
        std::error_code ec;
        fs::create_directories(_workshopDir, ec);
        auto target = _workshopDir / source.filename();
        for (int n = 1; !ec && fs::exists(target, ec); ++n)
        {
            auto filename = source.stem();
            filename += " (" + std::to_string(n) + ")";
            filename += source.extension();
            target = _workshopDir / filename;
        }

        if (ec || !fs::copy_file(source, target, ec))
        {
            poco_error_f1(_logger, "Workshop capture failed for %s", PathToUtf8(source));
            return;
        }
        dest = PathToUtf8(target);
        RememberWorkshopFile(target);
    }
    wrapper.LoadPresetFile(dest);
    _workshopActive = true;
    _workshopPath = dest;
    _workshopPreset = NormalizePathSeparators(dest);
    poco_information_f1(_logger, "Workshop: captured current preset to %s", dest);
}

void RemoteControl::PublishStatus(const ProjectMWrapper::PlaybackStatus& status, float fps)
{
    if (_currentPath != status.presetName)
    {
        _currentPath = status.presetName;
        _currentPreset = NormalizePathSeparators(_currentPath);
    }
    const auto& preset = _workshopActive ? _workshopPreset : _currentPreset;
    const auto& editPath = _workshopActive ? _workshopPath : _currentPath;

    auto& wrapper = ProjectMSDLApplication::instance().getSubsystem<ProjectMWrapper>();
    const auto blocked = wrapper.BlockedCount();
    const auto disliked = wrapper.DislikedCount();
    const auto now = Clock::now();
    if (now >= _nextSettingsSample) { RefreshSettings(); }

    std::unique_lock<std::mutex> categoryLock;
    std::string_view category;
    if (_catShuffle.load())
    {
        categoryLock = std::unique_lock<std::mutex>(_catMutex);
        category = _catShufflePrefix;
    }

    // Only the render thread writes this snapshot. HTTP readers take _statusMutex.
    const auto& previous = _statusSnapshot;
    bool favorited = previous.favorited;
    const auto favoriteRevision = _favoritesRevision.load();
    if (previous.preset != preset || favoriteRevision != _lastFavoritesRevision)
    {
        std::lock_guard<std::mutex> lock(_favMutex);
        favorited = _favorites.count(preset) > 0;
        _lastFavoritesRevision = _favoritesRevision.load();
    }

    const bool favoritesShuffle = _favShuffle.load();
    const bool sampleFps = now >= _nextFpsSample;
    if (sampleFps || previous.position != status.position || previous.size != status.playlistSize ||
        previous.shuffle != status.shuffle || previous.locked != status.locked ||
        previous.preset != preset || previous.category != category || previous.workshop != _workshopActive ||
        previous.favorited != favorited || previous.favoritesShuffle != favoritesShuffle ||
        previous.blocked != blocked || previous.disliked != disliked ||
        _editPath != editPath)
    {
        std::lock_guard<std::mutex> lock(_statusMutex);
        auto& snapshot = _statusSnapshot;
        snapshot.preset = preset;
        snapshot.category = category;
        snapshot.position = status.position;
        snapshot.size = status.playlistSize;
        snapshot.blocked = blocked;
        snapshot.disliked = disliked;
        snapshot.shuffle = status.shuffle;
        snapshot.locked = status.locked;
        snapshot.favorited = favorited;
        snapshot.favoritesShuffle = favoritesShuffle;
        snapshot.workshop = _workshopActive;
        if (sampleFps)
        {
            snapshot.fps = static_cast<int>(std::lround(fps));
            _nextFpsSample = now + 250ms;
        }
        _editPath = editPath;
        _statusDirty = true;
    }

    // ProjectM can auto-advance outside the chosen category on preset expiry.
    if (!category.empty() && !_workshopActive && !preset.empty() && preset.rfind(category, 0) != 0)
    {
        categoryLock.unlock();
        JumpToCategory();
    }
}

void RemoteControl::RefreshSettings()
{
    auto& app = ProjectMSDLApplication::instance();
    auto& wrapper = app.getSubsystem<ProjectMWrapper>();
    const auto pm = wrapper.ProjectM();
    if (!pm) { return; }

    const auto& config = app.config();
    SettingsSnapshot settings{
        projectm_get_preset_duration(pm),
        projectm_get_soft_cut_duration(pm),
        projectm_get_hard_cut_enabled(pm),
        projectm_get_hard_cut_duration(pm),
        projectm_get_hard_cut_sensitivity(pm),
        projectm_get_beat_sensitivity(pm),
        wrapper.TargetFPS(),
        projectm_get_aspect_correction(pm),
        config.getBool("projectM.reduceFlashing", false),
        config.getDouble("projectM.flashStrength", 0.6),
        config.getDouble("projectM.brightness", 1.0),
        config.getBool("projectM.tintEnabled", false),
        config.getString("projectM.tintColor", "#00ff00"),
        config.getDouble("projectM.tintStrength", 1.0),
        config.getBool("projectM.autoskipEnabled", true),
        config.getDouble("projectM.autoskipFps", 20.0),
        config.getInt("projectM.autoskipStrikes", 3),
        config.getBool("projectM.livePreview", true)
    };

    std::scoped_lock lock(_statusMutex, _previewMutex);
    _livePreview = settings.livePreview;
    if (!_livePreview)
    {
        _previewFrame.clear();
        _lastPreviewRequest = {};
    }
    _settingsSnapshot = std::move(settings);
    _settingsDirty = true;
    _nextSettingsSample = Clock::now() + 1s;
}

std::string RemoteControl::StatusJson() const
{
    std::lock_guard<std::mutex> lock(_statusMutex);
    if (_statusDirty)
    {
        const auto& status = _statusSnapshot;
        const auto& stats = _systemStats;
        std::ostringstream json;
        json.imbue(std::locale::classic());
        json << std::boolalpha
             << "{\"preset\":\"" << EscapeJson(status.preset) << "\""
             << ",\"position\":" << status.position
             << ",\"size\":" << status.size
             << ",\"catalogRevision\":" << status.catalogRevision
             << ",\"shuffle\":" << status.shuffle
             << ",\"locked\":" << status.locked
             << ",\"favorited\":" << status.favorited
             << ",\"favoritesShuffle\":" << status.favoritesShuffle
             << ",\"categoryShuffle\":\"" << EscapeJson(status.category) << "\""
             << ",\"workshop\":" << status.workshop
             << ",\"blocked\":" << status.blocked
             << ",\"disliked\":" << status.disliked
             << ",\"fps\":" << status.fps
             << ",\"cpu\":" << std::lround(stats.cpuPct)
             << ",\"memUsed\":" << stats.memUsedMB
             << ",\"memTotal\":" << stats.memTotalMB
             << ",\"temp\":" << std::lround(stats.tempC)
             << ",\"hasTemp\":" << stats.hasTemp
             << ",\"audioId\":" << status.audioId
             << ",\"audio\":\"" << EscapeJson(status.audio) << "\"}";
        _statusJson = json.str();
        _statusDirty = false;
    }
    return _statusJson;
}

std::string RemoteControl::SettingsJson() const
{
    std::lock_guard<std::mutex> lock(_statusMutex);
    if (_settingsDirty)
    {
        const auto& settings = _settingsSnapshot;
        std::ostringstream json;
        json.imbue(std::locale::classic());
        json << std::boolalpha
             << "{\"presetDuration\":" << settings.presetDuration
             << ",\"softCutDuration\":" << settings.softCutDuration
             << ",\"hardCut\":" << settings.hardCut
             << ",\"hardCutDuration\":" << settings.hardCutDuration
             << ",\"hardCutSensitivity\":" << settings.hardCutSensitivity
             << ",\"beatSensitivity\":" << settings.beatSensitivity
             << ",\"fps\":" << settings.fps
             << ",\"aspectCorrection\":" << settings.aspectCorrection
             << ",\"reduceFlashing\":" << settings.reduceFlashing
             << ",\"flashStrength\":" << settings.flashStrength
             << ",\"brightness\":" << settings.brightness
             << ",\"tintEnabled\":" << settings.tintEnabled
             << ",\"tintColor\":\"" << EscapeJson(settings.tintColor) << "\""
             << ",\"tintStrength\":" << settings.tintStrength
             << ",\"autoskipEnabled\":" << settings.autoskipEnabled
             << ",\"autoskipFps\":" << settings.autoskipFps
             << ",\"autoskipStrikes\":" << settings.autoskipStrikes
             << ",\"livePreview\":" << settings.livePreview << "}";
        _settingsJson = json.str();
        _settingsDirty = false;
    }
    return _settingsJson;
}

void RemoteControl::RefreshAudioDevices(AudioCapture& audio)
{
    _audioDeviceIndex = audio.AudioDeviceIndex();
    const auto devices = audio.AudioDeviceList();
    const auto current = devices.find(_audioDeviceIndex);

    std::string json = "{\"current\":" + std::to_string(_audioDeviceIndex) + ",\"devices\":[";
    std::vector<int> ids;
    ids.reserve(devices.size());
    bool first = true;
    for (const auto& device : devices)
    {
        ids.push_back(device.first);
        if (!first) { json += ','; }
        json += "{\"id\":";
        json += std::to_string(device.first);
        json += ",\"name\":\"";
        JsonUtil::AppendJsonEscaped(json, device.second);
        json += "\"}";
        first = false;
    }
    json += "]}";

    std::lock_guard<std::mutex> lock(_statusMutex);
    _audioDevicesJson = std::move(json);
    _audioDeviceIds = std::move(ids);
    _statusSnapshot.audioId = _audioDeviceIndex;
    _statusSnapshot.audio = current != devices.end() ? current->second : "";
    _statusDirty = true;
}

std::string RemoteControl::AudioDevicesJson() const
{
    std::lock_guard<std::mutex> lock(_statusMutex);
    return _audioDevicesJson;
}

std::string RemoteControl::FavoritesJson() const
{
    std::lock_guard<std::mutex> lock(_favMutex);
    return SerializeFavorites(_favorites);
}

bool RemoteControl::ToggleFavorite(const std::string& path)
{
    const auto normalized = NormalizePathSeparators(path);
    std::lock_guard<std::mutex> lock(_favMutex);
    const bool nowFavorite = _favorites.erase(normalized) == 0;
    if (nowFavorite) { _favorites.insert(normalized); }
    ++_favoritesRevision;
    _favoritesDirty = true;
    return nowFavorite;
}

void RemoteControl::LoadFavorites()
{
    std::ifstream in(PathFromUtf8(_favoritesFile));
    if (!in) { return; }
    std::stringstream buffer;
    buffer << in.rdbuf();
    for (auto& path : JsonUtil::ParseJsonStringArray(buffer.str()))
    {
        _favorites.insert(NormalizePathSeparators(std::move(path)));
    }
}

void RemoteControl::SaveFavorites()
{
    std::set<std::string> favorites;
    {
        std::lock_guard<std::mutex> lock(_favMutex);
        if (!_favoritesDirty) { return; }
        favorites = _favorites;
        _favoritesDirty = false;
    }
    if (!PresetLists::WriteFileAtomic(_favoritesFile, SerializeFavorites(favorites)))
    {
        {
            std::lock_guard<std::mutex> lock(_favMutex);
            _favoritesDirty = true;
        }
        poco_error_f1(_logger, "Could not write favorites file %s.", _favoritesFile);
    }
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
            json << "\"" << EscapeJson(entry.path().filename().string()) << "\"";
            first = false;
        }
    }
    json << "]";
    return json.str();
}
