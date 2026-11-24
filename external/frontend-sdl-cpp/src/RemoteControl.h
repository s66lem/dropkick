#pragma once

#include "ProjectMWrapper.h"
#include "SystemMonitor.h"

#include <Poco/Logger.h>
#include <Poco/Util/Subsystem.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class AudioCapture;
namespace httplib { class Server; }

/** HTTP handlers enqueue commands and serve snapshots. ProjectM and audio access stays on the render thread. */
class RemoteControl : public Poco::Util::Subsystem
{
public:
    enum class CommandType
    {
        Next, Previous, Random, ToggleShuffle, ToggleLock, NextAudio, SelectAudio, LoadPack,
        SetPosition, SetSetting, CaptureWorkshop, ClearBlocklist, LoadWorkshopPath,
        DislikeCurrent, ClearDislikes
    };

    struct Command
    {
        CommandType type;
        std::string arg;
        std::string arg2;
        int audioIndex{0};
    };

    RemoteControl();
    ~RemoteControl() override;

    const char* name() const override;
    void initialize(Poco::Util::Application& app) override;
    void uninitialize() override;

    void DrainCommands();
    bool WantsPreview() const;
    void PublishPreviewFrame(std::vector<uint8_t> bmp);
    void PublishStatus(const ProjectMWrapper::PlaybackStatus& status, float fps);
    void RefreshAudioDevices(AudioCapture& audio);
    bool ToggleFavorite(const std::string& path);

private:
    using Clock = std::chrono::steady_clock;

    struct StatusSnapshot
    {
        std::string preset;
        std::string audio;
        std::string category;
        uint64_t catalogRevision{0};
        uint32_t position{0};
        uint32_t size{0};
        uint32_t blocked{0};
        uint32_t disliked{0};
        int audioId{-1};
        int fps{0};
        bool shuffle{false};
        bool locked{false};
        bool favorited{false};
        bool favoritesShuffle{false};
        bool workshop{false};
    };

    struct SettingsSnapshot
    {
        double presetDuration;
        double softCutDuration;
        bool hardCut;
        double hardCutDuration;
        double hardCutSensitivity;
        double beatSensitivity;
        int fps;
        bool aspectCorrection;
        bool reduceFlashing;
        double flashStrength;
        double brightness;
        bool tintEnabled;
        std::string tintColor;
        double tintStrength;
        bool autoskipEnabled;
        double autoskipFps;
        int autoskipStrikes;
        bool livePreview{true};
    };

    void RegisterRoutes();
    void Enqueue(Command command);
    bool Authorized(const std::string& token) const;
    std::string StatusJson() const;
    std::string PacksJson() const;
    std::string SettingsJson() const;
    std::string FavoritesJson() const;
    std::string AudioDevicesJson() const;
    void LoadFavorites();
    void SaveFavorites();
    void RefreshPresetCache(ProjectMWrapper& wrapper);
    void RebuildPresetCache(ProjectMWrapper& wrapper);
    void RefreshSettings();
    void ApplySetting(const std::string& key, const std::string& value);
    void JumpToFavorite(bool nextInOrder);
    void JumpToCategory();
    void Monitor();
    void PollWorkshop();
    void CaptureToWorkshop();
    void RememberWorkshopFile(const std::filesystem::path& path); // Caller holds _workshopMutex.

    std::unique_ptr<httplib::Server> _server;
    std::thread _serverThread;

    std::mutex _monitorMutex;
    std::condition_variable _monitorWake;
    std::thread _monitorThread;
    bool _monitorStopping{false};
    SystemMonitor _systemMonitor;

    std::mutex _queueMutex;
    std::vector<Command> _queue;

    mutable std::mutex _previewMutex;
    bool _livePreview{true};
    Clock::time_point _lastPreviewRequest{};
    Clock::time_point _lastPreviewCapture{};
    std::vector<uint8_t> _previewFrame;
    uint64_t _previewSeq{0};

    mutable std::mutex _statusMutex;
    StatusSnapshot _statusSnapshot;
    SettingsSnapshot _settingsSnapshot{};
    SystemStats _systemStats;
    mutable std::string _statusJson{"{}"};
    mutable std::string _settingsJson{"{}"};
    mutable bool _statusDirty{true};
    mutable bool _settingsDirty{false};
    std::string _editPath;
    std::string _audioDevicesJson{"{\"current\":-1,\"devices\":[]}"};
    std::vector<int> _audioDeviceIds;
    Clock::time_point _nextFpsSample{};
    Clock::time_point _nextSettingsSample{};
    int _audioDeviceIndex{-1};

    mutable std::mutex _dataMutex;
    std::string _presetsJson{"[]"};
    std::string _presetsEtag;

    mutable std::mutex _favMutex;
    std::set<std::string> _favorites;
    bool _favoritesDirty{false};
    std::string _favoritesFile;
    std::atomic<uint64_t> _favoritesRevision{0};
    uint64_t _lastFavoritesRevision{0};

    std::unordered_map<std::string, uint32_t> _pathToIndex;
    std::unordered_map<std::string, std::vector<uint32_t>> _categoryIndices;
    uint64_t _lastPlaylistRevision{0};
    uint32_t _lastPlaylistSize{UINT32_MAX};
    uint64_t _presetCacheVersion{0};
    std::mt19937 _rng{std::random_device{}()};

    // Both shuffle modes are guarded by _catMutex when changing modes.
    std::mutex _catMutex;
    std::string _catShufflePrefix;
    std::atomic<bool> _catShuffle{false};
    std::atomic<bool> _favShuffle{false};

    std::mutex _workshopMutex;
    std::filesystem::path _workshopDir;
    std::unordered_map<std::string, std::filesystem::file_time_type> _workshopSeen;
    bool _workshopSeeded{false};

    // Active preset paths are owned by the render thread.
    bool _workshopActive{false};
    std::string _workshopPath;
    std::string _workshopPreset;
    std::string _currentPath;
    std::string _currentPreset;

    std::string _token;
    std::string _presetRoot;
    std::string _webRoot;
    uint16_t _port{8080};

    Poco::Logger& _logger{Poco::Logger::get("RemoteControl")};
};
