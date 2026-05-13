#include "ScrollOverview.hpp"
#include "../../plugin/Telemetry.hpp"
#include <algorithm>
#include <array>
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
    uint64_t         g_currentTelemetryFrame = 0;
    constexpr double MIN_LIVE_WINDOW_SIZE    = 3.0;

    void             flushCurrentRenderPass(PHLMONITOR monitor) {
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
        if (!Hyprview::telemetryEnabled())
            return;

        Hyprview::telemetryLog(std::format("frame={} event={} reason={} overviewBox={} intersects={} {}", g_currentTelemetryFrame, event, reason, Hyprview::formatBox(overviewBox),
                                           boolText(intersects), windowState(window)));
    }

    void renderBoxBorder(CBox box, PHLMONITOR monitor, const CHyprColor& color, double thickness) {
        if (!monitor || box.empty() || color.a <= 0.0 || thickness <= 0.0)
            return;

        thickness = std::min(thickness, std::min(box.w, box.h) / 2.0);
        if (thickness <= 0.0)
            return;

        const std::array<CBox, 4> EDGES = {
            CBox{box.x, box.y, box.w, thickness},
            CBox{box.x, box.y + box.h - thickness, box.w, thickness},
            CBox{box.x, box.y, thickness, box.h},
            CBox{box.x + box.w - thickness, box.y, thickness, box.h},
        };

        for (auto edge : EDGES) {
            edge.scale(monitor->m_scale).round();
            g_pHyprOpenGL->renderRect(edge, color, Render::GL::CHyprOpenGLImpl::SRectRenderData{.round = 0});
        }
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

PHLWINDOW CScrollOverview::overviewWindowToRender(PHLWINDOW window) const {
    if (!window)
        return nullptr;

    if (window->m_group)
        return window->m_group->current();

    return window;
}

PHLWINDOW CScrollOverview::windowForEntry(const SP<SWindowEntry>& image) const {
    if (!image)
        return nullptr;

    return overviewWindowToRender(image->pWindow.lock());
}

void CScrollOverview::rebuildWindowEntryLookups() const {
    if (!windowEntryLookupsDirty)
        return;

    rawWindowEntryLookup.clear();
    renderedWindowEntryLookup.clear();
    windowEntryWorkspaceLookup.clear();

    for (const auto& workspaceEntry : workspaceEntries) {
        if (!workspaceEntry)
            continue;

        for (const auto& entry : workspaceEntry->windowEntries) {
            if (!entry)
                continue;

            windowEntryWorkspaceLookup.emplace(entry.get(), workspaceEntry);

            if (const auto RAW_WINDOW = entry->pWindow.lock())
                rawWindowEntryLookup.emplace(RAW_WINDOW.get(), entry);

            if (const auto RENDERED_WINDOW = windowForEntry(entry))
                renderedWindowEntryLookup.emplace(RENDERED_WINDOW.get(), entry);
        }
    }

    windowEntryLookupsDirty = false;
}

void CScrollOverview::invalidateWindowEntryLookups() const {
    windowEntryLookupsDirty = true;
    rawWindowEntryLookup.clear();
    renderedWindowEntryLookup.clear();
    windowEntryWorkspaceLookup.clear();
}

SP<CScrollOverview::SWindowEntry> CScrollOverview::renderedWindowEntryForWindow(PHLWINDOW window) const {
    const auto TARGET = overviewWindowToRender(window);
    if (!TARGET)
        return nullptr;

    rebuildWindowEntryLookups();

    const auto IT = renderedWindowEntryLookup.find(TARGET.get());
    return IT == renderedWindowEntryLookup.end() ? nullptr : IT->second;
}

bool CScrollOverview::windowEntryRenderable(const SP<SWindowEntry>& image) const {
    return windowLiveRenderable(windowForEntry(image));
}

bool CScrollOverview::windowEntryVisible(const SP<SWindowEntry>& image) const {
    const auto WINDOW = windowForEntry(image);
    return image && WINDOW && image->liveRenderable && !image->overviewBox.empty() && overviewBoxIntersectsMonitor(image->overviewBox) &&
        !overviewWindowOccludedByFullscreen(WINDOW);
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
            Hyprview::telemetryLogLazy([&] {
                return std::format("frame={} event=force-surface-visibility-existing before={} {}", g_currentTelemetryFrame, Hyprview::formatRegion(HLSURFACE->m_visibleRegion),
                                   surfaceState(surface));
            });
            HLSURFACE->m_visibleRegion = {};
            Hyprview::telemetryLogLazy([&] {
                return std::format("frame={} event=force-surface-visibility-existing-after after={} {}", g_currentTelemetryFrame,
                                   Hyprview::formatRegion(HLSURFACE->m_visibleRegion), surfaceState(surface));
            });
            return;
        }
    }

    Hyprview::telemetryLogLazy([&] {
        return std::format("frame={} event=force-surface-visibility-new stored={} {}", g_currentTelemetryFrame, Hyprview::formatRegion(HLSURFACE->m_visibleRegion),
                           surfaceState(surface));
    });
    forcedSurfaceVisibility.push_back({surface, HLSURFACE->m_visibleRegion});
    HLSURFACE->m_visibleRegion = {};
    Hyprview::telemetryLogLazy([&] {
        return std::format("frame={} event=force-surface-visibility-new-after after={} {}", g_currentTelemetryFrame, Hyprview::formatRegion(HLSURFACE->m_visibleRegion),
                           surfaceState(surface));
    });
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

    if (!windowLiveRenderable(window)) {
        logWindowTelemetry("render-window-skip", window, box, INTERSECTS, "not-live-renderable");
        return false;
    }

    logWindowTelemetry("render-window-force-visible-before", window, box, INTERSECTS);
    forceWindowVisible(window);
    forceWindowSurfaceVisibility(window);
    logWindowTelemetry("render-window-force-visible-after", window, box, INTERSECTS);

    const auto   SAVED_POSITION        = window->m_realPosition->value();
    const auto   SAVED_SIZE            = window->m_realSize->value();
    auto         restoreWindowGeometry = Hyprutils::Utils::CScopeGuard([window, SAVED_POSITION, SAVED_SIZE] {
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

    Hyprview::telemetryLogLazy([&] {
        return std::format("frame={} event=render-window-submit renderBox={} sourceBox={} sourceRenderBox={} monitorScale={:.3f} windowPixelSize={:.2f},{:.2f} "
                           "renderScale={:.5f} translate={} {}",
                           g_currentTelemetryFrame, Hyprview::formatBox(renderBox), Hyprview::formatBox(sourceBox), Hyprview::formatBox(sourceRenderBox), MONITOR->m_scale,
                           ORIGINAL_WINDOW_WIDTH, ORIGINAL_WINDOW_HEIGHT, RENDER_SCALE, Hyprview::formatVector(RENDER_TRANSLATE), windowState(window));
    });

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

void CScrollOverview::renderWorkspaceLive(const SP<SWorkspaceEntry>& workspaceEntry, const Time::steady_tp& now) {
    if (!workspaceEntry || !workspaceEntry->pWorkspace || workspaceEntry->overviewBox.empty()) {
        Hyprview::telemetryLogLazy([&] { return std::format("frame={} event=workspace-skip reason=invalid-workspace-image", g_currentTelemetryFrame); });
        return;
    }

    const auto MONITOR = pMonitor.lock();

    const bool WORKSPACE_INTERSECTS = overviewBoxIntersectsMonitor(workspaceEntry->overviewBox);
    const bool TELEMETRY            = Hyprview::telemetryEnabled();
    const auto SELECTED_WINDOW      = TELEMETRY ? overviewWindowToRender(keyboardSelectedWindow.lock()) : PHLWINDOW{};
    const auto ACTIVE_WINDOW        = TELEMETRY ? overviewWindowToRender(Desktop::focusState()->window()) : PHLWINDOW{};
    double     telemetryContentPan  = 0.0;

    if (TELEMETRY) {
        const auto PAN_RANGE      = horizontalPanRangeForWorkspace(workspaceEntry);
        telemetryContentPan       = horizontalPanForWorkspace(workspaceEntry);
        const auto INDEX          = workspaceEntryIndex(workspaceEntry->pWorkspace);
        const auto SELECTED_ENTRY = renderedWindowEntryForWindow(SELECTED_WINDOW);
        const auto ACTIVE_ENTRY   = renderedWindowEntryForWindow(ACTIVE_WINDOW);
        const bool SELECTED_IN_WORKSPACE =
            SELECTED_WINDOW && windowBelongsToWorkspaceInOverview(SELECTED_WINDOW, workspaceEntry->pWorkspace) && workspaceEntryForWindowEntry(SELECTED_ENTRY) == workspaceEntry;
        const bool ACTIVE_IN_WORKSPACE =
            ACTIVE_WINDOW && windowBelongsToWorkspaceInOverview(ACTIVE_WINDOW, workspaceEntry->pWorkspace) && workspaceEntryForWindowEntry(ACTIVE_ENTRY) == workspaceEntry;

        Hyprview::telemetryLog(
            std::format("frame={} event=workspace-enter index={} workspace={} name={} scrolling={} overviewBox={} hitBox={} intersects={} pan={:.2f} panRange={:.2f},{:.2f} "
                        "selected={} selectedWorkspace={} selectedInWorkspace={} selectedBox={} active={} activeWorkspace={} activeInWorkspace={} activeBox={} windows={}",
                        g_currentTelemetryFrame, INDEX ? std::to_string(*INDEX) : "<none>", workspaceID(workspaceEntry->pWorkspace), workspaceEntry->pWorkspace->m_name,
                        boolText(workspaceUsesScrollingLayout(workspaceEntry->pWorkspace)), Hyprview::formatBox(workspaceEntry->overviewBox),
                        Hyprview::formatBox(workspaceEntry->hitBox), boolText(WORKSPACE_INTERSECTS), telemetryContentPan, PAN_RANGE.min, PAN_RANGE.max, windowID(SELECTED_WINDOW),
                        workspaceID(SELECTED_WINDOW ? SELECTED_WINDOW->m_workspace : PHLWORKSPACE{}), boolText(SELECTED_IN_WORKSPACE),
                        SELECTED_IN_WORKSPACE && SELECTED_ENTRY ? Hyprview::formatBox(SELECTED_ENTRY->overviewBox) : "<none>", windowID(ACTIVE_WINDOW),
                        workspaceID(ACTIVE_WINDOW ? ACTIVE_WINDOW->m_workspace : PHLWORKSPACE{}), boolText(ACTIVE_IN_WORKSPACE),
                        ACTIVE_IN_WORKSPACE && ACTIVE_ENTRY ? Hyprview::formatBox(ACTIVE_ENTRY->overviewBox) : "<none>", workspaceEntry->windowEntries.size()));
    }

    if (!WORKSPACE_INTERSECTS) {
        Hyprview::telemetryLogLazy([&] {
            return std::format("frame={} event=workspace-skip reason=outside-monitor workspace={} overviewBox={}", g_currentTelemetryFrame, workspaceID(workspaceEntry->pWorkspace),
                               Hyprview::formatBox(workspaceEntry->overviewBox));
        });
        return;
    }

    const auto WORKSPACE          = workspaceEntry->pWorkspace;
    const bool WAS_VISIBLE        = WORKSPACE->m_visible;
    const bool WAS_FORCE_RENDERED = WORKSPACE->m_forceRendering;
    WORKSPACE->m_visible          = true;
    WORKSPACE->m_forceRendering   = true;

    auto restoreWorkspace = Hyprutils::Utils::CScopeGuard([WORKSPACE, WAS_VISIBLE, WAS_FORCE_RENDERED] {
        WORKSPACE->m_visible        = WAS_VISIBLE;
        WORKSPACE->m_forceRendering = WAS_FORCE_RENDERED;
    });

    if (g_hyprviewConfig.scrolling.showWorkspaceLayers) {
        renderWorkspaceLayerLevel(workspaceEntry, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, now);
        renderWorkspaceLayerLevel(workspaceEntry, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, now);
    }

    std::vector<PHLWINDOW> renderedWindows;
    const auto             DRAGGED_WINDOW   = overviewWindowToRender((inputState.windowDrag ? inputState.windowDrag->window.lock() : PHLWINDOW{}));
    const bool             HAS_DRAG_PREVIEW = inputState.mode == ePointerMode::WINDOW_DRAG && !draggedLiveWindowBox().empty();
    size_t                 submittedWindows = 0;
    size_t                 skippedState     = 0;
    size_t                 skippedCull      = 0;
    size_t                 skippedTiny      = 0;
    size_t                 skippedDuplicate = 0;

    auto                   renderImage = [&](const SP<SWindowEntry>& entry) {
        if (!entry)
            return;

        const auto WINDOW     = windowForEntry(entry);
        const bool RENDERABLE = entry->liveRenderable;
        const bool INTERSECTS = entry ? overviewBoxIntersectsMonitor(entry->overviewBox) : false;
        if (TELEMETRY) {
            const CBox CURRENT_RAW = WINDOW && MONITOR && WINDOW->m_realPosition && WINDOW->m_realSize ?
                CBox{WINDOW->m_realPosition->value() - MONITOR->m_position, WINDOW->m_realSize->value()} :
                CBox{};
            const CBox GOAL_RAW =
                WINDOW && MONITOR && WINDOW->m_realPosition && WINDOW->m_realSize ? CBox{WINDOW->m_realPosition->goal() - MONITOR->m_position, WINDOW->m_realSize->goal()} : CBox{};
            logWindowTelemetry("window-candidate", WINDOW, entry ? entry->overviewBox : CBox{}, INTERSECTS,
                               std::format("renderable={} dragged={} pinnedFloating={} selected={} active={} workspaceMatch={} currentRaw={} goalRaw={} pan={:.2f} scale={:.5f}",
                                           boolText(RENDERABLE), boolText(WINDOW == DRAGGED_WINDOW), boolText(WINDOW && WINDOW->m_pinned && WINDOW->m_isFloating),
                                           boolText(WINDOW && WINDOW == SELECTED_WINDOW), boolText(WINDOW && WINDOW == ACTIVE_WINDOW),
                                           boolText(WINDOW && windowBelongsToWorkspaceInOverview(WINDOW, workspaceEntry->pWorkspace)), Hyprview::formatBox(CURRENT_RAW),
                                           Hyprview::formatBox(GOAL_RAW), telemetryContentPan, scale->value()));
        }

        if (!RENDERABLE || (HAS_DRAG_PREVIEW && WINDOW == DRAGGED_WINDOW) || (WINDOW->m_pinned && WINDOW->m_isFloating)) {
            ++skippedState;
            logWindowTelemetry("window-candidate-skip", WINDOW, entry ? entry->overviewBox : CBox{}, INTERSECTS,
                               !RENDERABLE ? "not-live-renderable" : (WINDOW == DRAGGED_WINDOW ? "dragged-window" : "pinned-floating"));
            return;
        }

        if (std::ranges::find(renderedWindows, WINDOW) != renderedWindows.end()) {
            ++skippedDuplicate;
            logWindowTelemetry("window-candidate-skip", WINDOW, entry->overviewBox, INTERSECTS, "duplicate-rendered-window");
            return;
        }

        if (!INTERSECTS) {
            ++skippedCull;
            logWindowTelemetry("window-candidate-skip", WINDOW, entry->overviewBox, INTERSECTS, "outside-monitor");
            return;
        }

        if (overviewBoxTooSmallForLiveRender(entry->overviewBox)) {
            ++skippedTiny;
            logWindowTelemetry("window-candidate-skip", WINDOW, entry->overviewBox, INTERSECTS, "tiny-window");
            return;
        }

        renderedWindows.emplace_back(WINDOW);
        if (renderWindowLive(WINDOW, entry->overviewBox, now))
            ++submittedWindows;
    };

    const auto FULLSCREEN_WINDOW = overviewWindowToRender(WORKSPACE->getFullscreenWindow());
    if (const auto FULLSCREEN_ENTRY = renderedWindowEntryForWindow(FULLSCREEN_WINDOW);
        FULLSCREEN_ENTRY && FULLSCREEN_ENTRY->liveRenderable && FULLSCREEN_WINDOW->m_workspace == WORKSPACE) {
        if (const auto ENTRY = renderedWindowEntryForWindow(FULLSCREEN_WINDOW))
            renderImage(ENTRY);

        for (const auto& entry : workspaceEntry->windowEntries) {
            const auto WINDOW = windowForEntry(entry);
            if (WINDOW && WINDOW->m_isFloating && WINDOW != FULLSCREEN_WINDOW)
                renderImage(entry);
        }

        return;
    }

    auto renderByState = [&](bool fullscreen, bool floating) {
        for (const auto& entry : workspaceEntry->windowEntries) {
            const auto WINDOW = windowForEntry(entry);
            if (!WINDOW || WINDOW->isFullscreen() != fullscreen || WINDOW->m_isFloating != floating)
                continue;

            renderImage(entry);
        }
    };

    renderByState(false, false);
    renderByState(false, true);
    renderByState(true, false);
    renderByState(true, true);

    Hyprview::telemetryLogLazy([&] {
        return std::format("frame={} event=workspace-render-summary workspace={} submitted={} skippedState={} skippedCull={} skippedTiny={} skippedDuplicate={}",
                           g_currentTelemetryFrame, workspaceID(workspaceEntry->pWorkspace), submittedWindows, skippedState, skippedCull, skippedTiny, skippedDuplicate);
    });
}

void CScrollOverview::renderDraggedWindowLive(const Time::steady_tp& now) {
    if (inputState.mode != ePointerMode::WINDOW_DRAG || !inputState.windowDrag)
        return;

    const auto WINDOW = overviewWindowToRender(inputState.windowDrag->window.lock());
    if (!WINDOW)
        return;

    const auto DRAG_BOX = draggedLiveWindowBox();
    if (DRAG_BOX.empty())
        return;

    const bool VALID_DROP = hasDropTarget();
    const auto ALPHA      = std::clamp(VALID_DROP ? g_hyprviewConfig.scrolling.dragAlpha : g_hyprviewConfig.scrolling.invalidDragAlpha, 0.0F, 1.0F);

    renderWindowLive(WINDOW, DRAG_BOX, now, ALPHA);

    if (!VALID_DROP)
        renderBoxBorder(DRAG_BOX, pMonitor.lock(), CHyprColor{g_hyprviewConfig.scrolling.invalidInsertionMarkerColor}, 4.0);
}

void CScrollOverview::renderPinnedFloatingWindowsLive(const Time::steady_tp& now) {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return;

    std::vector<PHLWINDOW> renderedWindows;
    const auto             DRAGGED_WINDOW = overviewWindowToRender((inputState.windowDrag ? inputState.windowDrag->window.lock() : PHLWINDOW{}));

    for (const auto& candidate : g_pCompositor->m_windows) {
        const auto WINDOW = overviewWindowToRender(candidate);
        if (!windowLiveRenderable(WINDOW) || !WINDOW->m_pinned || !WINDOW->m_isFloating || WINDOW->m_monitor != MONITOR || WINDOW == DRAGGED_WINDOW)
            continue;

        if (std::ranges::find(renderedWindows, WINDOW) != renderedWindows.end())
            continue;

        renderedWindows.emplace_back(WINDOW);

        if (const auto ENTRY = renderedWindowEntryForWindow(WINDOW)) {
            renderWindowLive(WINDOW, ENTRY->overviewBox, now);
            continue;
        }

        CBox box = {WINDOW->m_realPosition->value() - MONITOR->m_position, WINDOW->m_realSize->value()};
        renderWindowLive(WINDOW, box, now);
    }
}

void CScrollOverview::renderOverviewLive(const Time::steady_tp& now) {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return;

    rebuildGeometryCache();
    ++g_currentTelemetryFrame;

    Hyprview::telemetryLogLazy([&] {
        return std::format(
            "frame={} event=overview-start monitor={} monitorSize={} monitorPixel={} scale={:.5f} viewOffset={} workspaceEntries={} activeIndex={} viewportIndex={} closing={} "
            "swipe={} selected={} selectedWorkspace={} active={} activeWorkspace={}",
            g_currentTelemetryFrame, monitorName(MONITOR), Hyprview::formatVector(MONITOR->m_size), Hyprview::formatVector(MONITOR->m_pixelSize), scale->value(),
            Hyprview::formatVector(viewOffset->value()), workspaceEntries.size(), activeWorkspaceEntryIndex(), viewportCurrentWorkspace, boolText(closing), boolText(swipe),
            windowID(overviewWindowToRender(keyboardSelectedWindow.lock())),
            workspaceID(keyboardSelectedWindow && keyboardSelectedWindow->m_workspace ? keyboardSelectedWindow->m_workspace : PHLWORKSPACE{}),
            windowID(overviewWindowToRender(Desktop::focusState()->window())),
            workspaceID(Desktop::focusState()->window() && Desktop::focusState()->window()->m_workspace ? Desktop::focusState()->window()->m_workspace : PHLWORKSPACE{}));
    });

    {
        const bool PREVIOUS_BLOCK_SURFACE_FEEDBACK = g_pHyprRenderer->m_bBlockSurfaceFeedback;
        g_pHyprRenderer->m_bBlockSurfaceFeedback   = true;
        auto restoreSurfaceFeedback =
            Hyprutils::Utils::CScopeGuard([PREVIOUS_BLOCK_SURFACE_FEEDBACK] { g_pHyprRenderer->m_bBlockSurfaceFeedback = PREVIOUS_BLOCK_SURFACE_FEEDBACK; });
        auto restoreForcedVisibility = Hyprutils::Utils::CScopeGuard([this] {
            restoreForcedSurfaceVisibility();
            restoreForcedWindowVisibility();
        });

        renderLiveBackdrop(now);

        renderWorkspaceShadows();

        for (const auto& workspaceEntry : workspaceEntries)
            renderWorkspaceLive(workspaceEntry, now);

        const auto DRAGGED_WINDOW = overviewWindowToRender((inputState.windowDrag ? inputState.windowDrag->window.lock() : PHLWINDOW{}));
        for (const auto& workspaceEntry : workspaceEntries) {
            if (!workspaceEntry)
                continue;

            bool needsPrune = false;
            for (const auto& entry : workspaceEntry->windowEntries) {
                if (!entry || !entry->pWindow) {
                    needsPrune = true;
                    continue;
                }

                const auto WINDOW = windowForEntry(entry);
                if (!entry->liveVisible || WINDOW == DRAGGED_WINDOW || (WINDOW && WINDOW->m_pinned && WINDOW->m_isFloating))
                    continue;

                renderFocusIndicator(entry);
                renderActiveWindowIndicator(entry);
            }

            if (needsPrune) {
                std::erase_if(workspaceEntry->windowEntries, [](const auto& entry) { return !entry || !entry->pWindow; });
                invalidateWindowEntryLookups();
            }
        }

        renderWorkspaceAnnotations();
        renderDropTargetFeedback();
        renderDraggedWindowLive(now);
        renderPinnedFloatingWindowsLive(now);
        renderInsertionMarkers();

        sendOverviewFrameCallbacks(now);
    }

    renderHyprlandLayerPhase(now);
    Hyprview::telemetryLogLazy([&] { return std::format("frame={} event=overview-end", g_currentTelemetryFrame); });
}
