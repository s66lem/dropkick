#pragma once

#include "notifications/PlaybackControlNotification.h"

#include <projectM-4/projectM.h>
#include <projectM-4/playlist.h>

#include <Poco/Logger.h>
#include <Poco/NObserver.h>

#include <Poco/Util/AbstractConfiguration.h>
#include <Poco/Util/Subsystem.h>

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

class ProjectMWrapper : public Poco::Util::Subsystem
{
public:
    const char* name() const override;

    void initialize(Poco::Util::Application& app) override;

    void uninitialize() override;

    /**
     * Returns the projectM instance handle.
     * @return The projectM instance handle used to call API functions.
     */
    projectm_handle ProjectM() const;

    /**
     * Returns the playlist handle.
     * @return The plaslist handle.
     */
    projectm_playlist_handle Playlist() const;

    /**
     * Renders a single projectM frame.
     */
    void RenderFrame(uint32_t targetFbo = 0) const;

    /**
     * @brief Returns the targeted FPS value.
     * @return The user-configured target FPS. Can be 0, which means unlimited.
     */
    int TargetFPS();

    /**
     * @brief Updates projectM with the current, actual FPS value.
     * @param fps The current FPS value.
     */
    void UpdateRealFPS(float fps);

    /**
     * @brief If splash is disabled, shows the initial preset.
     * If shuffle is on, a random preset will be picked. Otherwise, the first playlist item is displayed.
     */
    void DisplayInitialPreset();

    /**
     * @brief Changes beat sensitivity by the given value.
     * @param value A positive or negative delta value.
     */
    void ChangeBeatSensitivity(float value);

    /**
     * @brief Returns the libprojectM version this application was built against.
     * @return A string with the libprojectM build version.
     */
    std::string ProjectMBuildVersion();

    /**
     * @brief Returns the libprojectM version this applications currently runs with.
     * @return A string with the libprojectM runtime library version.
     */
    std::string ProjectMRuntimeVersion();

    /**
     * Copies the full path of the current preset into the OS clipboard.
     */
    void PresetFileNameToClipboard() const;

    /**
     * @brief Playback status snapshot for the remote control.
     */
    struct PlaybackStatus
    {
        std::string presetName;
        uint32_t position{0};
        uint32_t playlistSize{0};
        bool shuffle{false};
        bool locked{false};
    };

    /**
     * @brief Clears the playlist and loads all presets under packPath (recursive, sorted).
     * @param packPath Absolute path to a preset pack directory.
     * @return True if at least one preset was loaded.
     */
    bool LoadPresetPack(const std::string& packPath);

    /**
     * @brief Returns the current playback status (preset name, position, shuffle, lock).
     */
    PlaybackStatus CurrentStatus() const;

    /**
     * @brief Returns the full playlist as a vector of preset file paths.
     */
    std::vector<std::string> PlaylistItems() const;

    /**
     * @brief Loads a preset directly from a file (bypasses the playlist), with a smooth transition.
     * @param path Absolute path to a .milk preset file.
     */
    void LoadPresetFile(const std::string& path) const;

    /**
     * @brief Quarantines the currently-playing preset (adds it to the blocklist, removes it from the
     * playlist) and advances to the next one. Used when a preset hangs the GPU.
     */
    void QuarantineCurrent();

    /**
     * @brief Number of presets currently on the GPU-hang blocklist.
     */
    uint32_t BlockedCount() const;

    /**
     * @brief Clears the blocklist and reloads the current preset path so blocked presets return.
     */
    void ClearBlocklist();

    /** @brief Adds the currently-playing preset to the dislike list, removes it, and advances. */
    void DislikeCurrent();

    /** @brief Number of presets on the dislike list. */
    uint32_t DislikedCount() const;

    /** @brief Clears the dislike list (disliked presets return on the next pack load/restart). */
    void ClearDislikes();

    /**
     * @brief Records that the current preset ran too slowly and skips it. Increments a persistent
     * per-preset strike count; once it reaches @p strikesThreshold the preset is blocklisted and
     * removed. Below the threshold it is only skipped and can recover on a later, faster run.
     */
    void AutoSkipSlow(uint32_t strikesThreshold);

private:
    /**
     * @brief projectM callback. Called whenever a preset is switched.
     * @param isHardCut True if the switch was a hard cut.
     * @param index New preset playlist index.
     * @param context Callback context, e.g. "this" pointer.
     */
    static void PresetSwitchedEvent(bool isHardCut, unsigned int index, void* context);

    void PlaybackControlNotificationHandler(const Poco::AutoPtr<PlaybackControlNotification>& notification);

    std::vector<std::string> GetPathListWithDefault(const std::string& baseKey, const std::string& defaultPath);

    /**
     * @brief Event callback if a configuration value has changed.
     * @param property The key and value that has been changed.
     */
    void OnConfigurationPropertyChanged(const Poco::Util::AbstractConfiguration::KeyValue& property);

    /**
     * @brief Event callback if a configuration value has been removed.
     * @param key The key of the removed property.
     */
    void OnConfigurationPropertyRemoved(const std::string& key);

    // GPU-hang auto-skip: quarantine presets that crash/hang the app. Render thread only.
    void LoadBlocklist();                          //!< Read blocklist file into memory.
    void ApplyRemovalLists();                      //!< Remove blocklisted OR disliked entries from the playlist.
    void LoadDislikes();                           //!< Read the dislike file into memory.
    void AddToDislikes(const std::string& path);   //!< Add a path to the dislike list (memory + file).
    void AddToBlocklist(const std::string& path);  //!< Add a path to the blocklist (memory + file).
    void WriteBreadcrumb(const std::string& path); //!< Record the active preset (crash breadcrumb).
    void ClearBreadcrumb();                        //!< Remove the breadcrumb on clean shutdown.
    void QuarantineFromCrash();                    //!< If the last run died mid-preset, blocklist it.
    void LoadSlowCounts();                          //!< Read persistent low-FPS strike counts.
    void SaveSlowCounts();                          //!< Persist strike counts to disk.

    std::set<std::string> _blocklist;   //!< Preset paths that hang/kill the app.
    std::string _blocklistPath;         //!< ~/.local/share/dropkick/blocklist.txt
    std::set<std::string> _dislikes;    //!< Preset paths the user disliked.
    std::string _dislikePath;           //!< ~/.local/share/dropkick/dislikes.txt
    std::string _breadcrumbPath;        //!< ~/.local/share/dropkick/state/loading
    std::map<std::string, uint32_t> _slowCounts; //!< Preset path -> low-FPS auto-skip strikes.
    std::string _slowCountsPath;                 //!< ~/.local/share/dropkick/slowcounts.txt

    Poco::AutoPtr<Poco::Util::AbstractConfiguration> _userConfig; //!< View of the "projectM" configuration subkey in the "user" configuration.
    Poco::AutoPtr<Poco::Util::AbstractConfiguration> _projectMConfigView; //!< View of the "projectM" configuration subkey in the "effective" configuration.

    projectm_handle _projectM{nullptr}; //!< Pointer to the projectM instance used by the application.
    projectm_playlist_handle _playlist{nullptr}; //!< Pointer to the projectM playlist manager instance.

    Poco::NObserver<ProjectMWrapper, PlaybackControlNotification> _playbackControlNotificationObserver{*this, &ProjectMWrapper::PlaybackControlNotificationHandler};

    Poco::Logger& _logger{Poco::Logger::get("SDLRenderingWindow")}; //!< The class logger.
};
