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

bool CScrollOverview::moveViewportWorkspace(bool up) {
    if (images.empty())
        return false;

    if (viewportCurrentWorkspace == 0 && !up)
        return false;
    if (viewportCurrentWorkspace == images.size() - 1 && up)
        return false;

    if (up)
        return setViewportWorkspace(viewportCurrentWorkspace + 1, false, true);

    return setViewportWorkspace(viewportCurrentWorkspace - 1, false, true);
}

bool CScrollOverview::setViewportWorkspace(size_t index, bool warp, bool activate) {
    if (images.empty())
        return false;

    index = std::clamp(index, static_cast<size_t>(0), images.size() - 1);
    return setViewportOffset({0.0, viewOffsetForWorkspaceIndex(index)}, warp, activate);
}

bool CScrollOverview::focusWorkspaceInViewport(SP<SWorkspaceImage> workspace, bool warp, bool activate) {
    if (!workspace)
        return false;

    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i] == workspace)
            return setViewportWorkspace(i, warp, activate);
    }

    return false;
}

bool CScrollOverview::moveViewportBy(double deltaY, bool warp, bool activate) {
    return setViewportOffset(viewOffset->value() + Vector2D{0.0, deltaY}, warp, activate);
}

bool CScrollOverview::setViewportOffset(Vector2D offset, bool warp, bool activate) {
    const auto CLAMPED             = clampedViewOffset(offset);
    const auto OLD_WORKSPACE_INDEX = viewportCurrentWorkspace;
    if (viewOffset->value().distance(CLAMPED) < 0.5) {
        syncViewportWorkspaceFromOffset(CLAMPED);
        if (OLD_WORKSPACE_INDEX != viewportCurrentWorkspace)
            syncSelectionToViewport(false);
        if (activate)
            activateViewportWorkspace();
        return false;
    }

    if (warp)
        viewOffset->setValueAndWarp(CLAMPED);
    else
        *viewOffset = CLAMPED;

    syncViewportWorkspaceFromOffset(CLAMPED);
    if (OLD_WORKSPACE_INDEX != viewportCurrentWorkspace)
        syncSelectionToViewport(false);
    if (activate)
        activateViewportWorkspace();
    damage();
    if (pMonitor)
        g_pCompositor->scheduleFrameForMonitor(pMonitor.lock());

    return true;
}

void CScrollOverview::syncViewportWorkspaceFromOffset(const Vector2D& offset) {
    if (!pMonitor || images.empty())
        return;

    const auto ACTIVE_INDEX = activeWorkspaceImageIndex();
    const auto REL_INDEX    = offset.y / pMonitor->m_size.y + sc<double>(ACTIVE_INDEX);
    const auto INDEX        = sc<int64_t>(std::llround(REL_INDEX));

    viewportCurrentWorkspace = sc<size_t>(std::clamp<int64_t>(INDEX, 0, sc<int64_t>(images.size() - 1)));
}

void CScrollOverview::activateViewportWorkspace() {
    if (!pMonitor || images.empty() || viewportCurrentWorkspace >= images.size())
        return;

    const auto WORKSPACE = images[viewportCurrentWorkspace] ? images[viewportCurrentWorkspace]->pWorkspace : PHLWORKSPACE{};
    activateWorkspace(WORKSPACE);
}

void CScrollOverview::activateWorkspace(PHLWORKSPACE workspace, bool focus) {
    if (!pMonitor || !workspace || workspace->m_isSpecialWorkspace || workspace->m_monitor != pMonitor || workspace == pMonitor->m_activeWorkspace)
        return;

    pMonitor->changeWorkspace(workspace, false, true, !focus);
}

double CScrollOverview::viewOffsetForWorkspaceIndex(size_t index) const {
    if (!pMonitor || images.empty())
        return 0.0;

    const auto ACTIVE_INDEX = activeWorkspaceImageIndex();
    return (sc<double>(index) - sc<double>(ACTIVE_INDEX)) * pMonitor->m_size.y;
}

Vector2D CScrollOverview::clampedViewOffset(Vector2D offset) const {
    if (!pMonitor || images.empty())
        return offset;

    const auto ACTIVE_INDEX = activeWorkspaceImageIndex();
    const auto MIN_Y        = -sc<double>(ACTIVE_INDEX) * pMonitor->m_size.y;
    const auto MAX_Y        = sc<double>(images.size() - 1 - ACTIVE_INDEX) * pMonitor->m_size.y;

    offset.x = 0.0;
    offset.y = std::clamp(offset.y, MIN_Y, MAX_Y);
    return offset;
}
