#include "ScrollOverview.hpp"
#include <algorithm>
#include <any>
#include <cmath>
#include <limits>

#define private   public
#define protected public
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/shared/workspace/WorkspaceRuleManager.hpp>
#include <hyprland/src/config/shared/animation/AnimationTree.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/algorithm/TiledAlgorithm.hpp>
#include <hyprland/src/layout/supplementary/WorkspaceAlgoMatcher.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/managers/animation/AnimationManager.hpp>
#include <hyprland/src/managers/animation/DesktopAnimationManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/cursor/CursorShapeOverrideController.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/protocols/XDGShell.hpp>
#include <hyprland/src/xwayland/XSurface.hpp>
#undef protected
#undef private
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>

void CScrollOverview::rebuildGeometryCache() {
    if (!pMonitor)
        return;

    const auto VIEWPORT_CENTER = CBox{{}, pMonitor->m_size}.middle();
    const auto WINDOW_GAP      = (std::max(0.0, static_cast<double>(g_hyprviewConfig.scrolling.windowGap)) / 2.0) * overviewStyleProgress();
    const auto WORKSPACE_STEP  = workspaceOverviewStep() * scale->value();
    float      yoff            = -sc<float>(activeWorkspaceImageIndex()) * WORKSPACE_STEP;

    for (const auto& wimg : images) {
        if (!wimg)
            continue;

        const auto CONTENT_PAN_X = horizontalPanForWorkspace(wimg);

        wimg->overviewBox = CBox{{}, pMonitor->m_size};
        wimg->overviewBox.translate(-VIEWPORT_CENTER).scale(scale->value()).translate(VIEWPORT_CENTER).translate(-viewOffset->value() * scale->value());
        wimg->overviewBox.translate({0.F, yoff});
        wimg->hitBox = wimg->overviewBox;

        for (const auto& img : wimg->windowImages) {
            if (!img || !img->pWindow)
                continue;

            const auto WINDOW_PAN = img->pWindow->m_isFloating ? 0.0 : CONTENT_PAN_X;
            const CBox BASE_BOX   = {img->pWindow->m_realPosition->value() - pMonitor->m_position, img->pWindow->m_realSize->value()};

            CBox       rawOverviewBox = CBox{BASE_BOX.pos() - Vector2D{WINDOW_PAN, 0.0}, BASE_BOX.size()};
            rawOverviewBox.translate(-VIEWPORT_CENTER).scale(scale->value()).translate(VIEWPORT_CENTER).translate(-viewOffset->value() * scale->value());
            rawOverviewBox.translate({0.F, yoff});

            img->overviewBox = rawOverviewBox;
            img->overviewBox = shrinkBox(img->overviewBox, WINDOW_GAP);

            wimg->hitBox = boxUnion(wimg->hitBox, img->overviewBox);
        }

        yoff += WORKSPACE_STEP;
    }

    buildInsertionMarkers();
}

double CScrollOverview::overviewStyleProgress() const {
    if (!scale)
        return 1.0;

    const double DEFAULT_ZOOM = std::clamp(sc<double>(g_hyprviewConfig.scrolling.defaultZoom), 0.1, 0.9);
    const double RANGE        = 1.0 - DEFAULT_ZOOM;
    if (RANGE <= 0.0)
        return 1.0;

    return std::clamp((1.0 - sc<double>(scale->value())) / RANGE, 0.0, 1.0);
}

SP<CScrollOverview::SWorkspaceImage> CScrollOverview::workspaceAt(const Vector2D& local) {
    for (auto it = images.rbegin(); it != images.rend(); ++it) {
        const auto& wimg = *it;
        if (wimg && wimg->pWorkspace && wimg->hitBox.containsPoint(local))
            return wimg;
    }

    return nullptr;
}

SP<CScrollOverview::SWindowImage> CScrollOverview::windowAtExact(const Vector2D& local) {
    for (auto wit = images.rbegin(); wit != images.rend(); ++wit) {
        const auto& wimg = *wit;
        if (!wimg || !wimg->hitBox.containsPoint(local))
            continue;

        for (auto it = wimg->windowImages.rbegin(); it != wimg->windowImages.rend(); ++it) {
            const auto& img = *it;
            if (img && img->pWindow && img->overviewBox.containsPoint(local))
                return img;
        }
    }

    return nullptr;
}

double CScrollOverview::distanceToBox(const Vector2D& point, const CBox& box) {
    if (box.empty())
        return std::numeric_limits<double>::max();

    const double dx = point.x < box.x ? box.x - point.x : (point.x > box.x + box.w ? point.x - (box.x + box.w) : 0.0);
    const double dy = point.y < box.y ? box.y - point.y : (point.y > box.y + box.h ? point.y - (box.y + box.h) : 0.0);
    return dx * dx + dy * dy;
}

CBox CScrollOverview::expandedWindowHitBox(const SP<SWindowImage>& image) const {
    if (!image || image->overviewBox.empty())
        return {};

    const double EXPANSION = std::max(0.0, sc<double>(g_hyprviewConfig.mouse.hitboxExpansion));
    CBox         box       = image->overviewBox;
    box.x -= EXPANSION;
    box.y -= EXPANSION;
    box.w += EXPANSION * 2.0;
    box.h += EXPANSION * 2.0;
    return box;
}

SP<CScrollOverview::SWindowImage> CScrollOverview::windowNear(const Vector2D& local) {
    if (g_hyprviewConfig.mouse.hitboxExpansion <= 0)
        return nullptr;

    SP<SWindowImage> best;
    double           bestDistance = std::numeric_limits<double>::max();

    for (auto wit = images.rbegin(); wit != images.rend(); ++wit) {
        const auto& wimg = *wit;
        if (!wimg)
            continue;

        for (auto it = wimg->windowImages.rbegin(); it != wimg->windowImages.rend(); ++it) {
            const auto& img = *it;
            if (!img || !img->pWindow)
                continue;

            const auto HITBOX = expandedWindowHitBox(img);
            if (HITBOX.empty() || !HITBOX.containsPoint(local))
                continue;

            if (!g_hyprviewConfig.mouse.nearestHitbox)
                return img;

            const auto DISTANCE = distanceToBox(local, img->overviewBox);
            if (DISTANCE < bestDistance) {
                best         = img;
                bestDistance = DISTANCE;
            }
        }
    }

    return best;
}

SP<CScrollOverview::SWindowImage> CScrollOverview::windowAt(const Vector2D& local) {
    if (auto exact = windowAtExact(local))
        return exact;

    return windowNear(local);
}

SP<CScrollOverview::SWorkspaceImage> CScrollOverview::imageForWorkspace(PHLWORKSPACE w) {
    for (const auto& i : images) {
        if (i->pWorkspace == w)
            return i;
    }
    return nullptr;
}

SP<CScrollOverview::SWindowImage> CScrollOverview::imageForWindow(PHLWINDOW w) const {
    if (!w)
        return nullptr;

    for (const auto& wimg : images) {
        if (!wimg)
            continue;

        for (const auto& img : wimg->windowImages) {
            if (img && img->pWindow == w)
                return img;
        }
    }

    return nullptr;
}
