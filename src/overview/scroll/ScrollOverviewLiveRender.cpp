#include "ScrollOverview.hpp"
#include <algorithm>
#include <cmath>
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
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/RendererHintsPassElement.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>
#undef protected
#undef private

using Render::GL::g_pHyprOpenGL;
using Render::RENDER_PASS_ALL;

namespace {
    void flushCurrentRenderPass(PHLMONITOR monitor) {
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
            const auto SIZE = surface->m_current.bufferSize.x > 0 && surface->m_current.bufferSize.y > 0 ? surface->m_current.bufferSize : surface->m_current.size;
            if (SIZE.x > 0 && SIZE.y > 0)
                HLSURFACE->m_visibleRegion = CRegion{0, 0, SIZE.x, SIZE.y};
            return;
        }
    }

    forcedSurfaceVisibility.push_back({surface, HLSURFACE->m_visibleRegion});
    const auto SIZE = surface->m_current.bufferSize.x > 0 && surface->m_current.bufferSize.y > 0 ? surface->m_current.bufferSize : surface->m_current.size;
    if (SIZE.x > 0 && SIZE.y > 0)
        HLSURFACE->m_visibleRegion = CRegion{0, 0, SIZE.x, SIZE.y};
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
    const auto MONITOR = pMonitor.lock();
    window             = overviewWindowToRender(window);
    if (!MONITOR || !window || box.empty() || !overviewBoxIntersectsMonitor(box))
        return false;

    if (!windowImageRenderable(window)) {
        if (const auto IMAGE = imageForRenderedWindow(window); IMAGE && IMAGE->fb) {
            renderWindowImage(IMAGE, box, alpha);
            return true;
        }

        return false;
    }

    forceWindowVisible(window);
    forceWindowSurfaceVisibility(window);

    CBox renderBox = box;
    renderBox.scale(MONITOR->m_scale).round();

    const double             WINDOW_WIDTH  = std::max(1.0, window->m_realSize->value().x * MONITOR->m_scale);
    const double             WINDOW_HEIGHT = std::max(1.0, window->m_realSize->value().y * MONITOR->m_scale);
    const float              RENDER_SCALE  = sc<float>(std::min(renderBox.w / WINDOW_WIDTH, renderBox.h / WINDOW_HEIGHT));

    Render::SRenderModifData modif;
    modif.modifs.emplace_back(Render::SRenderModifData::RMOD_TYPE_SCALE, RENDER_SCALE);
    modif.modifs.emplace_back(Render::SRenderModifData::RMOD_TYPE_TRANSLATE, renderBox.pos());

    g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = modif}));
    g_pHyprRenderer->renderWindow(window, MONITOR, now, true, RENDER_PASS_ALL, true, true);
    g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = Render::SRenderModifData{}}));
    flushCurrentRenderPass(MONITOR);

    return true;
}

void CScrollOverview::renderWorkspaceLive(const SP<SWorkspaceImage>& workspaceImage, const Time::steady_tp& now) {
    if (!workspaceImage || !workspaceImage->pWorkspace || workspaceImage->overviewBox.empty() || !overviewBoxIntersectsMonitor(workspaceImage->overviewBox))
        return;

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

    auto                   renderImage = [&](const SP<SWindowImage>& img) {
        if (!img)
            return;

        const auto WINDOW = overviewWindowToRender(img->pWindow.lock());
        if (!windowImageRenderable(WINDOW) || WINDOW == DRAGGED_WINDOW || (WINDOW->m_pinned && WINDOW->m_isFloating))
            return;

        if (std::ranges::find(renderedWindows, WINDOW) != renderedWindows.end())
            return;

        renderedWindows.emplace_back(WINDOW);
        renderWindowLive(WINDOW, img->overviewBox, now);
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
}
