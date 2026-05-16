#include "ScrollOverview.hpp"

#include <algorithm>

#define private   public
#define protected public
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/protocols/XDGShell.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/xwayland/XSurface.hpp>
#undef protected
#undef private

namespace {
    const std::vector<PHLWINDOW>& emptyWindowList() {
        static const std::vector<PHLWINDOW> EMPTY;
        return EMPTY;
    }
}

void CScrollOverview::invalidateOverviewWindowIndex() const {
    overviewWindowIndexDirty = true;
    overviewWindowIndex      = {};
}

CScrollOverview::SOptionalWorkspaceID CScrollOverview::overviewWorkspaceIDForWindow(PHLWINDOW window) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || !window)
        return {};

    if (!validMapped(window) && !window->m_fadingOut)
        return {};

    if (window->m_pinned) {
        if (window->m_monitor != MONITOR || !MONITOR->m_activeWorkspace || MONITOR->m_activeWorkspace->m_isSpecialWorkspace)
            return {};

        return MONITOR->m_activeWorkspace->m_id;
    }

    if (window->m_fadingOut && !window->m_workspace) {
        if (window->m_monitor != MONITOR || g_pCompositor->isWorkspaceSpecial(window->workspaceID()))
            return {};

        return window->workspaceID();
    }

    if (!window->m_workspace || window->m_workspace->m_isSpecialWorkspace || window->m_workspace->m_monitor != MONITOR)
        return {};

    return window->m_workspace->m_id;
}

void CScrollOverview::rebuildOverviewWindowIndex() const {
    const auto MONITOR = pMonitor.lock();

    overviewWindowIndex      = {};
    overviewWindowIndexDirty = false;

    if (!MONITOR)
        return;

    overviewWindowIndex.monitor           = MONITOR.get();
    overviewWindowIndex.activeWorkspaceID = MONITOR->activeWorkspaceID();
    overviewWindowIndex.compositorWindows = g_pCompositor->m_windows.size();

    for (const auto& window : g_pCompositor->m_windows) {
        const auto WORKSPACE_ID = overviewWorkspaceIDForWindow(window);
        if (!WORKSPACE_ID)
            continue;

        overviewWindowIndex.windows.emplace_back(window);

        if (window->m_pinned && window->m_isFloating && window->m_monitor == MONITOR)
            overviewWindowIndex.pinnedFloatingWindows.emplace_back(window);

        auto& workspaceWindows = window->m_isFloating ? overviewWindowIndex.floatingWindowsByWorkspace[*WORKSPACE_ID] : overviewWindowIndex.tiledWindowsByWorkspace[*WORKSPACE_ID];
        workspaceWindows.emplace_back(window);
    }
}

const CScrollOverview::SWindowList& CScrollOverview::overviewWindows() const {
    const auto MONITOR = pMonitor.lock();
    const auto ACTIVE  = MONITOR ? MONITOR->activeWorkspaceID() : WORKSPACE_INVALID;

    if (overviewWindowIndexDirty || overviewWindowIndex.monitor != MONITOR.get() || overviewWindowIndex.activeWorkspaceID != ACTIVE ||
        overviewWindowIndex.compositorWindows != g_pCompositor->m_windows.size())
        rebuildOverviewWindowIndex();

    return overviewWindowIndex.windows;
}

const CScrollOverview::SWindowList& CScrollOverview::pinnedFloatingOverviewWindows() const {
    overviewWindows();
    return overviewWindowIndex.pinnedFloatingWindows;
}

const CScrollOverview::SWindowList& CScrollOverview::overviewWindowsForWorkspace(PHLWORKSPACE workspace, bool floating) const {
    if (!workspace)
        return emptyWindowList();

    overviewWindows();

    const auto& BY_WORKSPACE = floating ? overviewWindowIndex.floatingWindowsByWorkspace : overviewWindowIndex.tiledWindowsByWorkspace;
    const auto  IT           = BY_WORKSPACE.find(workspace->m_id);
    return IT == BY_WORKSPACE.end() ? emptyWindowList() : IT->second;
}

void CScrollOverview::rebuildWorkspaceEntries(PHLWORKSPACE workspace) {
    if (!refreshingWorkspaceEntries && pMonitor->m_activeWorkspace != startedOn && !closing)
        onWorkspaceChange();

    resetSurfacePolicyCache();
    invalidateWindowEntryLookups();
    markGeometryCacheDirty(GEOMETRY_DIRTY_WINDOW_ENTRIES | GEOMETRY_DIRTY_WINDOW_GEOMETRY);

    auto overview = workspaceEntryForWorkspace(workspace);
    if (!overview)
        return;

    overview->windowEntries.clear();

    const auto&            tiledWindows    = overviewWindowsForWorkspace(workspace, false);
    const auto&            floatingWindows = overviewWindowsForWorkspace(workspace, true);
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
    if (!pMonitor || !window)
        return false;

    if (window->m_fadingOut)
        return window->m_monitor == pMonitor && window->m_snapshotFB && window->m_snapshotFB->getTexture();

    if (!validMapped(window))
        return false;

    if (window->m_isX11)
        return true;

    const auto SURFACE = window->wlSurface();
    if (!SURFACE || !SURFACE->resource() || !SURFACE->resource()->m_current.texture)
        return false;

    return true;
}
