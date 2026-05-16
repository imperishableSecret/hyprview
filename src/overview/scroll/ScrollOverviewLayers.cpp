#include "ScrollOverview.hpp"
#include "../../plugin/HyprviewConfig.hpp"

#include <algorithm>
#include <cstdint>
#include <wlr-layer-shell-unstable-v1.hpp>

#define private   public
#define protected public
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/view/Popup.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>
#undef protected
#undef private

using Render::GL::g_pHyprOpenGL;

namespace {
    void flushCurrentRenderPass(PHLMONITOR monitor) {
        if (!monitor)
            return;

        g_pHyprRenderer->m_renderPass.render(CRegion{CBox{{}, monitor->m_transformedSize}});
        g_pHyprRenderer->m_renderPass.clear();
    }
}

void CScrollOverview::forceLayerSurfaceTreeVisibility(PHLLS layer, bool popups) {
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
}

void CScrollOverview::renderLiveBackdrop(const Time::steady_tp& now) {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return;

    clearCurrentRenderTarget(CHyprColor{g_hyprviewConfig.scrolling.backdropColor});
    renderBackdropLayerLevel(ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, now);

    if (!g_hyprviewConfig.scrolling.backgroundBlur)
        return;

    CRegion blurDamage{0, 0, sc<int>(MONITOR->m_transformedSize.x), sc<int>(MONITOR->m_transformedSize.y)};
    g_pHyprRenderer->preBlurForCurrentMonitor(&blurDamage);

    const auto BLURRED_TEX = MONITOR->resources()->m_blurFB->getTexture();
    if (!BLURRED_TEX)
        return;

    clearCurrentRenderTarget(CHyprColor{g_hyprviewConfig.scrolling.backdropColor});

    CBox texbox = {{}, MONITOR->m_size};
    texbox.scale(MONITOR->m_scale).round();
    CRegion renderDamage{0, 0, INT16_MAX, INT16_MAX};
    g_pHyprOpenGL->renderTextureInternal(BLURRED_TEX, texbox, {.damage = &renderDamage, .a = 1.0});
}

void CScrollOverview::renderBackdropLayer(PHLLS layer, const Time::steady_tp& now) {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || !layer)
        return;

    if (!Desktop::View::validMapped(layer))
        return;

    const auto LAYER_MONITOR = layer->m_monitor.lock();
    if (LAYER_MONITOR && LAYER_MONITOR != MONITOR)
        return;

    forceLayerSurfaceTreeVisibility(layer, false);
    g_pHyprRenderer->renderLayer(layer, MONITOR, now, false);
}

void CScrollOverview::renderBackdropLayerLevel(uint32_t layer, const Time::steady_tp& now) {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || layer >= MONITOR->m_layerSurfaceLayers.size())
        return;

    for (const auto& layerRef : MONITOR->m_layerSurfaceLayers[layer]) {
        const auto LAYER = layerRef.lock();
        renderBackdropLayer(LAYER, now);
    }

    flushCurrentRenderPass(MONITOR);
}

void CScrollOverview::renderWorkspaceLayer(PHLLS layer, const SP<SWorkspaceEntry>& workspaceEntry, const Time::steady_tp& now) {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || !layer || !workspaceEntry || workspaceEntry->overviewBox.empty())
        return;

    if (!Desktop::View::validMapped(layer))
        return;

    const auto LAYER_MONITOR = layer->m_monitor.lock();
    if (LAYER_MONITOR && LAYER_MONITOR != MONITOR)
        return;

    const auto OVERVIEW_BOX = workspaceEntry->overviewBox;
    if (!workspaceIntersectsViewport(workspaceEntry))
        return;

    const double LAYER_SCALE = std::min(OVERVIEW_BOX.w / std::max(MONITOR->m_size.x, 1.0), OVERVIEW_BOX.h / std::max(MONITOR->m_size.y, 1.0));
    if (LAYER_SCALE <= 0.0)
        return;

    const Vector2D SAVED_POSITION = layer->m_realPosition->value();
    const Vector2D SAVED_SIZE     = layer->m_realSize->value();
    const Vector2D LAYER_OFFSET   = SAVED_POSITION - MONITOR->m_position;

    layer->m_realPosition->value() = MONITOR->m_position + OVERVIEW_BOX.pos() + LAYER_OFFSET * LAYER_SCALE;
    layer->m_realSize->value()     = SAVED_SIZE * LAYER_SCALE;

    auto restoreLayerGeometry = Hyprutils::Utils::CScopeGuard([layer, SAVED_POSITION, SAVED_SIZE] {
        layer->m_realPosition->value() = SAVED_POSITION;
        layer->m_realSize->value()     = SAVED_SIZE;
    });

    forceLayerSurfaceTreeVisibility(layer, false);
    g_pHyprRenderer->renderLayer(layer, MONITOR, now, false);
}

void CScrollOverview::renderWorkspaceLayerLevel(const SP<SWorkspaceEntry>& workspaceEntry, uint32_t layer, const Time::steady_tp& now) {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || layer >= MONITOR->m_layerSurfaceLayers.size())
        return;

    const CBox WORKSPACE_CLIP = workspaceRenderClipBox(workspaceEntry);
    if (WORKSPACE_CLIP.empty())
        return;

    for (const auto& layerRef : MONITOR->m_layerSurfaceLayers[layer]) {
        const auto LAYER = layerRef.lock();
        renderWorkspaceLayer(LAYER, workspaceEntry, now);
    }

    flushCurrentRenderPass(MONITOR);
}

void CScrollOverview::renderHyprlandLayerPhase(const Time::steady_tp& now) {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return;

    const bool PREVIOUS_BLOCK_SURFACE_FEEDBACK = g_pHyprRenderer->m_bBlockSurfaceFeedback;
    g_pHyprRenderer->m_bBlockSurfaceFeedback   = false;
    auto restoreSurfaceFeedback  = Hyprutils::Utils::CScopeGuard([PREVIOUS_BLOCK_SURFACE_FEEDBACK] { g_pHyprRenderer->m_bBlockSurfaceFeedback = PREVIOUS_BLOCK_SURFACE_FEEDBACK; });
    auto restoreForcedVisibility = Hyprutils::Utils::CScopeGuard([this] { restoreForcedSurfaceVisibility(); });

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
