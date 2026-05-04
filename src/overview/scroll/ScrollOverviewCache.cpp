#include "ScrollOverview.hpp"
#include <algorithm>
#include <cmath>

#define private   public
#define protected public
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/protocols/XDGShell.hpp>
#include <hyprland/src/xwayland/XSurface.hpp>
#undef protected
#undef private
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>

using Render::GL::g_pHyprOpenGL;
using Render::RENDER_PASS_ALL;

static constexpr DRMFormat OVERVIEW_FB_FORMAT = DRM_FORMAT_ABGR8888;

static void                flushCurrentRenderPass() {
    g_pHyprRenderer->m_renderData.damage = g_pHyprRenderer->m_renderPass.render(g_pHyprRenderer->m_renderData.damage);
    g_pHyprRenderer->m_renderPass.clear();
}

void CScrollOverview::redrawWorkspace(PHLWORKSPACE workspace, bool forcelowres) {
    if (!refreshingWorkspaceImages && pMonitor->m_activeWorkspace != startedOn && !closing) {
        // likely user changed.
        onWorkspaceChange();
    }

    blockOverviewRendering = true;

    g_pHyprOpenGL->makeEGLCurrent();

    auto image = imageForWorkspace(workspace);

    if (!image) {
        blockOverviewRendering = false;
        return;
    }

    image->windowImages.clear();

    std::vector<PHLWINDOW> tiledWindows;
    std::vector<PHLWINDOW> floatingWindows;
    for (const auto& w : g_pCompositor->m_windows) {
        if (!windowBelongsToWorkspaceInOverview(w, workspace))
            continue;

        if (w->m_isFloating)
            floatingWindows.emplace_back(w);
        else
            tiledWindows.emplace_back(w);
    }

    std::vector<PHLWINDOW> windows;
    windows.reserve(tiledWindows.size() + floatingWindows.size());
    windows.insert(windows.end(), tiledWindows.begin(), tiledWindows.end());
    windows.insert(windows.end(), floatingWindows.begin(), floatingWindows.end());

    for (const auto& w : windows) {
        auto img     = image->windowImages.emplace_back(makeShared<SWindowImage>());
        img->pWindow = w;
        img->fb      = g_pHyprRenderer->createFB("hyprview-window");
        img->fb->alloc(pMonitor->m_pixelSize.x, pMonitor->m_pixelSize.y, OVERVIEW_FB_FORMAT);
        const auto REDRAW_ON_COMMIT = [wk = WP<SWindowImage>{img}, self = WP<IOverview>{g_pOverview}] {
            if (!self || !(self == g_pOverview))
                return;

            const auto SCROLL = dynamicPointerCast<CScrollOverview>(self).lock();
            const auto IMG    = wk.lock();
            if (!SCROLL || SCROLL->closing || !IMG)
                return;

            IMG->dirty = true;
            SCROLL->damage();
            if (SCROLL->pMonitor)
                g_pCompositor->scheduleFrameForMonitor(SCROLL->pMonitor.lock());
        };

        if (w->m_isX11) {
            if (const auto XWAYLAND_SURFACE = w->m_xwaylandSurface.lock()) {
                img->windowCommit = makeUnique<CHyprSignalListener>(XWAYLAND_SURFACE->m_events.commit.listen(REDRAW_ON_COMMIT));
            }
        } else if (const auto XDG_SURFACE = w->m_xdgSurface.lock()) {
            img->windowCommit = makeUnique<CHyprSignalListener>(XDG_SURFACE->m_events.commit.listen(REDRAW_ON_COMMIT));
        }

        redrawWindowImage(img);
    }

    blockOverviewRendering = false;
}

void CScrollOverview::redrawWindowImage(SP<SWindowImage> img) {
    if (!img)
        return;

    const auto WINDOW = img->pWindow.lock();
    if (!windowImageRenderable(WINDOW))
        return;

    CRegion fakeDamage{0, 0, sc<int>(pMonitor->m_transformedSize.x), sc<int>(pMonitor->m_transformedSize.y)};
    g_pHyprRenderer->beginFullFakeRender(pMonitor.lock(), fakeDamage, img->fb);

    CScrollOverview::clearCurrentRenderTarget(CHyprColor{0, 0, 0, 0});

    g_pHyprRenderer->renderWindow(WINDOW, pMonitor.lock(), Time::steadyNow(), true, RENDER_PASS_ALL, true, true);

    g_pHyprRenderer->m_renderData.blockScreenShader = true;
    g_pHyprRenderer->endRender();

    img->lastWindowPosition = WINDOW->m_realPosition->value();
    img->lastWindowSize     = WINDOW->m_realSize->value();
    img->dirty              = false;
}

bool CScrollOverview::windowImageRenderable(PHLWINDOW window) const {
    if (closing || !pMonitor || !validMapped(window))
        return false;

    if (window->m_isX11)
        return true;

    const auto SURFACE = window->wlSurface();
    if (!SURFACE || !SURFACE->resource() || !SURFACE->resource()->m_current.texture)
        return false;

    return true;
}

bool CScrollOverview::redrawDirtyWindowImages() {
    bool redrawn = false;

    for (const auto& wimg : images) {
        if (!wimg)
            continue;

        for (const auto& img : wimg->windowImages) {
            if (!img)
                continue;

            const auto WINDOW = img->pWindow.lock();
            if (!windowImageRenderable(WINDOW))
                continue;

            if (!img->dirty && img->lastWindowSize == WINDOW->m_realSize->value() && img->lastWindowPosition == WINDOW->m_realPosition->value())
                continue;

            if (!redrawn)
                g_pHyprOpenGL->makeEGLCurrent();

            redrawWindowImage(img);
            redrawn = true;
        }
    }

    return redrawn;
}

bool CScrollOverview::markDirtyWindowImagesForDamage(const CRegion& damage) {
    if (!pMonitor || damage.empty())
        return false;

    auto logicalDamage = damage.copy();
    logicalDamage.scale(1.0F / std::max(1.0F, sc<float>(pMonitor->m_scale)));
    logicalDamage.translate(pMonitor->m_position);

    bool marked = false;
    for (const auto& wimg : images) {
        if (!wimg)
            continue;

        for (const auto& img : wimg->windowImages) {
            if (!img)
                continue;

            const auto WINDOW = img->pWindow.lock();
            if (!windowImageRenderable(WINDOW))
                continue;

            const CBox WINDOW_BOX = {WINDOW->m_realPosition->value(), WINDOW->m_realSize->value()};
            if (logicalDamage.copy().intersect(WINDOW_BOX).empty())
                continue;

            img->dirty = true;
            marked     = true;
        }
    }

    return marked;
}

void CScrollOverview::redrawAll(bool forcelowres) {

    for (const auto& img : images) {
        redrawWorkspace(img->pWorkspace);
    }

    // redraw bg
    if (!backgroundFb)
        backgroundFb = g_pHyprRenderer->createFB("hyprview-background");
    if (backgroundFb->m_size != pMonitor->m_pixelSize) {
        backgroundFb->release();
        backgroundFb->alloc(pMonitor->m_pixelSize.x, pMonitor->m_pixelSize.y, OVERVIEW_FB_FORMAT);
    }

    CRegion fakeDamage{0, 0, sc<int>(pMonitor->m_transformedSize.x), sc<int>(pMonitor->m_transformedSize.y)};
    g_pHyprRenderer->beginFullFakeRender(pMonitor.lock(), fakeDamage, backgroundFb);

    CScrollOverview::clearCurrentRenderTarget(CHyprColor{0, 0, 0, 1.0});

    if (g_hyprviewConfig.scrolling.backgroundBlur) {
        g_pHyprRenderer->renderAllClientsForWorkspace(pMonitor.lock(), nullptr, Time::steadyNow());
        flushCurrentRenderPass();

        CRegion    blurDamage{fakeDamage};
        const auto BLURRED_TEX = g_pHyprRenderer->blurMainFramebuffer(1.0F, &blurDamage);
        if (BLURRED_TEX) {
            CScrollOverview::clearCurrentRenderTarget(CHyprColor{0, 0, 0, 1.0});

            CBox texbox = {{}, pMonitor->m_size};
            texbox.scale(pMonitor->m_scale).round();
            CRegion renderDamage{0, 0, INT16_MAX, INT16_MAX};
            g_pHyprOpenGL->renderTextureInternal(BLURRED_TEX, texbox, {.damage = &renderDamage, .a = 1.0});
        }
    } else
        g_pHyprRenderer->renderAllClientsForWorkspace(pMonitor.lock(), nullptr, Time::steadyNow());

    g_pHyprRenderer->m_renderData.blockScreenShader = true;
    g_pHyprRenderer->endRender();
}
