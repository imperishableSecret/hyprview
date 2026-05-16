#include "ScrollOverview.hpp"
#include "../../plugin/HyprviewConfig.hpp"
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
    if (workspaceEntries.empty())
        return false;

    if (viewportCurrentWorkspace == 0 && !up)
        return false;
    if (viewportCurrentWorkspace == workspaceEntries.size() - 1 && up)
        return false;

    if (up)
        return setViewportWorkspace(viewportCurrentWorkspace + 1, false, true);

    return setViewportWorkspace(viewportCurrentWorkspace - 1, false, true);
}

bool CScrollOverview::setViewportWorkspace(size_t index, bool warp, bool activate) {
    if (workspaceEntries.empty())
        return false;

    index = std::clamp(index, static_cast<size_t>(0), workspaceEntries.size() - 1);
    return setViewportOffset({0.0, viewOffsetForWorkspaceIndex(index)}, warp, activate);
}

bool CScrollOverview::focusWorkspaceInViewport(SP<SWorkspaceEntry> workspace, bool warp, bool activate) {
    if (!workspace)
        return false;

    for (size_t i = 0; i < workspaceEntries.size(); ++i) {
        if (workspaceEntries[i] == workspace)
            return setViewportWorkspace(i, warp, activate);
    }

    return false;
}

bool CScrollOverview::reanchorViewportToWorkspace(PHLWORKSPACE workspace, bool preserveVisualOffset) {
    if (!workspace || workspaceEntries.empty() || !viewOffset)
        return false;

    const auto INDEX = workspaceEntryIndex(workspace);
    if (!INDEX)
        return false;

    Vector2D offset = {};
    if (preserveVisualOffset) {
        const auto OLD_ANCHOR_INDEX = activeWorkspaceEntryIndex();
        offset                      = viewOffset->value();
        offset.y += (sc<double>(OLD_ANCHOR_INDEX) - sc<double>(*INDEX)) * workspaceOverviewStep();
    }

    startedOn = workspace;

    if (preserveVisualOffset) {
        viewOffset->setValueAndWarp(clampedViewOffset(offset));
        syncViewportWorkspaceFromOffset(viewOffset->value());
    } else {
        viewportCurrentWorkspace = *INDEX;
        viewOffset->setValueAndWarp({});
    }

    markGeometryCacheDirty(GEOMETRY_DIRTY_VIEWPORT | GEOMETRY_DIRTY_INSERTION_MARKERS);
    return true;
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

    markGeometryCacheDirty(GEOMETRY_DIRTY_VIEWPORT | GEOMETRY_DIRTY_INSERTION_MARKERS);
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
    if (!pMonitor || workspaceEntries.empty())
        return;

    const auto ACTIVE_INDEX = activeWorkspaceEntryIndex();
    const auto REL_INDEX    = offset.y / workspaceOverviewStep() + sc<double>(ACTIVE_INDEX);
    const auto INDEX        = sc<int64_t>(std::llround(REL_INDEX));

    viewportCurrentWorkspace = sc<size_t>(std::clamp<int64_t>(INDEX, 0, sc<int64_t>(workspaceEntries.size() - 1)));
}

void CScrollOverview::activateViewportWorkspace() {
    if (!pMonitor || workspaceEntries.empty() || viewportCurrentWorkspace >= workspaceEntries.size())
        return;

    const auto WORKSPACE = workspaceEntries[viewportCurrentWorkspace] ? workspaceEntries[viewportCurrentWorkspace]->pWorkspace : PHLWORKSPACE{};
    activateWorkspace(WORKSPACE);
}

void CScrollOverview::activateWorkspace(PHLWORKSPACE workspace, bool focus) {
    if (!pMonitor || !workspace || workspace->m_isSpecialWorkspace || workspace->m_monitor != pMonitor || workspace == pMonitor->m_activeWorkspace)
        return;

    pMonitor->changeWorkspace(workspace, false, true, !focus);
}

double CScrollOverview::workspaceOverviewStep() const {
    if (!pMonitor)
        return 0.0;

    const double SCALE         = std::max(sc<double>(scale->value()), 0.001);
    const double WORKSPACE_GAP = std::max(0.0, sc<double>(g_hyprviewConfig.scrolling.workspaceGap)) * overviewStyleProgress();
    return pMonitor->m_size.y + WORKSPACE_GAP / SCALE;
}

double CScrollOverview::viewOffsetForWorkspaceIndex(size_t index) const {
    if (!pMonitor || workspaceEntries.empty())
        return 0.0;

    const auto ACTIVE_INDEX = activeWorkspaceEntryIndex();
    return (sc<double>(index) - sc<double>(ACTIVE_INDEX)) * workspaceOverviewStep();
}

Vector2D CScrollOverview::clampedViewOffset(Vector2D offset) const {
    if (!pMonitor || workspaceEntries.empty())
        return offset;

    const auto ACTIVE_INDEX = activeWorkspaceEntryIndex();
    const auto STEP         = workspaceOverviewStep();
    const auto MIN_Y        = -sc<double>(ACTIVE_INDEX) * STEP;
    const auto MAX_Y        = sc<double>(workspaceEntries.size() - 1 - ACTIVE_INDEX) * STEP;

    offset.x = 0.0;
    offset.y = std::clamp(offset.y, MIN_Y, MAX_Y);
    return offset;
}
