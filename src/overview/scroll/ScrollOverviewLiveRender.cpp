#include "ScrollOverview.hpp"
#include "../../plugin/Telemetry.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <string_view>
#include <vector>
#include <wlr-layer-shell-unstable-v1.hpp>

#define private   public
#define protected public
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/view/Group.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/view/Popup.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/RendererHintsPassElement.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>
#undef protected
#undef private

using Render::GL::g_pHyprOpenGL;
using Render::RENDER_PASS_ALL;

namespace {
    uint64_t g_currentTelemetryFrame = 0;
    constexpr double MIN_LIVE_WINDOW_SIZE = 3.0;

    void     flushCurrentRenderPass(PHLMONITOR monitor) {
        if (!monitor)
            return;

        g_pHyprRenderer->m_renderPass.render(CRegion{CBox{{}, monitor->m_transformedSize}});
        g_pHyprRenderer->m_renderPass.clear();
    }

    bool windowHasOverviewAnimation(PHLWINDOW window) {
        if (!window)
            return false;

        return window->m_realPosition->isBeingAnimated() || window->m_realSize->isBeingAnimated() || window->alpha(Desktop::View::WINDOW_ALPHA_FADE)->isBeingAnimated() ||
            window->alpha(Desktop::View::WINDOW_ALPHA_ACTIVE)->isBeingAnimated() || window->alpha(Desktop::View::WINDOW_ALPHA_FULLSCREEN)->isBeingAnimated() ||
            window->alpha(Desktop::View::WINDOW_ALPHA_LAYOUT)->isBeingAnimated() || window->alpha(Desktop::View::WINDOW_ALPHA_MOVE_TO_WORKSPACE)->isBeingAnimated() ||
            window->alpha(Desktop::View::WINDOW_ALPHA_MOVE_FROM_WORKSPACE)->isBeingAnimated() || window->m_borderFadeAnimationProgress->isBeingAnimated() ||
            window->m_borderAngleAnimationProgress->isBeingAnimated() || window->m_dimPercent->isBeingAnimated() || window->m_realShadowColor->isBeingAnimated();
    }

    bool layerHasOverviewAnimation(PHLLS layer) {
        if (!Desktop::View::validMapped(layer))
            return false;

        return layer->m_realPosition->isBeingAnimated() || layer->m_realSize->isBeingAnimated() || layer->m_alpha->isBeingAnimated();
    }

    std::string_view boolText(bool value) {
        return value ? "1" : "0";
    }

    std::string monitorName(PHLMONITOR monitor) {
        return monitor ? monitor->m_name : "<none>";
    }

    std::string workspaceID(PHLWORKSPACE workspace) {
        return workspace ? std::to_string(workspace->m_id) : "<none>";
    }

    std::string windowID(PHLWINDOW window) {
        return window ? Hyprview::formatRawPtr(window.get()) : "0";
    }

    std::string surfaceState(SP<CWLSurfaceResource> surface) {
        if (!surface)
            return "surface=0 resource=0 texture=0 visibleRegion=<none>";

        const auto HLSURFACE = Desktop::View::CWLSurface::fromResource(surface);
        return std::format("surface={:x} resource=1 texture={} buffer={} size={} visibleRegion={}", reinterpret_cast<uintptr_t>(surface.get()),
                           boolText(!!surface->m_current.texture), Hyprview::formatVector(surface->m_current.bufferSize), Hyprview::formatVector(surface->m_current.size),
                           HLSURFACE ? Hyprview::formatRegion(HLSURFACE->m_visibleRegion) : "<no-hlsurface>");
    }

    std::string windowState(PHLWINDOW window) {
        if (!window)
            return "window=0";

        const auto SURFACE   = window->wlSurface();
        const auto RESOURCE  = SURFACE ? SURFACE->resource() : nullptr;
        const auto WORKSPACE = window->m_workspace;
        const auto MONITOR   = window->m_monitor.lock();

        return std::format(
            "window={:x} workspace={} monitor={} mapped={} hidden={} suspended={} visible={} validMapped={} floating={} pinned={} fullscreen={} group={} realPos={} goalPos={} "
            "realSize={} goalSize={} {}",
            reinterpret_cast<uintptr_t>(window.get()), workspaceID(WORKSPACE), monitorName(MONITOR), boolText(window->m_isMapped), boolText(window->isHidden()),
            boolText(window->m_suspended), boolText(window->visible()), boolText(Desktop::View::validMapped(window)), boolText(window->m_isFloating), boolText(window->m_pinned),
            boolText(window->isFullscreen()), boolText(!!window->m_group), Hyprview::formatVector(window->m_realPosition->value()),
            Hyprview::formatVector(window->m_realPosition->goal()), Hyprview::formatVector(window->m_realSize->value()), Hyprview::formatVector(window->m_realSize->goal()),
            surfaceState(RESOURCE));
    }

    void logWindowTelemetry(std::string_view event, PHLWINDOW window, const CBox& overviewBox, bool intersects, std::string_view reason = "") {
        Hyprview::telemetryLog(std::format("frame={} event={} reason={} overviewBox={} intersects={} {}", g_currentTelemetryFrame, event, reason, Hyprview::formatBox(overviewBox),
                                           boolText(intersects), windowState(window)));
    }

    bool overviewBoxTooSmallForLiveRender(const CBox& box) {
        return box.w < MIN_LIVE_WINDOW_SIZE || box.h < MIN_LIVE_WINDOW_SIZE;
    }

    void projectWindowGeometryForLiveRender(PHLWINDOW window, PHLMONITOR monitor, const CBox& sourceBox) {
        if (!window || !monitor)
            return;

        const Vector2D WORKSPACE_OFFSET = !window->m_pinned && window->m_workspace ? window->m_workspace->m_renderOffset->value() : Vector2D{};
        window->m_realPosition->value() = monitor->m_position + sourceBox.pos() - WORKSPACE_OFFSET;
        window->m_realSize->value()     = sourceBox.size();
    }
}

bool CScrollOverview::snapshotFallbackActive() const {
    return false;
}

PHLWINDOW CScrollOverview::overviewWindowToRender(PHLWINDOW window) const {
    if (!window)
        return nullptr;

    if (window->m_group)
        return window->m_group->current();

    return window;
}

SP<CScrollOverview::SWindowImage> CScrollOverview::imageForRenderedWindow(PHLWINDOW window) const {
    const auto TARGET = overviewWindowToRender(window);
    if (!TARGET)
        return nullptr;

    for (const auto& wimg : images) {
        if (!wimg)
            continue;

        for (const auto& img : wimg->windowImages) {
            if (!img)
                continue;

            if (overviewWindowToRender(img->pWindow.lock()) == TARGET)
                return img;
        }
    }

    return nullptr;
}

bool CScrollOverview::overviewBoxIntersectsMonitor(const CBox& box) const {
    if (!pMonitor || box.w <= 0.0 || box.h <= 0.0)
        return false;

    const auto SIZE = pMonitor->m_size;
    return box.x < SIZE.x && box.x + box.w > 0.0 && box.y < SIZE.y && box.y + box.h > 0.0;
}

bool CScrollOverview::overviewWindowOccludedByFullscreen(PHLWINDOW window) const {
    window = overviewWindowToRender(window);
    if (!window || window->m_isFloating || !window->m_workspace)
        return false;

    const auto FULLSCREEN = overviewWindowToRender(window->m_workspace->getFullscreenWindow());
    return FULLSCREEN && FULLSCREEN != window && FULLSCREEN->m_workspace == window->m_workspace;
}

void CScrollOverview::forceSurfaceVisibility(SP<CWLSurfaceResource> surface) {
    if (!surface)
        return;

    const auto HLSURFACE = Desktop::View::CWLSurface::fromResource(surface);
    if (!HLSURFACE)
        return;

    for (const auto& entry : forcedSurfaceVisibility) {
        if (entry.surface == surface) {
            Hyprview::telemetryLog(std::format("frame={} event=force-surface-visibility-existing before={} {}", g_currentTelemetryFrame,
                                               Hyprview::formatRegion(HLSURFACE->m_visibleRegion), surfaceState(surface)));
            HLSURFACE->m_visibleRegion = {};
            Hyprview::telemetryLog(std::format("frame={} event=force-surface-visibility-existing-after after={} {}", g_currentTelemetryFrame,
                                               Hyprview::formatRegion(HLSURFACE->m_visibleRegion), surfaceState(surface)));
            return;
        }
    }

    Hyprview::telemetryLog(std::format("frame={} event=force-surface-visibility-new stored={} {}", g_currentTelemetryFrame, Hyprview::formatRegion(HLSURFACE->m_visibleRegion),
                                       surfaceState(surface)));
    forcedSurfaceVisibility.push_back({surface, HLSURFACE->m_visibleRegion});
    HLSURFACE->m_visibleRegion = {};
    Hyprview::telemetryLog(std::format("frame={} event=force-surface-visibility-new-after after={} {}", g_currentTelemetryFrame, Hyprview::formatRegion(HLSURFACE->m_visibleRegion),
                                       surfaceState(surface)));
}

void CScrollOverview::forceWindowSurfaceVisibility(PHLWINDOW window) {
    if (!window || !window->wlSurface() || !window->wlSurface()->resource())
        return;

    window->wlSurface()->resource()->breadthfirst([this](SP<CWLSurfaceResource> surface, const Vector2D&, void*) { forceSurfaceVisibility(surface); }, nullptr);

    if (window->m_isX11 || !window->m_popupHead)
        return;

    window->m_popupHead->breadthfirst(
        [this](WP<Desktop::View::CPopup> popup, void*) {
            if (!popup || !popup->aliveAndVisible() || !popup->wlSurface() || !popup->wlSurface()->resource())
                return;

            popup->wlSurface()->resource()->breadthfirst([this](SP<CWLSurfaceResource> surface, const Vector2D&, void*) { forceSurfaceVisibility(surface); }, nullptr);
        },
        nullptr);
}

void CScrollOverview::forceWindowVisible(PHLWINDOW window) {
    if (!window)
        return;

    for (const auto& entry : forcedWindowVisibility) {
        if (entry.window == window) {
            window->m_hidden = false;
            return;
        }
    }

    forcedWindowVisibility.push_back({window, window->m_hidden});
    window->m_hidden = false;
}

void CScrollOverview::restoreForcedSurfaceVisibility() {
    for (const auto& entry : forcedSurfaceVisibility) {
        const auto SURFACE = entry.surface;
        if (!SURFACE)
            continue;

        const auto HLSURFACE = Desktop::View::CWLSurface::fromResource(SURFACE);
        if (!HLSURFACE)
            continue;

        HLSURFACE->m_visibleRegion = entry.visibleRegion;
    }

    forcedSurfaceVisibility.clear();
}

void CScrollOverview::restoreForcedWindowVisibility() {
    std::vector<SP<Desktop::View::CGroup>> groupsToRefresh;

    for (const auto& entry : forcedWindowVisibility) {
        const auto WINDOW = entry.window.lock();
        if (!WINDOW)
            continue;

        if (WINDOW->m_group) {
            if (std::ranges::find(groupsToRefresh, WINDOW->m_group) == groupsToRefresh.end())
                groupsToRefresh.emplace_back(WINDOW->m_group);
            continue;
        }

        WINDOW->m_hidden = entry.hidden;
    }

    for (const auto& group : groupsToRefresh) {
        if (group)
            group->updateWindowVisibility();
    }

    forcedWindowVisibility.clear();
}

bool CScrollOverview::renderWindowLive(PHLWINDOW window, const CBox& box, const Time::steady_tp& now, double alpha) {
    const auto MONITOR    = pMonitor.lock();
    window                = overviewWindowToRender(window);
    const bool INTERSECTS = overviewBoxIntersectsMonitor(box);
    logWindowTelemetry("render-window-enter", window, box, INTERSECTS);

    if (!MONITOR || !window || box.empty() || !INTERSECTS) {
        logWindowTelemetry("render-window-skip", window, box, INTERSECTS, "invalid-monitor-window-or-box");
        return false;
    }

    if (overviewBoxTooSmallForLiveRender(box)) {
        logWindowTelemetry("render-window-skip", window, box, INTERSECTS, "tiny-window");
        return false;
    }

    if (!windowImageRenderable(window)) {
        if (const auto IMAGE = imageForRenderedWindow(window); IMAGE && IMAGE->fb) {
            logWindowTelemetry("render-window-fallback-snapshot", window, box, INTERSECTS, "not-live-renderable");
            renderWindowImage(IMAGE, box, alpha);
            return true;
        }

        logWindowTelemetry("render-window-skip", window, box, INTERSECTS, "not-live-renderable-no-snapshot");
        return false;
    }

    logWindowTelemetry("render-window-force-visible-before", window, box, INTERSECTS);
    forceWindowVisible(window);
    forceWindowSurfaceVisibility(window);
    logWindowTelemetry("render-window-force-visible-after", window, box, INTERSECTS);

    const auto SAVED_POSITION = window->m_realPosition->value();
    const auto SAVED_SIZE     = window->m_realSize->value();
    auto       restoreWindowGeometry = Hyprutils::Utils::CScopeGuard([window, SAVED_POSITION, SAVED_SIZE] {
        if (!window)
            return;

        window->m_realPosition->value() = SAVED_POSITION;
        window->m_realSize->value()     = SAVED_SIZE;
    });

    const double ORIGINAL_WINDOW_WIDTH  = std::max(1.0, SAVED_SIZE.x * MONITOR->m_scale);
    const double ORIGINAL_WINDOW_HEIGHT = std::max(1.0, SAVED_SIZE.y * MONITOR->m_scale);

    CBox         renderBox = box;
    renderBox.scale(MONITOR->m_scale).round();

    const float RENDER_SCALE = sc<float>(std::min(renderBox.w / ORIGINAL_WINDOW_WIDTH, renderBox.h / ORIGINAL_WINDOW_HEIGHT));
    CBox        sourceBox    = box;
    sourceBox.w              = SAVED_SIZE.x;
    sourceBox.h              = SAVED_SIZE.y;
    sourceBox.x              = box.x + box.w / 2.0 - sourceBox.w / 2.0;
    sourceBox.y              = box.y + box.h / 2.0 - sourceBox.h / 2.0;

    CBox sourceRenderBox = sourceBox;
    sourceRenderBox.scale(MONITOR->m_scale).round();

    const Vector2D RENDER_TRANSLATE = renderBox.pos() - sourceRenderBox.pos() * RENDER_SCALE;

    Hyprview::telemetryLog(std::format("frame={} event=render-window-submit renderBox={} sourceBox={} sourceRenderBox={} monitorScale={:.3f} windowPixelSize={:.2f},{:.2f} "
                                       "renderScale={:.5f} translate={} {}",
                                       g_currentTelemetryFrame, Hyprview::formatBox(renderBox), Hyprview::formatBox(sourceBox), Hyprview::formatBox(sourceRenderBox),
                                       MONITOR->m_scale, ORIGINAL_WINDOW_WIDTH, ORIGINAL_WINDOW_HEIGHT, RENDER_SCALE, Hyprview::formatVector(RENDER_TRANSLATE),
                                       windowState(window)));

    Render::SRenderModifData modif;
    modif.modifs.emplace_back(Render::SRenderModifData::RMOD_TYPE_SCALE, RENDER_SCALE);
    modif.modifs.emplace_back(Render::SRenderModifData::RMOD_TYPE_TRANSLATE, RENDER_TRANSLATE);

    projectWindowGeometryForLiveRender(window, MONITOR, sourceBox);
    g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = modif}));
    g_pHyprRenderer->renderWindow(window, MONITOR, now, true, RENDER_PASS_ALL, false, true);
    g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = Render::SRenderModifData{}}));
    flushCurrentRenderPass(MONITOR);

    return true;
}

void CScrollOverview::renderWorkspaceLive(const SP<SWorkspaceImage>& workspaceImage, const Time::steady_tp& now) {
    if (!workspaceImage || !workspaceImage->pWorkspace || workspaceImage->overviewBox.empty()) {
        Hyprview::telemetryLog(std::format("frame={} event=workspace-skip reason=invalid-workspace-image", g_currentTelemetryFrame));
        return;
    }

    const auto MONITOR = pMonitor.lock();

    const bool WORKSPACE_INTERSECTS = overviewBoxIntersectsMonitor(workspaceImage->overviewBox);
    const auto PAN_RANGE            = horizontalPanRangeForWorkspace(workspaceImage);
    const auto PAN                  = horizontalPanForWorkspace(workspaceImage);
    const auto INDEX                = workspaceImageIndex(workspaceImage->pWorkspace);
    const auto SELECTED_WINDOW      = overviewWindowToRender(keyboardSelectedWindow.lock());
    const auto ACTIVE_WINDOW        = overviewWindowToRender(Desktop::focusState()->window());
    const auto SELECTED_IMAGE       = imageForRenderedWindow(SELECTED_WINDOW);
    const auto ACTIVE_IMAGE         = imageForRenderedWindow(ACTIVE_WINDOW);
    const bool SELECTED_IN_WORKSPACE =
        SELECTED_WINDOW && windowBelongsToWorkspaceInOverview(SELECTED_WINDOW, workspaceImage->pWorkspace) && workspaceImageForWindowImage(SELECTED_IMAGE) == workspaceImage;
    const bool ACTIVE_IN_WORKSPACE =
        ACTIVE_WINDOW && windowBelongsToWorkspaceInOverview(ACTIVE_WINDOW, workspaceImage->pWorkspace) && workspaceImageForWindowImage(ACTIVE_IMAGE) == workspaceImage;

    Hyprview::telemetryLog(
        std::format("frame={} event=workspace-enter index={} workspace={} name={} scrolling={} overviewBox={} hitBox={} intersects={} pan={:.2f} panRange={:.2f},{:.2f} "
                    "selected={} selectedWorkspace={} selectedInWorkspace={} selectedBox={} active={} activeWorkspace={} activeInWorkspace={} activeBox={} windows={}",
                    g_currentTelemetryFrame, INDEX ? std::to_string(*INDEX) : "<none>", workspaceID(workspaceImage->pWorkspace), workspaceImage->pWorkspace->m_name,
                    boolText(workspaceUsesScrollingLayout(workspaceImage->pWorkspace)), Hyprview::formatBox(workspaceImage->overviewBox),
                    Hyprview::formatBox(workspaceImage->hitBox), boolText(WORKSPACE_INTERSECTS), PAN, PAN_RANGE.min, PAN_RANGE.max, windowID(SELECTED_WINDOW),
                    workspaceID(SELECTED_WINDOW ? SELECTED_WINDOW->m_workspace : PHLWORKSPACE{}), boolText(SELECTED_IN_WORKSPACE),
                    SELECTED_IN_WORKSPACE && SELECTED_IMAGE ? Hyprview::formatBox(SELECTED_IMAGE->overviewBox) : "<none>", windowID(ACTIVE_WINDOW),
                    workspaceID(ACTIVE_WINDOW ? ACTIVE_WINDOW->m_workspace : PHLWORKSPACE{}), boolText(ACTIVE_IN_WORKSPACE),
                    ACTIVE_IN_WORKSPACE && ACTIVE_IMAGE ? Hyprview::formatBox(ACTIVE_IMAGE->overviewBox) : "<none>", workspaceImage->windowImages.size()));

    if (!WORKSPACE_INTERSECTS) {
        Hyprview::telemetryLog(std::format("frame={} event=workspace-skip reason=outside-monitor workspace={} overviewBox={}", g_currentTelemetryFrame,
                                           workspaceID(workspaceImage->pWorkspace), Hyprview::formatBox(workspaceImage->overviewBox)));
        return;
    }

    const auto WORKSPACE          = workspaceImage->pWorkspace;
    const bool WAS_VISIBLE        = WORKSPACE->m_visible;
    const bool WAS_FORCE_RENDERED = WORKSPACE->m_forceRendering;
    WORKSPACE->m_visible          = true;
    WORKSPACE->m_forceRendering   = true;

    auto                   restoreWorkspace = Hyprutils::Utils::CScopeGuard([WORKSPACE, WAS_VISIBLE, WAS_FORCE_RENDERED] {
        WORKSPACE->m_visible        = WAS_VISIBLE;
        WORKSPACE->m_forceRendering = WAS_FORCE_RENDERED;
    });

    std::vector<PHLWINDOW> renderedWindows;
    const auto             DRAGGED_WINDOW = overviewWindowToRender(inputState.draggedWindow.lock());
    size_t                 submittedWindows = 0;
    size_t                 skippedState     = 0;
    size_t                 skippedCull      = 0;
    size_t                 skippedTiny      = 0;
    size_t                 skippedDuplicate = 0;

    auto                   renderImage = [&](const SP<SWindowImage>& img) {
        if (!img)
            return;

        const auto WINDOW     = overviewWindowToRender(img->pWindow.lock());
        const bool RENDERABLE = windowImageRenderable(WINDOW);
        const bool INTERSECTS = img ? overviewBoxIntersectsMonitor(img->overviewBox) : false;
        const CBox CURRENT_RAW =
            WINDOW && MONITOR && WINDOW->m_realPosition && WINDOW->m_realSize ? CBox{WINDOW->m_realPosition->value() - MONITOR->m_position, WINDOW->m_realSize->value()} : CBox{};
        const CBox GOAL_RAW =
            WINDOW && MONITOR && WINDOW->m_realPosition && WINDOW->m_realSize ? CBox{WINDOW->m_realPosition->goal() - MONITOR->m_position, WINDOW->m_realSize->goal()} : CBox{};
        logWindowTelemetry("window-candidate", WINDOW, img ? img->overviewBox : CBox{}, INTERSECTS,
                           std::format("renderable={} dragged={} pinnedFloating={} selected={} active={} workspaceMatch={} currentRaw={} goalRaw={} pan={:.2f} scale={:.5f}",
                                       boolText(RENDERABLE), boolText(WINDOW == DRAGGED_WINDOW), boolText(WINDOW && WINDOW->m_pinned && WINDOW->m_isFloating),
                                       boolText(WINDOW && WINDOW == SELECTED_WINDOW), boolText(WINDOW && WINDOW == ACTIVE_WINDOW),
                                       boolText(WINDOW && windowBelongsToWorkspaceInOverview(WINDOW, workspaceImage->pWorkspace)), Hyprview::formatBox(CURRENT_RAW),
                                       Hyprview::formatBox(GOAL_RAW), PAN, scale->value()));

        if (!RENDERABLE || WINDOW == DRAGGED_WINDOW || (WINDOW->m_pinned && WINDOW->m_isFloating)) {
            ++skippedState;
            logWindowTelemetry("window-candidate-skip", WINDOW, img ? img->overviewBox : CBox{}, INTERSECTS,
                               !RENDERABLE ? "not-live-renderable" : (WINDOW == DRAGGED_WINDOW ? "dragged-window" : "pinned-floating"));
            return;
        }

        if (std::ranges::find(renderedWindows, WINDOW) != renderedWindows.end()) {
            ++skippedDuplicate;
            logWindowTelemetry("window-candidate-skip", WINDOW, img->overviewBox, INTERSECTS, "duplicate-rendered-window");
            return;
        }

        if (!INTERSECTS) {
            ++skippedCull;
            logWindowTelemetry("window-candidate-skip", WINDOW, img->overviewBox, INTERSECTS, "outside-monitor");
            return;
        }

        if (overviewBoxTooSmallForLiveRender(img->overviewBox)) {
            ++skippedTiny;
            logWindowTelemetry("window-candidate-skip", WINDOW, img->overviewBox, INTERSECTS, "tiny-window");
            return;
        }

        renderedWindows.emplace_back(WINDOW);
        if (renderWindowLive(WINDOW, img->overviewBox, now))
            ++submittedWindows;
    };

    const auto FULLSCREEN_WINDOW = overviewWindowToRender(WORKSPACE->getFullscreenWindow());
    if (windowImageRenderable(FULLSCREEN_WINDOW) && FULLSCREEN_WINDOW->m_workspace == WORKSPACE) {
        if (const auto IMAGE = imageForRenderedWindow(FULLSCREEN_WINDOW))
            renderImage(IMAGE);

        for (const auto& img : workspaceImage->windowImages) {
            const auto WINDOW = overviewWindowToRender(img ? img->pWindow.lock() : nullptr);
            if (WINDOW && WINDOW->m_isFloating && WINDOW != FULLSCREEN_WINDOW)
                renderImage(img);
        }

        return;
    }

    auto renderByState = [&](bool fullscreen, bool floating) {
        for (const auto& img : workspaceImage->windowImages) {
            const auto WINDOW = overviewWindowToRender(img ? img->pWindow.lock() : nullptr);
            if (!WINDOW || WINDOW->isFullscreen() != fullscreen || WINDOW->m_isFloating != floating)
                continue;

            renderImage(img);
        }
    };

    renderByState(false, false);
    renderByState(false, true);
    renderByState(true, false);
    renderByState(true, true);

    Hyprview::telemetryLog(std::format("frame={} event=workspace-render-summary workspace={} submitted={} skippedState={} skippedCull={} skippedTiny={} skippedDuplicate={}",
                                       g_currentTelemetryFrame, workspaceID(workspaceImage->pWorkspace), submittedWindows, skippedState, skippedCull, skippedTiny, skippedDuplicate));
}

void CScrollOverview::renderDraggedWindowLive(const Time::steady_tp& now) {
    if (inputState.mode != ePointerMode::WINDOW_DRAG || !inputState.draggedImage)
        return;

    const auto WINDOW = overviewWindowToRender(inputState.draggedWindow.lock() ? inputState.draggedWindow.lock() : inputState.draggedImage->pWindow.lock());
    if (!WINDOW)
        return;

    const auto DRAG_BOX   = draggedWindowBox();
    const bool VALID_DROP = hasDropTarget();
    const auto ALPHA      = std::clamp(VALID_DROP ? g_hyprviewConfig.scrolling.dragAlpha : g_hyprviewConfig.scrolling.invalidDragAlpha, 0.0F, 1.0F);

    renderWindowLive(WINDOW, DRAG_BOX, now, ALPHA);

    CBox texbox = DRAG_BOX;
    texbox.scale(pMonitor->m_scale).round();
    g_pHyprOpenGL->renderRect(texbox, VALID_DROP ? CHyprColor{g_hyprviewConfig.scrolling.hoverColor} : CHyprColor{g_hyprviewConfig.scrolling.invalidInsertionMarkerColor},
                              Render::GL::CHyprOpenGLImpl::SRectRenderData{.round = 5});
}

void CScrollOverview::renderPinnedFloatingWindowsLive(const Time::steady_tp& now) {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return;

    std::vector<PHLWINDOW> renderedWindows;
    const auto             DRAGGED_WINDOW = overviewWindowToRender(inputState.draggedWindow.lock());

    for (const auto& candidate : g_pCompositor->m_windows) {
        const auto WINDOW = overviewWindowToRender(candidate);
        if (!windowImageRenderable(WINDOW) || !WINDOW->m_pinned || !WINDOW->m_isFloating || WINDOW->m_monitor != MONITOR || WINDOW == DRAGGED_WINDOW)
            continue;

        if (std::ranges::find(renderedWindows, WINDOW) != renderedWindows.end())
            continue;

        renderedWindows.emplace_back(WINDOW);

        if (const auto IMAGE = imageForRenderedWindow(WINDOW)) {
            renderWindowLive(WINDOW, IMAGE->overviewBox, now);
            continue;
        }

        CBox box = {WINDOW->m_realPosition->value() - MONITOR->m_position, WINDOW->m_realSize->value()};
        renderWindowLive(WINDOW, box, now);
    }
}

void CScrollOverview::renderHyprlandLayerPhase(const Time::steady_tp& now) {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return;

    const bool PREVIOUS_BLOCK_SURFACE_FEEDBACK = g_pHyprRenderer->m_bBlockSurfaceFeedback;
    g_pHyprRenderer->m_bBlockSurfaceFeedback   = false;
    auto restoreSurfaceFeedback  = Hyprutils::Utils::CScopeGuard([PREVIOUS_BLOCK_SURFACE_FEEDBACK] { g_pHyprRenderer->m_bBlockSurfaceFeedback = PREVIOUS_BLOCK_SURFACE_FEEDBACK; });
    auto restoreForcedVisibility = Hyprutils::Utils::CScopeGuard([this] { restoreForcedSurfaceVisibility(); });

    auto forceLayerSurfaceTreeVisibility = [&](PHLLS layer, bool popups) {
        if (!layer)
            return;

        if (!popups && layer->wlSurface() && layer->wlSurface()->resource())
            layer->wlSurface()->resource()->breadthfirst([this](SP<CWLSurfaceResource> surface, const Vector2D&, void*) { forceSurfaceVisibility(surface); }, nullptr);

        if (!popups || !layer->m_popupHead)
            return;

        layer->m_popupHead->breadthfirst(
            [this](WP<Desktop::View::CPopup> popup, void*) {
                if (!popup || !popup->aliveAndVisible() || !popup->wlSurface() || !popup->wlSurface()->resource())
                    return;

                popup->wlSurface()->resource()->breadthfirst([this](SP<CWLSurfaceResource> surface, const Vector2D&, void*) { forceSurfaceVisibility(surface); }, nullptr);
            },
            nullptr);
    };

    auto renderLayerLevel = [&](uint32_t layer, bool popups = false) {
        for (const auto& layerRef : MONITOR->m_layerSurfaceLayers[layer]) {
            const auto LAYER = layerRef.lock();
            if (!LAYER)
                continue;

            if (!Desktop::View::validMapped(LAYER))
                continue;

            forceLayerSurfaceTreeVisibility(LAYER, popups);
            g_pHyprRenderer->renderLayer(LAYER, MONITOR, now, popups);
        }
    };

    renderLayerLevel(ZWLR_LAYER_SHELL_V1_LAYER_TOP);

    for (auto const& imePopup : g_pInputManager->m_relay.m_inputMethodPopups)
        g_pHyprRenderer->renderIMEPopup(imePopup.get(), MONITOR, now);

    renderLayerLevel(ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY);

    for (uint32_t layer = ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND; layer <= ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY; ++layer)
        renderLayerLevel(layer, true);

    g_pHyprRenderer->renderDragIcon(MONITOR, now);
    flushCurrentRenderPass(MONITOR);
}

void CScrollOverview::renderOverviewLive(const Time::steady_tp& now) {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return;

    rebuildGeometryCache();
    ++g_currentTelemetryFrame;

    Hyprview::telemetryLog(
        std::format("frame={} event=overview-start monitor={} monitorSize={} monitorPixel={} scale={:.5f} viewOffset={} images={} activeIndex={} viewportIndex={} closing={} "
                    "swipe={} selected={} selectedWorkspace={} active={} activeWorkspace={}",
                    g_currentTelemetryFrame, monitorName(MONITOR), Hyprview::formatVector(MONITOR->m_size), Hyprview::formatVector(MONITOR->m_pixelSize), scale->value(),
                    Hyprview::formatVector(viewOffset->value()), images.size(), activeWorkspaceImageIndex(), viewportCurrentWorkspace, boolText(closing), boolText(swipe),
                    windowID(overviewWindowToRender(keyboardSelectedWindow.lock())),
                    workspaceID(keyboardSelectedWindow && keyboardSelectedWindow->m_workspace ? keyboardSelectedWindow->m_workspace : PHLWORKSPACE{}),
                    windowID(overviewWindowToRender(Desktop::focusState()->window())),
                    workspaceID(Desktop::focusState()->window() && Desktop::focusState()->window()->m_workspace ? Desktop::focusState()->window()->m_workspace : PHLWORKSPACE{})));

    {
        const bool PREVIOUS_BLOCK_SURFACE_FEEDBACK = g_pHyprRenderer->m_bBlockSurfaceFeedback;
        g_pHyprRenderer->m_bBlockSurfaceFeedback   = true;
        auto restoreSurfaceFeedback =
            Hyprutils::Utils::CScopeGuard([PREVIOUS_BLOCK_SURFACE_FEEDBACK] { g_pHyprRenderer->m_bBlockSurfaceFeedback = PREVIOUS_BLOCK_SURFACE_FEEDBACK; });
        auto restoreForcedVisibility = Hyprutils::Utils::CScopeGuard([this] {
            restoreForcedSurfaceVisibility();
            restoreForcedWindowVisibility();
        });

        clearCurrentRenderTarget(CHyprColor{g_hyprviewConfig.scrolling.backdropColor});

        CBox texbox = {{}, MONITOR->m_size};
        texbox.scale(MONITOR->m_scale).round();
        CRegion damage{0, 0, INT16_MAX, INT16_MAX};
        if (backgroundFb)
            g_pHyprOpenGL->renderTextureInternal(backgroundFb->getTexture(), texbox, {.damage = &damage, .a = 1.0});

        renderWorkspaceShadows();

        for (const auto& wimg : images)
            renderWorkspaceLive(wimg, now);

        const auto DRAGGED_WINDOW = overviewWindowToRender(inputState.draggedWindow.lock());
        for (const auto& wimg : images) {
            if (!wimg)
                continue;

            bool dirty = false;
            for (const auto& img : wimg->windowImages) {
                if (!img || !img->pWindow) {
                    dirty = true;
                    continue;
                }

                const auto WINDOW = overviewWindowToRender(img->pWindow.lock());
                if (!windowImageRenderable(WINDOW) || WINDOW == DRAGGED_WINDOW || overviewWindowOccludedByFullscreen(WINDOW) || (WINDOW->m_pinned && WINDOW->m_isFloating) ||
                    !overviewBoxIntersectsMonitor(img->overviewBox))
                    continue;

                renderFocusIndicator(img);
                renderActiveWindowIndicator(img);
            }

            if (dirty)
                std::erase_if(wimg->windowImages, [](const auto& img) { return !img || !img->pWindow; });
        }

        renderWorkspaceAnnotations();
        renderDropTargetFeedback();
        renderDraggedWindowLive(now);
        renderPinnedFloatingWindowsLive(now);
        renderInsertionMarkers();

        sendOverviewFrameCallbacks(now);
    }

    renderHyprlandLayerPhase(now);
    Hyprview::telemetryLog(std::format("frame={} event=overview-end", g_currentTelemetryFrame));
}
