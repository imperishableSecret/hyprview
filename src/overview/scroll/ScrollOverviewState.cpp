#include "ScrollOverview.hpp"
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

void                      CScrollOverview::refreshWorkspaceImages(PHLWORKSPACE preferredViewport, bool warpViewport) {
    if (!pMonitor)
        return;

    const auto FALLBACK_VIEWPORT =
        !preferredViewport && viewportCurrentWorkspace < images.size() && images[viewportCurrentWorkspace] ? images[viewportCurrentWorkspace]->pWorkspace : PHLWORKSPACE{};

    images.clear();

    for (const auto& w : g_pCompositor->getWorkspaces()) {
        if (w && w->m_monitor == pMonitor && !w->m_isSpecialWorkspace && workspaceVisibleInOverview(w.lock()))
            images.emplace_back(makeShared<SWorkspaceImage>(w.lock()));
    }

    std::sort(images.begin(), images.end(), [](const auto& a, const auto& b) { return a->pWorkspace->m_id < b->pWorkspace->m_id; });
    pruneWorkspaceContentPans();

    refreshingWorkspaceImages = true;
    redrawAll();
    refreshingWorkspaceImages = false;
    rebuildGeometryCache();
    syncSelectionToViewport(false);

    const auto VIEWPORT_WORKSPACE = preferredViewport ? preferredViewport : FALLBACK_VIEWPORT;
    bool       centeredPreferred  = false;
    bool       movedViewport      = false;
    const bool NORMALIZE_ANCHOR   = VIEWPORT_WORKSPACE && !warpViewport && pMonitor->m_activeWorkspace == VIEWPORT_WORKSPACE;
    if (VIEWPORT_WORKSPACE) {
        if (const auto INDEX = workspaceImageIndex(VIEWPORT_WORKSPACE); INDEX) {
            if (NORMALIZE_ANCHOR)
                queueViewportAnchorNormalization(VIEWPORT_WORKSPACE);

            movedViewport     = setViewportWorkspace(*INDEX, warpViewport);
            centeredPreferred = true;
        } else if (!images.empty())
            movedViewport = setViewportWorkspace(std::min(viewportCurrentWorkspace, images.size() - 1), true);
    }

    if (centeredPreferred && NORMALIZE_ANCHOR) {
        if (!movedViewport)
            normalizeViewportAnchor(VIEWPORT_WORKSPACE);
    }

    damage();
}

void CScrollOverview::queueRefreshWorkspaceImages(PHLWORKSPACE preferredViewport, bool warpViewport) {
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

        SCROLL->refreshWorkspaceImages(PREFERRED, WARP);
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

    const auto INDEX = workspaceImageIndex(workspace);
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
        SCROLL->centerCursorOnWindowImage(WINDOW);
    });
}

bool CScrollOverview::centerCursorOnWindowImage(PHLWINDOW window) {
    if (!window || !pMonitor || closing || inputState.mode != ePointerMode::IDLE || !window->m_workspace || window->m_workspace->m_monitor != pMonitor)
        return false;

    rebuildGeometryCache();

    const auto IMAGE = imageForWindow(window);
    if (!IMAGE)
        return false;

    const auto TARGET = pMonitor->m_position + IMAGE->overviewBox.middle();
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

    bool marked = false;
    for (const auto& wimg : images) {
        if (!wimg)
            continue;

        for (const auto& img : wimg->windowImages) {
            if (!img || !windowImageRenderable(img->pWindow.lock()))
                continue;

            img->dirty = true;
            marked     = true;
        }
    }

    if (marked) {
        damageDirty = true;
        damage();
        g_pCompositor->scheduleFrameForMonitor(pMonitor.lock());
    }
}

void CScrollOverview::onDamageReported(const CRegion& damageRegion) {
    if (!pMonitor || closing)
        return;

    if (markDirtyWindowImagesForDamage(damageRegion)) {
        damageDirty = true;
        damage();
        g_pCompositor->scheduleFrameForMonitor(pMonitor.lock());
    }
}
