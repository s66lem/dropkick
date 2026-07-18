#include "gui/MainMenu.h"

#include "AudioCapture.h"
#include "ProjectMSDLApplication.h"
#include "ProjectMWrapper.h"

#include "gui/ProjectMGUI.h"
#include "gui/SystemBrowser.h"

#include "notifications/PlaybackControlNotification.h"
#include "notifications/QuitNotification.h"
#include "notifications/UpdateWindowTitleNotification.h"

#include "imgui.h"

#include <Poco/NotificationCenter.h>


MainMenu::MainMenu(ProjectMGUI& gui)
    : _notificationCenter(Poco::NotificationCenter::defaultCenter())
    , _gui(gui)
    , _projectMWrapper(Poco::Util::Application::instance().getSubsystem<ProjectMWrapper>())
    , _audioCapture(Poco::Util::Application::instance().getSubsystem<AudioCapture>())
{
}

void MainMenu::Draw()
{
    // Auto-hide: the menu bar disappears after 5s without pointer activity near it
    // and comes back while the cursor hovers the top edge of the window. Uses wall
    // time, not ImGui::GetTime() — the ImGui clock freezes while the UI is hidden.
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now - _lastDrawTime > 0.5)
    {
        _lastActivityTime = now; // the UI was just toggled on — start with the bar visible
    }
    _lastDrawTime = now;

    const auto& io = ImGui::GetIO();
    const bool nearTop = io.MousePos.y >= 0.0f && io.MousePos.y <= ImGui::GetFrameHeight() * 1.5f;
    if (nearTop || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
    {
        _lastActivityTime = now; // hovering the bar or a menu is open — keep it up
    }
    if (now - _lastActivityTime > 5.0)
    {
        return;
    }

    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("Settings...", "Ctrl+s"))
            {
                _gui.ShowSettingsWindow();
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Quit projectM", "Ctrl+q"))
            {
                _notificationCenter.postNotification(new QuitNotification);
            }

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Playback"))
        {
            auto& app = ProjectMSDLApplication::instance();

            if (ImGui::MenuItem("Play Next Preset", "n"))
            {
                _notificationCenter.postNotification(new PlaybackControlNotification(PlaybackControlNotification::Action::NextPreset));
            }
            if (ImGui::MenuItem("Play Previous Preset", "p"))
            {
                // History-based: the preset that actually played before (matters with shuffle).
                _notificationCenter.postNotification(new PlaybackControlNotification(PlaybackControlNotification::Action::LastPreset));
            }
            if (ImGui::MenuItem("Random Preset", "r"))
            {
                _notificationCenter.postNotification(new PlaybackControlNotification(PlaybackControlNotification::Action::RandomPreset));
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Lock Preset", "Spacebar", app.config().getBool("projectM.presetLocked", false)))
            {
                _notificationCenter.postNotification(new PlaybackControlNotification(PlaybackControlNotification::Action::TogglePresetLocked));
            }
            if (ImGui::MenuItem("Enable Shuffle", "y", app.config().getBool("projectM.shuffleEnabled", true)))
            {
                _notificationCenter.postNotification(new PlaybackControlNotification(PlaybackControlNotification::Action::ToggleShuffle));
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Copy Current Preset Filename", "Ctrl+c"))
            {
                _projectMWrapper.PresetFileNameToClipboard();
            }

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Options"))
        {
            auto& app = ProjectMSDLApplication::instance();

            if (ImGui::BeginMenu("Audio Capture Device"))
            {
                auto devices = _audioCapture.AudioDeviceList();
                auto currentIndex = _audioCapture.AudioDeviceIndex();

                for (const auto& device : devices)
                {
                    if (ImGui::MenuItem(device.second.c_str(), "", device.first == currentIndex))
                    {
                        _audioCapture.AudioDeviceIndex(device.first);
                    }
                }
                ImGui::EndMenu();
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Display Toast Messages", "", app.config().getBool("projectM.displayToasts", true)))
            {
                app.UserConfiguration()->setBool("projectM.displayToasts", !app.config().getBool("projectM.displayToasts", true));
            }
            if (ImGui::MenuItem("Display Preset Name in Window Title", "", app.config().getBool("window.displayPresetNameInTitle", true)))
            {
                app.UserConfiguration()->setBool("window.displayPresetNameInTitle", !app.config().getBool("window.displayPresetNameInTitle", true));
                _notificationCenter.postNotification(new UpdateWindowTitleNotification);
            }

            ImGui::Separator();

            float beatSensitivity = projectm_get_beat_sensitivity(_projectMWrapper.ProjectM());
            if (ImGui::SliderFloat("Beat Sensitivity", &beatSensitivity, 0.0f, 2.0f))
            {
                projectm_set_beat_sensitivity(_projectMWrapper.ProjectM(), beatSensitivity);
                app.UserConfiguration()->setDouble("projectM.beatSensitivity", beatSensitivity);
            }

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Help"))
        {
            if (ImGui::MenuItem("Quick Help..."))
            {
                _gui.ShowHelpWindow();
            }

            ImGui::Separator();

            if (ImGui::MenuItem("About projectM..."))
            {
                _gui.ShowAboutWindow();
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Visit the projectM Wiki on GitHub"))
            {
                SystemBrowser::OpenURL("https://github.com/projectM-visualizer/projectm/wiki");
            }
            if (ImGui::MenuItem("Report a Bug or Request a Feature"))
            {
                SystemBrowser::OpenURL("https://github.com/projectM-visualizer/projectm/issues/new/choose");
            }
            if (ImGui::MenuItem("Sponsor projectM on OpenCollective"))
            {
                SystemBrowser::OpenURL("https://opencollective.com/projectm");
            }
            ImGui::EndMenu();
        }

        ImGui::EndMainMenuBar();
    }
}
