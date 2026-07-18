#pragma once

#include <string>

#include <chrono>

class ProjectMGUI;
class ProjectMWrapper;
class AudioCapture;

namespace Poco {
class NotificationCenter;
}

class MainMenu
{
public:
    MainMenu() = delete;

    explicit MainMenu(ProjectMGUI& gui);

    /**
     * @brief Draws the main menu bar.
     */
    void Draw();

private:
    Poco::NotificationCenter& _notificationCenter; //!< Notification center instance.
    ProjectMGUI& _gui; //!< Reference to the GUI subsystem.
    ProjectMWrapper& _projectMWrapper; //!< Reference to the projectM wrapper subsystem.
    AudioCapture& _audioCapture; //!< Reference to the audio capture subsystem.

    double _lastActivityTime{0.0}; //!< Wall time of the last pointer activity near the menu bar (auto-hide).
    double _lastDrawTime{0.0};     //!< Wall time of the last Draw() call (detects the UI being toggled on).
};
