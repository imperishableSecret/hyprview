#include "ScrollOverview.hpp"

#include <algorithm>

#define private   public
#define protected public
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/protocols/XDGShell.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/xwayland/XSurface.hpp>
#undef protected
#undef private

void CScrollOverview::rebuildWorkspaceEntries(PHLWORKSPACE workspace) {
    if (!refreshingWorkspaceEntries && pMonitor->m_activeWorkspace != startedOn && !closing)
        onWorkspaceChange();

    invalidateWindowEntryLookups();
    markGeometryCacheDirty();

    auto overview = workspaceEntryForWorkspace(workspace);
    if (!overview)
        return;

    overview->windowEntries.clear();

    std::vector<PHLWINDOW> tiledWindows;
    std::vector<PHLWINDOW> floatingWindows;
    for (const auto& window : g_pCompositor->m_windows) {
        if (!windowBelongsToWorkspaceInOverview(window, workspace))
            continue;

        if (window->m_isFloating)
            floatingWindows.emplace_back(window);
        else
            tiledWindows.emplace_back(window);
    }

    std::vector<PHLWINDOW> windows;
    windows.reserve(tiledWindows.size() + floatingWindows.size());
    windows.insert(windows.end(), tiledWindows.begin(), tiledWindows.end());
    windows.insert(windows.end(), floatingWindows.begin(), floatingWindows.end());

    for (const auto& window : windows) {
        auto entry     = overview->windowEntries.emplace_back(makeShared<SWindowEntry>());
        entry->pWindow = window;

        const auto DAMAGE_ON_COMMIT = [wk = WP<SWindowEntry>{entry}, self = WP<IOverview>{g_pOverview}] {
            if (!self || !(self == g_pOverview))
                return;

            const auto SCROLL = dynamicPointerCast<CScrollOverview>(self).lock();
            const auto ENTRY  = wk.lock();
            if (!SCROLL || SCROLL->closing || !ENTRY)
                return;

            SCROLL->damage();
            if (SCROLL->pMonitor)
                g_pCompositor->scheduleFrameForMonitor(SCROLL->pMonitor.lock());
        };

        if (window->m_isX11) {
            if (const auto XWAYLAND_SURFACE = window->m_xwaylandSurface.lock())
                entry->windowCommit = makeUnique<CHyprSignalListener>(XWAYLAND_SURFACE->m_events.commit.listen(DAMAGE_ON_COMMIT));
        } else if (const auto XDG_SURFACE = window->m_xdgSurface.lock())
            entry->windowCommit = makeUnique<CHyprSignalListener>(XDG_SURFACE->m_events.commit.listen(DAMAGE_ON_COMMIT));
    }
}

void CScrollOverview::rebuildAllWorkspaceEntries() {
    for (const auto& image : workspaceEntries) {
        if (image && image->pWorkspace)
            rebuildWorkspaceEntries(image->pWorkspace);
    }
}

bool CScrollOverview::windowLiveRenderable(PHLWINDOW window) const {
    if (!pMonitor || !validMapped(window))
        return false;

    if (window->m_isX11)
        return true;

    const auto SURFACE = window->wlSurface();
    if (!SURFACE || !SURFACE->resource() || !SURFACE->resource()->m_current.texture)
        return false;

    return true;
}
