#include "Dispatchers.hpp"

#include "PluginRuntime.hpp"
#include "../overview/scroll/ScrollOverview.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>

namespace {
    PHLWINDOW windowToBringFromWorkspace(const PHLWORKSPACE& workspace) {
        if (!workspace)
            return nullptr;

        for (auto it = g_pCompositor->m_windows.rbegin(); it != g_pCompositor->m_windows.rend(); ++it) {
            const auto& w = *it;
            if (!w || w->m_workspace != workspace || !w->m_isMapped || w->isHidden())
                continue;

            return w;
        }

        return nullptr;
    }

    SDispatchResult bringWindowFromWorkspace(int64_t sourceWorkspaceID) {
        if (sourceWorkspaceID == WORKSPACE_INVALID)
            return {.success = false, .error = "selected workspace is empty"};

        const auto FOCUSSTATE = Desktop::focusState();
        const auto MONITOR    = FOCUSSTATE->monitor();
        if (!MONITOR || !MONITOR->m_activeWorkspace)
            return {.success = false, .error = "no active monitor/workspace"};

        if (sourceWorkspaceID == MONITOR->activeWorkspaceID())
            return {};

        const auto SOURCEWORKSPACE = g_pCompositor->getWorkspaceByID(sourceWorkspaceID);
        if (!SOURCEWORKSPACE)
            return {.success = false, .error = "selected workspace is not open"};

        const auto WINDOW = windowToBringFromWorkspace(SOURCEWORKSPACE);
        if (!WINDOW)
            return {.success = false, .error = "selected workspace has no mapped windows"};

        g_pCompositor->moveWindowToWorkspaceSafe(WINDOW, MONITOR->m_activeWorkspace);
        FOCUSSTATE->fullWindowFocus(WINDOW, Desktop::FOCUS_REASON_KEYBIND);
        g_pCompositor->warpCursorTo(WINDOW->middle());
        return {};
    }

    SDispatchResult openOverview() {
        const auto MONITOR = Desktop::focusState()->monitor();
        if (!MONITOR || !MONITOR->m_activeWorkspace)
            return {.success = false, .error = "no active monitor/workspace"};

        Hyprview::COverviewRenderGuard renderGuard;
        g_pOverview = makeShared<CScrollOverview>(MONITOR->m_activeWorkspace);
        Hyprview::damageOverviewMonitor();
        return {};
    }
}

namespace Hyprview {

    SDispatchResult onOverviewDispatcher(std::string arg) {
        if (g_pOverview && g_pOverview->m_isSwiping)
            return {.success = false, .error = "already swiping"};

        if (arg == "select") {
            if (g_pOverview) {
                g_pOverview->selectHoveredWorkspace();
                g_pOverview->close();
            }
            return {};
        }

        if (arg == "bring") {
            if (g_pOverview) {
                g_pOverview->selectHoveredWorkspace();
                const auto BRINGRESULT = bringWindowFromWorkspace(g_pOverview->selectedWorkspaceID());
                g_pOverview->close(false);
                return BRINGRESULT;
            }
            return {};
        }

        if (arg == "toggle") {
            if (g_pOverview)
                g_pOverview->close();
            else
                return openOverview();
            return {};
        }

        if (arg == "off" || arg == "close" || arg == "disable") {
            if (g_pOverview)
                g_pOverview->close();
            return {};
        }

        if (g_pOverview)
            return {};

        return openOverview();
    }

    SDispatchResult moveHoveredWindowToActiveWorkspace() {
        if (!g_pOverview)
            return {.success = false, .error = "overview is not open"};

        g_pOverview->selectHoveredWorkspace();

        const auto WINDOW = g_pOverview->selectedWindow();
        if (!WINDOW)
            return {.success = false, .error = "no hovered window"};

        const auto FOCUSSTATE = Desktop::focusState();
        const auto MONITOR    = FOCUSSTATE->monitor();
        if (!MONITOR || !MONITOR->m_activeWorkspace)
            return {.success = false, .error = "no active monitor/workspace"};

        if (WINDOW->m_workspace != MONITOR->m_activeWorkspace)
            g_pCompositor->moveWindowToWorkspaceSafe(WINDOW, MONITOR->m_activeWorkspace);

        FOCUSSTATE->fullWindowFocus(WINDOW, Desktop::FOCUS_REASON_KEYBIND);
        g_pCompositor->warpCursorTo(WINDOW->middle());
        return {};
    }

}
