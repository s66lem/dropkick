#include "AboutWindow.h"

#include "ProjectMGUI.h"
#include "SystemBrowser.h"

#include "ProjectMSDLApplication.h"
#include "ProjectMWrapper.h"

#include <imgui.h>

#include <Poco/Util/Application.h>

AboutWindow::AboutWindow(ProjectMGUI& gui)
    : _gui(gui)
    , _application(ProjectMSDLApplication::instance())
    , _projectMWrapper(Poco::Util::Application::instance().getSubsystem<ProjectMWrapper>())
{
}

void AboutWindow::Show()
{
    _visible = true;
}

void AboutWindow::Draw()
{
    if (!_visible)
    {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(750, 600), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("About Dropkick###About", &_visible, ImGuiWindowFlags_NoCollapse))
    {
        _gui.PushToastFont();
        ImGui::TextUnformatted("Dropkick");
        _gui.PopFont();
        ImGui::Dummy({.0f, 6.0f});
        ImGui::TextUnformatted("A Raspberry Pi audio visualizer built on projectM.");
        ImGui::Dummy({.0f, 10.0f});
        ImGui::Text("Frontend: %s", PROJECTMSDL_VERSION);
        ImGui::Text("libprojectM: %s (built with %s)", _projectMWrapper.ProjectMRuntimeVersion().c_str(), _projectMWrapper.ProjectMBuildVersion().c_str());
        ImGui::Dummy({.0f, 14.0f});

        ImGui::TextUnformatted("What Dropkick adds:");
        ImGui::BulletText("OpenGL ES 3.1 support for the Raspberry Pi 5 (V3D) GPU");
        ImGui::BulletText("Phone/tablet remote: browse, favorites, live preset editor, settings");
        ImGui::BulletText("Auto-skip that quarantines presets which hang the GPU");
        ImGui::BulletText("\"Reduce flashing\" render filter for strobe-heavy presets");
        ImGui::Dummy({.0f, 6.0f});
        ImGui::TextWrapped("Open the remote in a browser on the same network:");
        if (ImGui::SmallButton("http://<this-device>:8080"))
        {
            SystemBrowser::OpenURL("http://localhost:8080");
        }
        ImGui::Dummy({.0f, 6.0f});
        ImGui::TextWrapped("Dropkick source and issues:");
        if (ImGui::SmallButton("https://github.com/s66lem/dropkick"))
        {
            SystemBrowser::OpenURL("https://github.com/s66lem/dropkick");
        }

        ImGui::Dummy({.0f, 10.0f});
        ImGui::Separator();
        ImGui::Dummy({.0f, 10.0f});
        ImGui::TextWrapped("Dropkick is built on the projectM SDL frontend and libprojectM by the projectM Team and contributors. The frontend is licensed under the GNU General Public License v3; libprojectM under the GNU LGPL v2.1.");
        ImGui::Dummy({.0f, 10.0f});
        ImGui::TextWrapped("Upstream projectM source:");
        if (ImGui::SmallButton("https://github.com/projectM-visualizer/frontend-sdl2"))
        {
            SystemBrowser::OpenURL("https://github.com/projectM-visualizer/frontend-sdl2");
        }
        ImGui::Dummy({.0f, 10.0f});
        if (ImGui::CollapsingHeader("Open-Source Software Used in this Application"))
        {
            ImGui::TextUnformatted("Used in projectM SDL:");
            ImGui::BulletText("libprojectM by The projectM Team (GNU LGPL v2.1)");
            ImGui::BulletText("Simple DirectMedia Layer 2 (SDL) (zlib License)");
            ImGui::BulletText("Dear ImGui by Omar Cornut and contributors (MIT)");
            ImGui::BulletText("The POCO C++ Framework by Applied Informatics GmbH (MIT)");
            ImGui::BulletText("FreeType 2 (FreeType License / GNU GPL v2)");

            ImGui::Dummy({.0f, 10.0f});
            ImGui::TextUnformatted("Via libprojectM:");
            ImGui::BulletText("projectm-eval by The projectM Team (MIT)");
            ImGui::BulletText("SOIL2 by Martín Lucas Golini (MIT-0)");
            ImGui::BulletText("hlslparser by Unknown Worlds Entertainment, Inc. (MIT)");
            ImGui::BulletText("OpenGL Mathematics (GLM) by G-Truc Creation (The Happy Bunny License)");
        }
    }
    ImGui::End();
}
