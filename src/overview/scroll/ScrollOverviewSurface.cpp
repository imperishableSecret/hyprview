#include "ScrollOverview.hpp"
#include <algorithm>
#include <chrono>
#include <utility>
#include <vector>
#include <wayland-server-core.h>
#include <wlr-layer-shell-unstable-v1.hpp>

#define private   public
#define protected public
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/view/Popup.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#undef protected
#undef private

static constexpr std::chrono::milliseconds OVERVIEW_WINDOW_FRAME_INTERVAL = std::chrono::milliseconds(33);
static constexpr std::chrono::milliseconds OVERVIEW_IDLE_FRAME_INTERVAL   = std::chrono::milliseconds(100);

static bool                                windowHasOverviewAnimation(PHLWINDOW window) {
    if (!window)
        return false;

    return window->m_realPosition->isBeingAnimated() || window->m_realSize->isBeingAnimated() || window->alpha(Desktop::View::WINDOW_ALPHA_FADE)->isBeingAnimated() ||
        window->alpha(Desktop::View::WINDOW_ALPHA_ACTIVE)->isBeingAnimated() || window->alpha(Desktop::View::WINDOW_ALPHA_FULLSCREEN)->isBeingAnimated() ||
        window->alpha(Desktop::View::WINDOW_ALPHA_LAYOUT)->isBeingAnimated() || window->alpha(Desktop::View::WINDOW_ALPHA_MOVE_TO_WORKSPACE)->isBeingAnimated() ||
        window->alpha(Desktop::View::WINDOW_ALPHA_MOVE_FROM_WORKSPACE)->isBeingAnimated() || window->m_borderFadeAnimationProgress->isBeingAnimated() ||
        window->m_borderAngleAnimationProgress->isBeingAnimated() || window->m_dimPercent->isBeingAnimated() || window->m_realShadowColor->isBeingAnimated();
}

static bool layerHasOverviewAnimation(PHLLS layer) {
    if (!Desktop::View::validMapped(layer))
        return false;

    return layer->m_realPosition->isBeingAnimated() || layer->m_realSize->isBeingAnimated() || layer->m_alpha->isBeingAnimated();
}

CScrollOverview::SOverviewSurfaceOwner CScrollOverview::overviewSurfaceOwner(SP<CWLSurfaceResource> surface) const {
    if (!surface)
        return {};

    const auto HLSURFACE = Desktop::View::CWLSurface::fromResource(surface);
    if (!HLSURFACE)
        return {};

    const auto VIEW = HLSURFACE->view();
    if (!VIEW)
        return {};

    if (const auto LAYER = Desktop::View::CLayerSurface::fromView(VIEW))
        return {.type = eOverviewSurfaceOwner::LAYER, .layer = LAYER, .monitor = LAYER->m_monitor.lock()};

    if (const auto WINDOW = Desktop::View::CWindow::fromView(VIEW))
        return {.type = eOverviewSurfaceOwner::WINDOW, .window = WINDOW, .monitor = WINDOW->m_monitor.lock()};

    const auto POPUP = Desktop::View::CPopup::fromView(VIEW);
    if (!POPUP)
        return {};

    const auto OWNER = POPUP->getT1Owner();
    if (!OWNER || !OWNER->view())
        return {};

    if (const auto LAYER = Desktop::View::CLayerSurface::fromView(OWNER->view()))
        return {.type = eOverviewSurfaceOwner::LAYER_POPUP, .layer = LAYER, .monitor = LAYER->m_monitor.lock()};

    if (const auto WINDOW = Desktop::View::CWindow::fromView(OWNER->view()))
        return {.type = eOverviewSurfaceOwner::WINDOW_POPUP, .window = WINDOW, .monitor = WINDOW->m_monitor.lock()};

    return {};
}

bool CScrollOverview::surfaceOwnerBelongsToOverviewMonitor(const SOverviewSurfaceOwner& owner, PHLMONITOR monitor) const {
    if (!monitor || owner.type == eOverviewSurfaceOwner::UNKNOWN)
        return false;

    if (owner.monitor && owner.monitor != monitor)
        return false;

    if ((owner.type == eOverviewSurfaceOwner::LAYER || owner.type == eOverviewSurfaceOwner::LAYER_POPUP) && owner.layer) {
        const auto LAYER_MONITOR = owner.layer->m_monitor.lock();
        return !LAYER_MONITOR || LAYER_MONITOR == monitor;
    }

    if ((owner.type == eOverviewSurfaceOwner::WINDOW || owner.type == eOverviewSurfaceOwner::WINDOW_POPUP) && owner.window)
        return owner.window->m_monitor == monitor;

    return false;
}

bool CScrollOverview::overviewWindowVisible(PHLWINDOW window) const {
    window = overviewWindowToRender(window);
    if (!window || !pMonitor)
        return false;

    if (window->m_pinned && window->m_isFloating)
        return window->m_monitor == pMonitor;

    const auto IMAGE = imageForRenderedWindow(window);
    if (!IMAGE || IMAGE->overviewBox.empty())
        return false;

    return overviewBoxIntersectsMonitor(IMAGE->overviewBox) && !overviewWindowOccludedByFullscreen(window);
}

bool CScrollOverview::surfaceTreeHasFrameCallbacks(SP<CWLSurfaceResource> surface) const {
    if (!surface)
        return false;

    bool hasCallbacks = false;
    surface->breadthfirst(
        [&hasCallbacks](SP<CWLSurfaceResource> child, const Vector2D&, void*) {
            if (child && !child->m_current.callbacks.empty())
                hasCallbacks = true;
        },
        nullptr);

    return hasCallbacks;
}

void CScrollOverview::surfaceTreePresent(SP<CWLSurfaceResource> surface, PHLMONITOR monitor, const Time::steady_tp& now) {
    if (!surface || !monitor)
        return;

    std::pair<PHLMONITOR, Time::steady_tp> data = {monitor, now};
    surface->breadthfirst(
        [](SP<CWLSurfaceResource> child, const Vector2D&, void* data) {
            if (!child)
                return;

            const auto [MONITOR, NOW] = *static_cast<std::pair<PHLMONITOR, Time::steady_tp>*>(data);
            child->presentFeedback(NOW, MONITOR, false);
        },
        &data);
}

bool CScrollOverview::shouldHandleSurfaceDamage(SP<CWLSurfaceResource> surface) {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || closing || !surface)
        return true;

    const auto OWNER = overviewSurfaceOwner(surface);
    if (OWNER.type == eOverviewSurfaceOwner::UNKNOWN)
        return true;

    if (!surfaceOwnerBelongsToOverviewMonitor(OWNER, MONITOR))
        return true;

    if (OWNER.type == eOverviewSurfaceOwner::LAYER || OWNER.type == eOverviewSurfaceOwner::LAYER_POPUP) {
        if (!OWNER.layer)
            return false;

        if (OWNER.layer->m_layer > ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM) {
            damage();
            g_pCompositor->scheduleFrameForMonitor(MONITOR);
            return false;
        }

        return true;
    }

    auto WINDOW = overviewWindowToRender(OWNER.window);
    if (!WINDOW || WINDOW->m_monitor != MONITOR)
        return false;

    if (WINDOW->m_pinned && WINDOW->m_isFloating) {
        damage();
        g_pCompositor->scheduleFrameForMonitor(MONITOR);
        return false;
    }

    const auto IMAGE = imageForRenderedWindow(WINDOW);
    if (!IMAGE || overviewWindowOccludedByFullscreen(WINDOW) || !overviewBoxIntersectsMonitor(IMAGE->overviewBox))
        return false;

    damage();
    g_pCompositor->scheduleFrameForMonitor(MONITOR);
    return false;
}

bool CScrollOverview::shouldAllowSurfaceFrame(SP<CWLSurfaceResource> surface, const Time::steady_tp& now) {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || closing || !surface)
        return true;

    const auto OWNER = overviewSurfaceOwner(surface);
    if (OWNER.type == eOverviewSurfaceOwner::UNKNOWN)
        return true;

    if (!surfaceOwnerBelongsToOverviewMonitor(OWNER, MONITOR))
        return true;

    if (OWNER.type == eOverviewSurfaceOwner::LAYER || OWNER.type == eOverviewSurfaceOwner::LAYER_POPUP)
        return true;

    const auto WINDOW = overviewWindowToRender(OWNER.window);
    if (!WINDOW)
        return true;

    if (!overviewWindowVisible(WINDOW)) {
        scheduleRealtimePreviewFrame();
        return false;
    }

    if (sendingOverviewFrameCallbacks)
        return true;

    scheduleRealtimePreviewFrame();
    return false;
}

bool CScrollOverview::shouldAllowRealtimePreviewFrame() const {
    if (lastRealtimePreviewFrame.time_since_epoch().count() == 0)
        return true;

    return Time::steadyNow() - lastRealtimePreviewFrame >= OVERVIEW_WINDOW_FRAME_INTERVAL;
}

bool CScrollOverview::shouldAllowRealtimePreviewSchedule() {
    if (closing)
        return true;

    if (scale->isBeingAnimated() || viewOffset->isBeingAnimated())
        return true;

    if (realtimePreviewFrameQueued) {
        scheduleRealtimePreviewFrame();
        return false;
    }

    if (shouldAllowRealtimePreviewFrame()) {
        realtimePreviewFrameQueued = true;
        return true;
    }

    scheduleRealtimePreviewFrame();
    return false;
}

bool CScrollOverview::shouldSuppressRenderDamage() const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || closing)
        return false;

    if (scale->isBeingAnimated() || viewOffset->isBeingAnimated())
        return false;

    for (const auto& window : g_pCompositor->m_windows) {
        const auto WINDOW = overviewWindowToRender(window);
        if (!windowImageRenderable(WINDOW) || WINDOW->m_monitor != MONITOR || !overviewWindowVisible(WINDOW))
            continue;

        if (windowHasOverviewAnimation(WINDOW))
            return false;
    }

    for (const auto LAYER : {ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, ZWLR_LAYER_SHELL_V1_LAYER_TOP, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY}) {
        for (const auto& layerRef : MONITOR->m_layerSurfaceLayers[LAYER]) {
            if (layerHasOverviewAnimation(layerRef.lock()))
                return false;
        }
    }

    return true;
}

void CScrollOverview::schedulePreviewFrameAfter(std::chrono::milliseconds delay) {
    if (!realtimePreviewTimer)
        return;

    const auto DELAY = std::max<int>(1, static_cast<int>(delay.count()));
    const auto DUE   = Time::steadyNow() + std::chrono::milliseconds(DELAY);

    if (realtimePreviewTimerArmed && realtimePreviewTimerDue <= DUE)
        return;

    realtimePreviewTimerArmed = true;
    realtimePreviewTimerDue   = DUE;
    wl_event_source_timer_update(realtimePreviewTimer, DELAY);
}

void CScrollOverview::scheduleMinimumPreviewFrame() {
    if (closing)
        return;

    schedulePreviewFrameAfter(OVERVIEW_IDLE_FRAME_INTERVAL);
}

void CScrollOverview::scheduleRealtimePreviewFrame() {
    const auto NOW     = Time::steadyNow();
    const auto ELAPSED = lastRealtimePreviewFrame.time_since_epoch().count() == 0 ? OVERVIEW_WINDOW_FRAME_INTERVAL :
                                                                                    std::chrono::duration_cast<std::chrono::milliseconds>(NOW - lastRealtimePreviewFrame);
    const auto DELAY   = OVERVIEW_WINDOW_FRAME_INTERVAL - std::min(ELAPSED, OVERVIEW_WINDOW_FRAME_INTERVAL);
    schedulePreviewFrameAfter(DELAY);
}

int CScrollOverview::realtimePreviewTimerCallback(void* data) {
    const auto OVERVIEW = static_cast<CScrollOverview*>(data);
    if (!OVERVIEW)
        return 0;

    OVERVIEW->realtimePreviewTimerArmed  = false;
    OVERVIEW->realtimePreviewTimerDue    = {};
    OVERVIEW->realtimePreviewFrameQueued = false;

    if (OVERVIEW->closing || !OVERVIEW->pMonitor)
        return 0;

    OVERVIEW->damage();
    OVERVIEW->scheduleMinimumPreviewFrame();

    return 0;
}

void CScrollOverview::sendOverviewFrameCallbacks(const Time::steady_tp& now) {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || closing)
        return;

    bool       sentWindowFrame    = false;
    const bool CAN_FRAME_WINDOW   = shouldAllowRealtimePreviewFrame();
    const bool PREVIOUS_SENDING   = sendingOverviewFrameCallbacks;
    sendingOverviewFrameCallbacks = CAN_FRAME_WINDOW;
    std::vector<PHLWINDOW> framedWindows;

    auto                   frameWindow = [&](PHLWINDOW window) {
        window = overviewWindowToRender(window);
        if (!windowImageRenderable(window) || !overviewWindowVisible(window))
            return;

        if (std::ranges::find(framedWindows, window) != framedWindows.end())
            return;

        framedWindows.emplace_back(window);

        if (!CAN_FRAME_WINDOW) {
            scheduleRealtimePreviewFrame();
            return;
        }

        surfaceTreePresent(window->wlSurface() ? window->wlSurface()->resource() : nullptr, MONITOR, now);
        sentWindowFrame = true;
    };

    for (const auto& wimg : images) {
        if (!wimg || wimg->overviewBox.empty() || !wimg->overviewBox.overlaps(CBox{{}, MONITOR->m_size}))
            continue;

        for (const auto& img : wimg->windowImages) {
            if (!img)
                continue;

            frameWindow(img->pWindow.lock());
        }
    }

    for (const auto& window : g_pCompositor->m_windows) {
        const auto WINDOW = overviewWindowToRender(window);
        if (!WINDOW || !WINDOW->m_pinned || !WINDOW->m_isFloating || WINDOW->m_monitor != MONITOR)
            continue;

        frameWindow(WINDOW);
    }

    sendingOverviewFrameCallbacks = PREVIOUS_SENDING;

    if (sentWindowFrame)
        lastRealtimePreviewFrame = now;

    realtimePreviewFrameQueued = false;
}
