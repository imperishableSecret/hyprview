#include "ScrollOverview.hpp"
#include "../../plugin/Telemetry.hpp"
#include <algorithm>
#include <any>
#include <cmath>

#define private   public
#define protected public
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#undef protected
#undef private
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>

static constexpr uint64_t FOCUS_CURSOR_SYNC_MS = 220;

namespace {
    std::string workspaceID(PHLWORKSPACE workspace) {
        return workspace ? std::to_string(workspace->m_id) : "<none>";
    }
}

void CScrollOverview::refreshWorkspaceEntries(PHLWORKSPACE preferredViewport, bool warpViewport) {
    if (!pMonitor)
        return;

    invalidateOverviewWindowIndex();

    const auto FALLBACK_VIEWPORT = !preferredViewport && viewportCurrentWorkspace < workspaceEntries.size() && workspaceEntries[viewportCurrentWorkspace] ?
        workspaceEntries[viewportCurrentWorkspace]->pWorkspace :
        PHLWORKSPACE{};

    Hyprview::telemetryLogLazy([&] {
        return std::format("event=refresh-workspaces-start preferred={} fallback={} warp={} oldImages={} viewportIndex={} activeWorkspace={}", workspaceID(preferredViewport),
                           workspaceID(FALLBACK_VIEWPORT), Hyprview::boolToken(warpViewport), workspaceEntries.size(), viewportCurrentWorkspace,
                           workspaceID(pMonitor->m_activeWorkspace));
    });

    resetSurfacePolicyCache();
    invalidateWindowEntryLookups();
    markGeometryCacheDirty(GEOMETRY_DIRTY_WORKSPACE_LIST | GEOMETRY_DIRTY_WINDOW_ENTRIES | GEOMETRY_DIRTY_INSERTION_MARKERS);
    workspaceEntries.clear();

    for (const auto& w : g_pCompositor->getWorkspaces()) {
        if (w && w->m_monitor == pMonitor && !w->m_isSpecialWorkspace && workspaceVisibleInOverview(w.lock()))
            workspaceEntries.emplace_back(makeShared<SWorkspaceEntry>(w.lock()));
    }

    std::sort(workspaceEntries.begin(), workspaceEntries.end(), [](const auto& a, const auto& b) { return a->pWorkspace->m_id < b->pWorkspace->m_id; });
    pruneWorkspaceContentPans();

    for (size_t i = 0; i < workspaceEntries.size(); ++i) {
        const auto& ENTRY = workspaceEntries[i];
        Hyprview::telemetryLogLazy([&] {
            return std::format("event=refresh-workspace-image index={} workspace={} name={} displayable={} scrolling={}", i,
                               workspaceID(ENTRY ? ENTRY->pWorkspace : PHLWORKSPACE{}), ENTRY && ENTRY->pWorkspace ? ENTRY->pWorkspace->m_name : "<none>",
                               Hyprview::boolToken(ENTRY && workspaceHasDisplayableWindows(ENTRY->pWorkspace)),
                               Hyprview::boolToken(ENTRY && workspaceUsesScrollingLayout(ENTRY->pWorkspace)));
        });
    }

    refreshingWorkspaceEntries = true;
    rebuildAllWorkspaceEntries();
    refreshingWorkspaceEntries = false;
    rebuildGeometryCache();
    syncSelectionToViewport(false);

    const auto VIEWPORT_WORKSPACE = preferredViewport ? preferredViewport : FALLBACK_VIEWPORT;
    bool       centeredPreferred  = false;
    bool       movedViewport      = false;
    const bool NORMALIZE_ANCHOR   = VIEWPORT_WORKSPACE && !warpViewport && pMonitor->m_activeWorkspace == VIEWPORT_WORKSPACE;
    if (VIEWPORT_WORKSPACE) {
        if (const auto INDEX = workspaceEntryIndex(VIEWPORT_WORKSPACE); INDEX) {
            if (NORMALIZE_ANCHOR)
                queueViewportAnchorNormalization(VIEWPORT_WORKSPACE);

            movedViewport     = setViewportWorkspace(*INDEX, warpViewport);
            centeredPreferred = true;
        } else if (!workspaceEntries.empty())
            movedViewport = setViewportWorkspace(std::min(viewportCurrentWorkspace, workspaceEntries.size() - 1), true);
    }

    if (centeredPreferred && NORMALIZE_ANCHOR) {
        if (!movedViewport)
            normalizeViewportAnchor(VIEWPORT_WORKSPACE);
    }

    Hyprview::telemetryLogLazy([&] {
        return std::format(
            "event=refresh-workspaces-end workspaceEntries={} viewportIndex={} viewportWorkspace={} viewOffset={} centeredPreferred={} movedViewport={} "
            "normalizeAnchor={}",
            workspaceEntries.size(), viewportCurrentWorkspace,
            workspaceID(viewportCurrentWorkspace < workspaceEntries.size() && workspaceEntries[viewportCurrentWorkspace] ? workspaceEntries[viewportCurrentWorkspace]->pWorkspace :
                                                                                                                           PHLWORKSPACE{}),
            Hyprview::formatVector(viewOffset->value()), Hyprview::boolToken(centeredPreferred), Hyprview::boolToken(movedViewport), Hyprview::boolToken(NORMALIZE_ANCHOR));
    });

    damage();
}

void CScrollOverview::queueRefreshWorkspaceEntries(PHLWORKSPACE preferredViewport, bool warpViewport) {
    if (closing)
        return;

    if (preferredViewport)
        queuedRefreshWorkspace = preferredViewport;
    else if (!queuedRefreshWorkspace && pMonitor)
        queuedRefreshWorkspace = pMonitor->m_activeWorkspace;

    queuedRefreshWarp = queuedRefreshWarp && warpViewport;

    if (refreshQueued)
        return;

    refreshQueued = true;

    const WP<IOverview> SELF = g_pOverview;
    g_pEventLoopManager->doLater([SELF] {
        if (!SELF || !(SELF == g_pOverview))
            return;

        const auto SCROLL = dynamicPointerCast<CScrollOverview>(SELF).lock();
        if (!SCROLL || SCROLL->closing)
            return;

        const auto PREFERRED = SCROLL->queuedRefreshWorkspace.lock();
        const bool WARP      = SCROLL->queuedRefreshWarp;

        SCROLL->refreshQueued = false;
        SCROLL->queuedRefreshWorkspace.reset();
        SCROLL->queuedRefreshWarp = true;

        SCROLL->refreshWorkspaceEntries(PREFERRED, WARP);
        SCROLL->highlightHoverDebug(false);
    });
}

void CScrollOverview::queueViewportAnchorNormalization(PHLWORKSPACE workspace) {
    if (!workspace || !pMonitor || workspace != pMonitor->m_activeWorkspace)
        return;

    pendingAnchorWorkspace = workspace;

    const WP<IOverview> SELF = g_pOverview;
    viewOffset->setCallbackOnEnd([SELF](auto) {
        if (!SELF || !(SELF == g_pOverview))
            return;

        const auto SCROLL = dynamicPointerCast<CScrollOverview>(SELF).lock();
        if (!SCROLL || SCROLL->closing)
            return;

        SCROLL->viewOffset->setCallbackOnEnd(nullptr);

        const auto WORKSPACE = SCROLL->pendingAnchorWorkspace.lock();
        SCROLL->pendingAnchorWorkspace.reset();
        SCROLL->normalizeViewportAnchor(WORKSPACE);
    });
}

void CScrollOverview::normalizeViewportAnchor(PHLWORKSPACE workspace) {
    if (!workspace || !pMonitor || closing || workspace != pMonitor->m_activeWorkspace)
        return;

    const auto INDEX = workspaceEntryIndex(workspace);
    if (!INDEX || viewportCurrentWorkspace != *INDEX)
        return;

    startedOn = workspace;
    viewOffset->setCallbackOnEnd(nullptr);
    viewOffset->setValueAndWarp({});
    syncViewportWorkspaceFromOffset({});
    rebuildGeometryCache();
    damage();
}

void CScrollOverview::queueFocusedWindowCursorSync(PHLWINDOW window) {
    if (!window || closing)
        return;

    queuedCursorWindow = window;
    cursorSyncUntilMs  = Time::millis(Time::steadyNow()) + FOCUS_CURSOR_SYNC_MS;

    if (cursorSyncQueued)
        return;

    cursorSyncQueued = true;

    const WP<IOverview> SELF = g_pOverview;
    g_pEventLoopManager->doLater([SELF] {
        if (!SELF || !(SELF == g_pOverview))
            return;

        const auto SCROLL = dynamicPointerCast<CScrollOverview>(SELF).lock();
        if (!SCROLL || SCROLL->closing)
            return;

        const auto WINDOW        = SCROLL->queuedCursorWindow.lock();
        SCROLL->cursorSyncQueued = false;
        SCROLL->centerCursorOnWindowEntry(WINDOW);
    });
}

bool CScrollOverview::centerCursorOnWindowEntry(PHLWINDOW window) {
    if (!window || !pMonitor || closing || inputState.mode != ePointerMode::IDLE || !window->m_workspace || window->m_workspace->m_monitor != pMonitor)
        return false;

    ensureGeometryCache();

    const auto ENTRY = windowEntryForWindow(window);
    if (!ENTRY)
        return false;

    const auto TARGET = pMonitor->m_position + ENTRY->overviewBox.middle();
    if (TARGET.distance(g_pInputManager->getMouseCoordsInternal()) >= 0.5) {
        cursorSyncWarping = true;
        g_pCompositor->warpCursorTo(TARGET);
        cursorSyncWarping = false;
    }

    lastMousePosLocal = g_pInputManager->getMouseCoordsInternal() - pMonitor->m_position;
    highlightHoverDebug(false);
    damage();
    return true;
}

void CScrollOverview::clearFocusedWindowCursorSync() {
    queuedCursorWindow.reset();
    cursorSyncQueued  = false;
    cursorSyncUntilMs = 0;
    cursorSyncWarping = false;
}

void CScrollOverview::damage() {
    blockDamageReporting = true;
    g_pHyprRenderer->damageMonitor(pMonitor.lock());
    blockDamageReporting = false;
}

void CScrollOverview::onDamageReported() {
    if (!pMonitor || closing)
        return;

    damage();
    g_pCompositor->scheduleFrameForMonitor(pMonitor.lock());
}

void CScrollOverview::onDamageReported(const CRegion& damageRegion) {
    if (!pMonitor || closing)
        return;

    if (damageRegion.empty())
        return;

    damage();
    g_pCompositor->scheduleFrameForMonitor(pMonitor.lock());
}
