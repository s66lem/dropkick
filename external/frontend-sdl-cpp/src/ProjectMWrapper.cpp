#include "ProjectMWrapper.h"

#include "ProjectMSDLApplication.h"
#include "SDLRenderingWindow.h"

#include "notifications/DisplayToastNotification.h"

#include <Poco/Delegate.h>
#include <Poco/File.h>
#include <Poco/NotificationCenter.h>
#include <Poco/Path.h>

#include <SDL2/SDL_opengl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

namespace
{
/** Reads a newline-separated list file into a set (used for the blocklist and dislikes). */
void LoadLineSet(const std::string& filePath, std::set<std::string>& out)
{
    out.clear();
    std::ifstream in(filePath);
    std::string line;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        if (!line.empty()) { out.insert(line); }
    }
}
} // namespace

const char* ProjectMWrapper::name() const
{
    return "ProjectM Wrapper";
}

void ProjectMWrapper::initialize(Poco::Util::Application& app)
{
    auto& projectMSDLApp = dynamic_cast<ProjectMSDLApplication&>(app);
    _projectMConfigView = projectMSDLApp.config().createView("projectM");
    _userConfig = projectMSDLApp.UserConfiguration();
    poco_information_f1(_logger, "Events enabled: %?d", _projectMConfigView->eventsEnabled());

    _blocklistPath = Poco::Path::expand("~/.local/share/dropkick/blocklist.txt");
    _breadcrumbPath = Poco::Path::expand("~/.local/share/dropkick/state/loading");
    LoadBlocklist();
    QuarantineFromCrash(); // if the previous run died mid-preset, blocklist that preset

    _dislikePath = Poco::Path::expand("~/.local/share/dropkick/dislikes.txt");
    LoadDislikes();

    _slowCountsPath = Poco::Path::expand("~/.local/share/dropkick/slowcounts.txt");
    LoadSlowCounts();

    if (!_projectM)
    {
        auto& sdlWindow = app.getSubsystem<SDLRenderingWindow>();

        int canvasWidth{0};
        int canvasHeight{0};

        sdlWindow.GetDrawableSize(canvasWidth, canvasHeight);

        auto presetPaths = GetPathListWithDefault("presetPath", app.config().getString("application.dir", ""));
        auto texturePaths = GetPathListWithDefault("texturePath", app.config().getString("", ""));

        _projectM = projectm_create();
        if (!_projectM)
        {
            poco_error(_logger, "Failed to initialize projectM. Possible reasons are a lack of required OpenGL features or GPU resources.");
            throw std::runtime_error("projectM initialization failed");
        }

        int fps = _projectMConfigView->getInt("fps", 60);
        if (fps <= 0)
        {
            // We don't know the target framerate, pass in a default of 60.
            fps = 60;
        }

        projectm_set_window_size(_projectM, canvasWidth, canvasHeight);
        projectm_set_fps(_projectM, fps);
        projectm_set_mesh_size(_projectM, _projectMConfigView->getInt("meshX", 48), _projectMConfigView->getInt("meshY", 32));
        projectm_set_aspect_correction(_projectM, _projectMConfigView->getBool("aspectCorrectionEnabled", true));
        projectm_set_preset_locked(_projectM, _projectMConfigView->getBool("presetLocked", false));

        // Preset display settings
        projectm_set_preset_duration(_projectM, _projectMConfigView->getDouble("displayDuration", 30.0));
        projectm_set_soft_cut_duration(_projectM, _projectMConfigView->getDouble("transitionDuration", 3.0));
        projectm_set_hard_cut_enabled(_projectM, _projectMConfigView->getBool("hardCutsEnabled", false));
        projectm_set_hard_cut_duration(_projectM, _projectMConfigView->getDouble("hardCutDuration", 20.0));
        projectm_set_hard_cut_sensitivity(_projectM, static_cast<float>(_projectMConfigView->getDouble("hardCutSensitivity", 1.0)));
        projectm_set_beat_sensitivity(_projectM, static_cast<float>(_projectMConfigView->getDouble("beatSensitivity", 1.0)));

        if (!texturePaths.empty())
        {
            std::vector<const char*> texturePathList;
            texturePathList.reserve(texturePaths.size());
            for (const auto& texturePath : texturePaths)
            {
                texturePathList.push_back(texturePath.data());
            }

            projectm_set_texture_search_paths(_projectM, texturePathList.data(), texturePaths.size());
        }

        // Playlist
        _playlist = projectm_playlist_create(_projectM);
        if (!_playlist)
        {

            poco_error(_logger, "Failed to create the projectM preset playlist manager instance.");
            throw std::runtime_error("Playlist initialization failed");
        }

        projectm_playlist_set_shuffle(_playlist, _projectMConfigView->getBool("shuffleEnabled", true));

        for (const auto& presetPath : presetPaths)
        {
            Poco::File file(presetPath);
            if (file.exists() && file.isFile())
            {
                projectm_playlist_add_preset(_playlist, presetPath.c_str(), false);
            }
            else
            {
                // Symbolic links also fall under this. Without complex resolving, we can't
                // be sure what the link exactly points to, especially if a trailing slash is missing.
                projectm_playlist_add_path(_playlist, presetPath.c_str(), true, false);
            }
        }
        projectm_playlist_sort(_playlist, 0, projectm_playlist_size(_playlist), SORT_PREDICATE_FILENAME_ONLY, SORT_ORDER_ASCENDING);
        ApplyRemovalLists(); // drop presets known to hang/kill the app, or disliked

        projectm_playlist_set_preset_switched_event_callback(_playlist, &ProjectMWrapper::PresetSwitchedEvent, static_cast<void*>(this));
    }

    Poco::NotificationCenter::defaultCenter().addObserver(_playbackControlNotificationObserver);

    // Observe user configuration changes (set via the settings window)
    _userConfig->propertyChanged += Poco::delegate(this, &ProjectMWrapper::OnConfigurationPropertyChanged);
    _userConfig->propertyRemoved += Poco::delegate(this, &ProjectMWrapper::OnConfigurationPropertyRemoved);
}

void ProjectMWrapper::uninitialize()
{
    _userConfig->propertyRemoved -= Poco::delegate(this, &ProjectMWrapper::OnConfigurationPropertyRemoved);
    _userConfig->propertyChanged -= Poco::delegate(this, &ProjectMWrapper::OnConfigurationPropertyChanged);
    Poco::NotificationCenter::defaultCenter().removeObserver(_playbackControlNotificationObserver);

    ClearBreadcrumb(); // clean shutdown — the current preset didn't crash us

    if (_projectM)
    {
        projectm_destroy(_projectM);
        _projectM = nullptr;
    }

    if (_playlist)
    {
        projectm_playlist_destroy(_playlist);
        _playlist = nullptr;
    }
}

projectm_handle ProjectMWrapper::ProjectM() const
{
    return _projectM;
}

projectm_playlist_handle ProjectMWrapper::Playlist() const
{
    return _playlist;
}

int ProjectMWrapper::TargetFPS()
{
    return _projectMConfigView->getInt("fps", 60);
}

void ProjectMWrapper::UpdateRealFPS(float fps)
{
    projectm_set_fps(_projectM, static_cast<uint32_t>(std::round(fps)));
}

void ProjectMWrapper::RenderFrame(uint32_t targetFbo) const
{
    glClearColor(0.0, 0.0, 0.0, 0.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    size_t currentMeshX{0};
    size_t currentMeshY{0};
    projectm_get_mesh_size(_projectM, &currentMeshX, &currentMeshY);
    if (currentMeshX != _projectMConfigView->getInt("meshX", 220) ||
        currentMeshY != _projectMConfigView->getInt("meshY", 125))
    {
        projectm_set_mesh_size(_projectM, _projectMConfigView->getInt("meshX", 220), _projectMConfigView->getInt("meshY", 125));
    }

    if (targetFbo != 0)
    {
        projectm_opengl_render_frame_fbo(_projectM, targetFbo);
    }
    else
    {
        projectm_opengl_render_frame(_projectM);
    }
}

void ProjectMWrapper::DisplayInitialPreset()
{
    if (!_projectMConfigView->getBool("enableSplash", true))
    {
        if (_projectMConfigView->getBool("shuffleEnabled", true))
        {
            projectm_playlist_play_next(_playlist, true);
        }
        else
        {
            projectm_playlist_set_position(_playlist, 0, true);
        }
    }
}

void ProjectMWrapper::ChangeBeatSensitivity(float value)
{
    projectm_set_beat_sensitivity(_projectM, projectm_get_beat_sensitivity(_projectM) + value);
    Poco::NotificationCenter::defaultCenter().postNotification(
        new DisplayToastNotification(Poco::format("Beat Sensitivity: %.2hf", projectm_get_beat_sensitivity(_projectM))));
}

std::string ProjectMWrapper::ProjectMBuildVersion()
{
    return PROJECTM_VERSION_STRING;
}

std::string ProjectMWrapper::ProjectMRuntimeVersion()
{
    auto* projectMVersion = projectm_get_version_string();
    std::string projectMRuntimeVersion(projectMVersion);
    projectm_free_string(projectMVersion);

    return projectMRuntimeVersion;
}

void ProjectMWrapper::PresetFileNameToClipboard() const
{
    auto presetName = projectm_playlist_item(_playlist, projectm_playlist_get_position(_playlist));
    SDL_SetClipboardText(presetName);
    projectm_playlist_free_string(presetName);
}

bool ProjectMWrapper::LoadPresetPack(const std::string& packPath)
{
    if (!_playlist)
    {
        return false;
    }

    projectm_playlist_clear(_playlist);
    uint32_t added = projectm_playlist_add_path(_playlist, packPath.c_str(),
                                                true /*recurse*/, false /*allow_duplicates*/);
    if (added > 0)
    {
        projectm_playlist_sort(_playlist, 0, projectm_playlist_size(_playlist),
                               SORT_PREDICATE_FILENAME_ONLY, SORT_ORDER_ASCENDING);
    }
    ApplyRemovalLists(); // keep blocked/disliked presets out after a pack switch

    poco_information_f2(_logger, "Loaded preset pack '%s' (%?u presets).", packPath, added);
    return added > 0;
}

ProjectMWrapper::PlaybackStatus ProjectMWrapper::CurrentStatus() const
{
    PlaybackStatus status;
    if (!_playlist || !_projectM)
    {
        return status;
    }

    status.playlistSize = projectm_playlist_size(_playlist);
    status.position = projectm_playlist_get_position(_playlist);
    status.shuffle = projectm_playlist_get_shuffle(_playlist);
    status.locked = projectm_get_preset_locked(_projectM);

    if (status.playlistSize > 0 && status.position < status.playlistSize)
    {
        char* item = projectm_playlist_item(_playlist, status.position);
        if (item)
        {
            status.presetName = item;
            projectm_playlist_free_string(item);
        }
    }
    return status;
}

std::vector<std::string> ProjectMWrapper::PlaylistItems() const
{
    std::vector<std::string> items;
    if (!_playlist)
    {
        return items;
    }

    uint32_t size = projectm_playlist_size(_playlist);
    if (size == 0)
    {
        return items;
    }

    char** array = projectm_playlist_items(_playlist, 0, size);
    if (array)
    {
        for (uint32_t i = 0; array[i] != nullptr; ++i)
        {
            items.emplace_back(array[i]);
        }
        projectm_playlist_free_string_array(array);
    }
    return items;
}

void ProjectMWrapper::LoadPresetFile(const std::string& path) const
{
    if (_projectM)
    {
        projectm_load_preset_file(_projectM, path.c_str(), true);
    }
}

void ProjectMWrapper::LoadBlocklist()
{
    LoadLineSet(_blocklistPath, _blocklist);
}

void ProjectMWrapper::AddToBlocklist(const std::string& path)
{
    if (path.empty() || _blocklist.count(path)) { return; }
    _blocklist.insert(path);
    std::ofstream out(_blocklistPath, std::ios::app);
    if (out) { out << path << "\n"; }
    poco_information_f1(_logger, "Quarantined preset (GPU-hang blocklist): %s", path);
}

void ProjectMWrapper::ApplyRemovalLists()
{
    if (!_playlist || (_blocklist.empty() && _dislikes.empty())) { return; }
    uint32_t size = projectm_playlist_size(_playlist);
    uint32_t removed = 0;
    for (uint32_t i = size; i-- > 0;) // high->low so indices stay valid while removing
    {
        char* item = projectm_playlist_item(_playlist, i);
        if (item)
        {
            if (_blocklist.count(item) || _dislikes.count(item))
            {
                projectm_playlist_remove_preset(_playlist, i);
                ++removed;
            }
            projectm_playlist_free_string(item);
        }
    }
    if (removed)
    {
        poco_information_f2(_logger, "Removal lists: dropped %?u presets (%?u blocked, disliked lists applied).",
                            removed, static_cast<uint32_t>(_blocklist.size()));
    }
}

void ProjectMWrapper::WriteBreadcrumb(const std::string& path)
{
    if (path.empty()) { return; }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(_breadcrumbPath).parent_path(), ec); // no-op if present
    std::ofstream out(_breadcrumbPath, std::ios::trunc);
    if (out) { out << path; }
}

void ProjectMWrapper::ClearBreadcrumb()
{
    std::remove(_breadcrumbPath.c_str());
}

void ProjectMWrapper::QuarantineFromCrash()
{
    std::ifstream in(_breadcrumbPath);
    if (!in) { return; }
    std::string path((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) { path.pop_back(); }
    if (!path.empty())
    {
        poco_information_f1(_logger, "Previous run died on a preset — quarantining: %s", path);
        AddToBlocklist(path);
    }
    std::remove(_breadcrumbPath.c_str());
}

std::string ProjectMWrapper::CurrentPresetPath() const
{
    if (!_playlist) { return {}; }
    char* item = projectm_playlist_item(_playlist, projectm_playlist_get_position(_playlist));
    std::string path = item ? item : "";
    if (item) { projectm_playlist_free_string(item); }
    return path;
}

void ProjectMWrapper::QuarantineCurrent()
{
    if (!_playlist) { return; }
    AddToBlocklist(CurrentPresetPath());
    ClearBreadcrumb();
    projectm_playlist_play_next(_playlist, true); // move off the bad preset first
    ApplyRemovalLists();                          // then drop it (and any others) from the playlist
}

uint32_t ProjectMWrapper::BlockedCount() const
{
    return static_cast<uint32_t>(_blocklist.size());
}

void ProjectMWrapper::ClearBlocklist()
{
    _blocklist.clear();
    std::remove(_blocklistPath.c_str());
    poco_information(_logger, "Blocklist cleared (cleared presets return on next pack load/restart).");
}

void ProjectMWrapper::LoadDislikes()
{
    LoadLineSet(_dislikePath, _dislikes);
}

void ProjectMWrapper::AddToDislikes(const std::string& path)
{
    if (path.empty() || _dislikes.count(path)) { return; }
    _dislikes.insert(path);
    std::ofstream out(_dislikePath, std::ios::app);
    if (out) { out << path << "\n"; }
    poco_information_f1(_logger, "Disliked preset: %s", path);
}

void ProjectMWrapper::DislikeCurrent()
{
    if (!_playlist) { return; }
    std::string path = CurrentPresetPath();
    if (path.empty()) { return; }

    AddToDislikes(path);
    projectm_playlist_play_next(_playlist, true); // move off it first
    ApplyRemovalLists();                          // then drop it (and any others)
}

uint32_t ProjectMWrapper::DislikedCount() const
{
    return static_cast<uint32_t>(_dislikes.size());
}

void ProjectMWrapper::ClearDislikes()
{
    _dislikes.clear();
    std::remove(_dislikePath.c_str());
    poco_information(_logger, "Dislike list cleared (disliked presets return on next pack load/restart).");
}

void ProjectMWrapper::LoadSlowCounts()
{
    _slowCounts.clear();
    std::ifstream in(_slowCountsPath);
    std::string line;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        auto tab = line.find('\t');
        if (tab == std::string::npos) { continue; }
        uint32_t count = static_cast<uint32_t>(std::strtoul(line.substr(0, tab).c_str(), nullptr, 10));
        std::string path = line.substr(tab + 1);
        if (!path.empty() && count > 0) { _slowCounts[path] = count; }
    }
}

void ProjectMWrapper::SaveSlowCounts()
{
    std::ofstream out(_slowCountsPath, std::ios::trunc);
    if (!out) { return; }
    for (const auto& kv : _slowCounts) { out << kv.second << '\t' << kv.first << "\n"; }
}

void ProjectMWrapper::AutoSkipSlow(uint32_t strikesThreshold)
{
    if (!_playlist) { return; }
    std::string path = CurrentPresetPath();
    if (path.empty()) { return; }

    uint32_t strikes = ++_slowCounts[path];
    if (strikesThreshold > 0 && strikes >= strikesThreshold)
    {
        _slowCounts.erase(path);
        SaveSlowCounts();
        AddToBlocklist(path);
        projectm_playlist_play_next(_playlist, true);
        ApplyRemovalLists();
        poco_information_f2(_logger, "Auto-skip: preset too slow %?u times — blocklisted: %s",
                            strikes, path);
    }
    else
    {
        SaveSlowCounts();
        projectm_playlist_play_next(_playlist, true);
        poco_information_f2(_logger, "Auto-skip: slow preset (strike %?u): %s", strikes, path);
    }
}

void ProjectMWrapper::PresetSwitchedEvent(bool isHardCut, unsigned int index, void* context)
{
    auto that = reinterpret_cast<ProjectMWrapper*>(context);
    auto presetName = projectm_playlist_item(that->_playlist, index);
    std::string path = presetName ? presetName : "";
    poco_information_f1(that->_logger, "Displaying preset: %s", path);
    projectm_playlist_free_string(presetName);

    // Crash breadcrumb: record the now-active preset so a GPU hang that kills us
    // can be quarantined on the next (supervisor) restart.
    that->WriteBreadcrumb(path);

    Poco::NotificationCenter::defaultCenter().postNotification(new UpdateWindowTitleNotification);
}

void ProjectMWrapper::PlaybackControlNotificationHandler(const Poco::AutoPtr<PlaybackControlNotification>& notification)
{
    bool shuffleEnabled = projectm_playlist_get_shuffle(_playlist);

    switch (notification->ControlAction())
    {
        case PlaybackControlNotification::Action::NextPreset:
            projectm_playlist_set_shuffle(_playlist, false);
            projectm_playlist_play_next(_playlist, !notification->SmoothTransition());
            projectm_playlist_set_shuffle(_playlist, shuffleEnabled);
            break;

        case PlaybackControlNotification::Action::PreviousPreset:
            projectm_playlist_set_shuffle(_playlist, false);
            projectm_playlist_play_previous(_playlist, !notification->SmoothTransition());
            projectm_playlist_set_shuffle(_playlist, shuffleEnabled);
            break;

        case PlaybackControlNotification::Action::LastPreset:
            projectm_playlist_play_last(_playlist, !notification->SmoothTransition());
            break;

        case PlaybackControlNotification::Action::RandomPreset: {
            projectm_playlist_set_shuffle(_playlist, true);
            projectm_playlist_play_next(_playlist, !notification->SmoothTransition());
            projectm_playlist_set_shuffle(_playlist, shuffleEnabled);
            break;
        }

        case PlaybackControlNotification::Action::ToggleShuffle:
            _userConfig->setBool("projectM.shuffleEnabled", !shuffleEnabled);
            break;

        case PlaybackControlNotification::Action::TogglePresetLocked: {
            _userConfig->setBool("projectM.presetLocked", !projectm_get_preset_locked(_projectM));
            break;
        }
    }
}

std::vector<std::string> ProjectMWrapper::GetPathListWithDefault(const std::string& baseKey, const std::string& defaultPath)
{
    using Poco::Util::AbstractConfiguration;

    std::vector<std::string> pathList;
    auto defaultPresetPath = _projectMConfigView->getString(baseKey, defaultPath);
    if (!defaultPresetPath.empty())
    {
        pathList.push_back(defaultPresetPath);
    }
    AbstractConfiguration::Keys subKeys;
    _projectMConfigView->keys(baseKey, subKeys);
    for (const auto& key : subKeys)
    {
        auto path = _projectMConfigView->getString(baseKey + "." + key, "");
        if (!path.empty())
        {
            pathList.push_back(std::move(path));
        }
    }
    return pathList;
}

void ProjectMWrapper::OnConfigurationPropertyChanged(const Poco::Util::AbstractConfiguration::KeyValue& property)
{
    OnConfigurationPropertyRemoved(property.key());
}

void ProjectMWrapper::OnConfigurationPropertyRemoved(const std::string& key)
{
    if (_projectM == nullptr || _playlist == nullptr)
    {
        return;
    }

    if (key == "projectM.presetLocked")
    {
        projectm_set_preset_locked(_projectM, _projectMConfigView->getBool("presetLocked", false));
        Poco::NotificationCenter::defaultCenter().postNotification(new UpdateWindowTitleNotification);
    }

    if (key == "projectM.shuffleEnabled")
    {
        projectm_playlist_set_shuffle(_playlist, _projectMConfigView->getBool("shuffleEnabled", true));
    }

    if (key == "projectM.aspectCorrectionEnabled")
    {
        projectm_set_aspect_correction(_projectM, _projectMConfigView->getBool("aspectCorrectionEnabled", true));
    }

    if (key == "projectM.displayDuration")
    {
        projectm_set_preset_duration(_projectM, _projectMConfigView->getDouble("displayDuration", 30.0));
    }

    if (key == "projectM.transitionDuration")
    {
        projectm_set_soft_cut_duration(_projectM, _projectMConfigView->getDouble("transitionDuration", 3.0));
    }

    if (key == "projectM.hardCutsEnabled")
    {
        projectm_set_hard_cut_enabled(_projectM, _projectMConfigView->getBool("hardCutsEnabled", false));
    }

    if (key == "projectM.hardCutDuration")
    {
        projectm_set_hard_cut_duration(_projectM, _projectMConfigView->getDouble("hardCutDuration", 20.0));
    }

    if (key == "projectM.hardCutSensitivity")
    {
        projectm_set_hard_cut_sensitivity(_projectM, static_cast<float>(_projectMConfigView->getDouble("hardCutSensitivity", 1.0)));
    }

    if (key == "projectM.meshX" || key == "projectM.meshY")
    {
        projectm_set_mesh_size(_projectM, _projectMConfigView->getUInt64("meshX", 48), _projectMConfigView->getUInt64("meshY", 32));
    }
}
