#include "ScrollOverview.hpp"
#include "../../plugin/Telemetry.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <string_view>
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

    const auto ENTRY = renderedWindowEntryForWindow(window);
    if (!ENTRY || ENTRY->overviewBox.empty())
        return false;

    return windowEntryIntersectsWorkspaceViewport(ENTRY, workspaceEntryForWindowEntry(ENTRY)) && !overviewWindowOccludedByFullscreen(window);
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

bool CScrollOverview::hasVisibleRealtimePreviewCallbacks() const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || closing)
        return false;

    auto visibleWindowHasCallbacks = [this, MONITOR](PHLWINDOW window) {
        window = overviewWindowToRender(window);
        if (!windowLiveRenderable(window) || !overviewWindowVisible(window) || window->m_monitor != MONITOR)
            return false;

        return surfaceTreeHasFrameCallbacks(window->wlSurface() ? window->wlSurface()->resource() : nullptr);
    };

    for (const auto& workspaceEntry : workspaceEntries) {
        if (!workspaceIntersectsViewport(workspaceEntry))
            continue;

        for (const auto& entry : workspaceEntry->windowEntries) {
            if (entry && visibleWindowHasCallbacks(entry->pWindow.lock()))
                return true;
        }
    }

    for (const auto& window : g_pCompositor->m_windows) {
        const auto WINDOW = overviewWindowToRender(window);
        if (!WINDOW || !WINDOW->m_pinned || !WINDOW->m_isFloating)
            continue;

        if (visibleWindowHasCallbacks(WINDOW))
            return true;
    }

    return false;
}

PHLWINDOW CScrollOverview::closeTargetWindow() const {
    return overviewWindowToRender(closeFrameWindow.lock());
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

bool CScrollOverview::sendCloseTargetFrameCallback(const Time::steady_tp& now) {
    const auto MONITOR = pMonitor.lock();
    const auto WINDOW  = closeTargetWindow();
    if (!MONITOR || !windowLiveRenderable(WINDOW) || WINDOW->m_monitor != MONITOR)
        return false;

    const auto SURFACE = WINDOW->wlSurface() ? WINDOW->wlSurface()->resource() : nullptr;
    if (!surfaceTreeHasFrameCallbacks(SURFACE))
        return false;

    const bool PREVIOUS_SENDING   = sendingOverviewFrameCallbacks;
    sendingOverviewFrameCallbacks = true;

    surfaceTreePresent(SURFACE, MONITOR, now);

    sendingOverviewFrameCallbacks = PREVIOUS_SENDING;
    lastRealtimePreviewFrame      = now;
    realtimePreviewFrameQueued    = false;
    return true;
}

bool CScrollOverview::shouldHandleSurfaceDamage(SP<CWLSurfaceResource> surface) {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || closing || !surface)
        return true;

    const auto OWNER = overviewSurfaceOwner(surface);
    if (OWNER.type == eOverviewSurfaceOwner::UNKNOWN)
        return true;

    auto ownerName = [](eOverviewSurfaceOwner type) -> std::string_view {
        switch (type) {
            case eOverviewSurfaceOwner::WINDOW: return "window";
            case eOverviewSurfaceOwner::LAYER: return "layer";
            case eOverviewSurfaceOwner::WINDOW_POPUP: return "window-popup";
            case eOverviewSurfaceOwner::LAYER_POPUP: return "layer-popup";
            case eOverviewSurfaceOwner::UNKNOWN: break;
        }

        return "unknown";
    };

    auto logDecision = [&](bool allow, std::string_view reason) {
        if (!Hyprview::telemetryEnabled())
            return;

        const auto WINDOW        = overviewWindowToRender(OWNER.window);
        const auto ENTRY         = renderedWindowEntryForWindow(WINDOW);
        const auto LAYER_MONITOR = OWNER.layer ? OWNER.layer->m_monitor.lock() : PHLMONITOR{};
        Hyprview::telemetryLog(std::format(
            "event=surface-damage-decision allow={} reason={} owner={} surface={:x} overviewMonitor={} ownerMonitor={} window={:x} windowWorkspace={} image={} imageBox={} "
            "imageIntersects={} occluded={} layer={:x} layerNs={} layerLevel={} layerMapped={} layerValidMapped={}",
            allow ? 1 : 0, reason, ownerName(OWNER.type), reinterpret_cast<uintptr_t>(surface.get()), MONITOR ? MONITOR->m_name : "<none>",
            OWNER.monitor ? OWNER.monitor->m_name : "<none>", WINDOW ? reinterpret_cast<uintptr_t>(WINDOW.get()) : 0,
            WINDOW && WINDOW->m_workspace ? std::to_string(WINDOW->m_workspace->m_id) : "<none>", ENTRY ? 1 : 0, ENTRY ? Hyprview::formatBox(ENTRY->overviewBox) : "<none>",
            ENTRY ? windowEntryIntersectsWorkspaceViewport(ENTRY, workspaceEntryForWindowEntry(ENTRY)) : false, WINDOW ? overviewWindowOccludedByFullscreen(WINDOW) : false,
            OWNER.layer ? reinterpret_cast<uintptr_t>(OWNER.layer.get()) : 0, OWNER.layer ? OWNER.layer->m_namespace : "<none>", OWNER.layer ? sc<int>(OWNER.layer->m_layer) : -1,
            OWNER.layer ? OWNER.layer->m_mapped : false, OWNER.layer ? Desktop::View::validMapped(OWNER.layer) : false));
    };

    if (!surfaceOwnerBelongsToOverviewMonitor(OWNER, MONITOR)) {
        logDecision(true, "owner-outside-overview-monitor");
        return true;
    }

    if (OWNER.type == eOverviewSurfaceOwner::LAYER || OWNER.type == eOverviewSurfaceOwner::LAYER_POPUP) {
        if (!OWNER.layer) {
            logDecision(false, "missing-layer-owner");
            return false;
        }

        if (OWNER.layer->m_layer > ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM) {
            damage();
            g_pCompositor->scheduleFrameForMonitor(MONITOR);
            logDecision(false, "external-layer-reroute-to-overview");
            return false;
        }

        logDecision(true, "background-bottom-layer-native");
        return true;
    }

    auto WINDOW = overviewWindowToRender(OWNER.window);
    if (!WINDOW || WINDOW->m_monitor != MONITOR) {
        logDecision(false, "missing-window-or-monitor-mismatch");
        return false;
    }

    if (WINDOW->m_pinned && WINDOW->m_isFloating) {
        damage();
        g_pCompositor->scheduleFrameForMonitor(MONITOR);
        logDecision(false, "pinned-floating-reroute-to-overview");
        return false;
    }

    const auto ENTRY = renderedWindowEntryForWindow(WINDOW);
    if (!ENTRY || overviewWindowOccludedByFullscreen(WINDOW) || !windowEntryIntersectsWorkspaceViewport(ENTRY, workspaceEntryForWindowEntry(ENTRY))) {
        logDecision(false, "window-not-visible-in-overview");
        return false;
    }

    damage();
    g_pCompositor->scheduleFrameForMonitor(MONITOR);
    logDecision(false, "overview-window-reroute-to-overview");
    return false;
}

bool CScrollOverview::shouldAllowSurfaceFrame(SP<CWLSurfaceResource> surface, const Time::steady_tp& now) {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || closing || !surface)
        return true;

    const auto OWNER = overviewSurfaceOwner(surface);
    if (OWNER.type == eOverviewSurfaceOwner::UNKNOWN)
        return true;

    auto ownerName = [](eOverviewSurfaceOwner type) -> std::string_view {
        switch (type) {
            case eOverviewSurfaceOwner::WINDOW: return "window";
            case eOverviewSurfaceOwner::LAYER: return "layer";
            case eOverviewSurfaceOwner::WINDOW_POPUP: return "window-popup";
            case eOverviewSurfaceOwner::LAYER_POPUP: return "layer-popup";
            case eOverviewSurfaceOwner::UNKNOWN: break;
        }

        return "unknown";
    };

    auto logDecision = [&](bool allow, std::string_view reason) {
        if (!Hyprview::telemetryEnabled())
            return;

        const auto WINDOW = overviewWindowToRender(OWNER.window);
        const auto ENTRY  = renderedWindowEntryForWindow(WINDOW);
        Hyprview::telemetryLog(std::format(
            "event=surface-frame-decision allow={} reason={} owner={} surface={:x} overviewMonitor={} ownerMonitor={} window={:x} windowWorkspace={} image={} imageBox={} "
            "imageIntersects={} overviewWindowVisible={} sendingOverviewFrameCallbacks={} layer={:x} layerNs={} layerLevel={} layerMapped={} layerValidMapped={}",
            allow ? 1 : 0, reason, ownerName(OWNER.type), reinterpret_cast<uintptr_t>(surface.get()), MONITOR ? MONITOR->m_name : "<none>",
            OWNER.monitor ? OWNER.monitor->m_name : "<none>", WINDOW ? reinterpret_cast<uintptr_t>(WINDOW.get()) : 0,
            WINDOW && WINDOW->m_workspace ? std::to_string(WINDOW->m_workspace->m_id) : "<none>", ENTRY ? 1 : 0, ENTRY ? Hyprview::formatBox(ENTRY->overviewBox) : "<none>",
            ENTRY ? windowEntryIntersectsWorkspaceViewport(ENTRY, workspaceEntryForWindowEntry(ENTRY)) : false, WINDOW ? overviewWindowVisible(WINDOW) : false,
            sendingOverviewFrameCallbacks ? 1 : 0, OWNER.layer ? reinterpret_cast<uintptr_t>(OWNER.layer.get()) : 0, OWNER.layer ? OWNER.layer->m_namespace : "<none>",
            OWNER.layer ? sc<int>(OWNER.layer->m_layer) : -1, OWNER.layer ? OWNER.layer->m_mapped : false, OWNER.layer ? Desktop::View::validMapped(OWNER.layer) : false));
    };

    if (!surfaceOwnerBelongsToOverviewMonitor(OWNER, MONITOR)) {
        logDecision(true, "owner-outside-overview-monitor");
        return true;
    }

    if (OWNER.type == eOverviewSurfaceOwner::LAYER || OWNER.type == eOverviewSurfaceOwner::LAYER_POPUP) {
        logDecision(true, "external-layer-frame-native");
        return true;
    }

    const auto WINDOW = overviewWindowToRender(OWNER.window);
    if (!WINDOW) {
        logDecision(true, "missing-window");
        return true;
    }

    if (!windowLiveRenderable(WINDOW)) {
        logDecision(false, "window-not-live-renderable");
        return false;
    }

    if (!overviewWindowVisible(WINDOW)) {
        logDecision(false, "window-not-visible");
        return false;
    }

    if (sendingOverviewFrameCallbacks) {
        logDecision(true, "overview-frame-callback-pass-through");
        return true;
    }

    scheduleRealtimePreviewFrame();
    logDecision(false, "visible-window-throttle-native-frame");
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

    if (!hasVisibleRealtimePreviewCallbacks()) {
        realtimePreviewFrameQueued = false;
        return false;
    }

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
        if (!windowLiveRenderable(WINDOW) || WINDOW->m_monitor != MONITOR || !overviewWindowVisible(WINDOW))
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

    if (!OVERVIEW->hasVisibleRealtimePreviewCallbacks())
        return 0;

    OVERVIEW->damage();

    return 0;
}

void CScrollOverview::sendOverviewFrameCallbacks(const Time::steady_tp& now) {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return;

    if (closing) {
        sendCloseTargetFrameCallback(now);
        return;
    }

    bool       sentWindowFrame    = false;
    const bool CAN_FRAME_WINDOW   = shouldAllowRealtimePreviewFrame();
    const bool PREVIOUS_SENDING   = sendingOverviewFrameCallbacks;
    sendingOverviewFrameCallbacks = CAN_FRAME_WINDOW;
    std::vector<PHLWINDOW> framedWindows;

    auto                   frameWindow = [&](PHLWINDOW window) {
        window = overviewWindowToRender(window);
        if (!windowLiveRenderable(window) || !overviewWindowVisible(window))
            return;

        const auto SURFACE = window->wlSurface() ? window->wlSurface()->resource() : nullptr;
        if (!surfaceTreeHasFrameCallbacks(SURFACE))
            return;

        if (std::ranges::find(framedWindows, window) != framedWindows.end())
            return;

        framedWindows.emplace_back(window);

        if (!CAN_FRAME_WINDOW) {
            scheduleRealtimePreviewFrame();
            return;
        }

        surfaceTreePresent(SURFACE, MONITOR, now);
        sentWindowFrame = true;
    };

    for (const auto& workspaceEntry : workspaceEntries) {
        if (!workspaceIntersectsViewport(workspaceEntry))
            continue;

        for (const auto& entry : workspaceEntry->windowEntries) {
            if (!entry)
                continue;

            frameWindow(entry->pWindow.lock());
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
