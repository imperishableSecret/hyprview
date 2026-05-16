#include "ScrollOverview.hpp"
#include "../../plugin/Telemetry.hpp"
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

static constexpr double INSERTION_MARKER_HEIGHT = 28.0;
static constexpr double INSERTION_MARKER_GAP    = 8.0;
static constexpr double INSERTION_MARKER_WIDTH  = 64.0;

static void             damageOverviewForPan(WP<Hyprutils::Animation::CBaseAnimatedVariable> thisptr) {
    if (g_pOverview)
        g_pOverview->damage();
}

namespace {
    std::string workspaceID(PHLWORKSPACE workspace) {
        return workspace ? std::to_string(workspace->m_id) : "<none>";
    }
}

CBox CScrollOverview::boxUnion(const CBox& a, const CBox& b) {
    if (a.empty())
        return b;
    if (b.empty())
        return a;

    const double x1 = std::min(a.x, b.x);
    const double y1 = std::min(a.y, b.y);
    const double x2 = std::max(a.x + a.w, b.x + b.w);
    const double y2 = std::max(a.y + a.h, b.y + b.h);

    return {x1, y1, x2 - x1, y2 - y1};
}

bool CScrollOverview::workspaceUsesScrollingLayout(PHLWORKSPACE workspace) const {
    if (!workspace || !workspace->m_space || !workspace->m_space->algorithm())
        return false;

    const auto& TILED_ALGO = workspace->m_space->algorithm()->tiledAlgo();
    if (!TILED_ALGO)
        return false;

    return Layout::Supplementary::algoMatcher()->getNameForTiledAlgo(&typeid(*TILED_ALGO.get())) == "scrolling";
}

CScrollOverview::SWorkspacePanRange CScrollOverview::horizontalPanRangeForWorkspace(const SP<SWorkspaceEntry>& workspace) const {
    if (!pMonitor || !workspace || !workspaceUsesScrollingLayout(workspace->pWorkspace))
        return {};

    bool   foundTiled   = false;
    double minX         = 0.0;
    double maxX         = 0.0;
    double minCenterPan = 0.0;
    double maxCenterPan = 0.0;

    for (const auto& entry : workspace->windowEntries) {
        if (!entry || !entry->pWindow || entry->pWindow->m_isFloating)
            continue;

        const auto   POS        = entry->pWindow->m_realPosition->goal() - pMonitor->m_position;
        const auto   SIZE       = entry->pWindow->m_realSize->goal();
        const double CENTER_PAN = POS.x + SIZE.x / 2.0 - pMonitor->m_size.x / 2.0;

        if (!foundTiled) {
            minX         = POS.x;
            maxX         = POS.x + SIZE.x;
            minCenterPan = CENTER_PAN;
            maxCenterPan = CENTER_PAN;
            foundTiled   = true;
        } else {
            minX         = std::min(minX, POS.x);
            maxX         = std::max(maxX, POS.x + SIZE.x);
            minCenterPan = std::min(minCenterPan, CENTER_PAN);
            maxCenterPan = std::max(maxCenterPan, CENTER_PAN);
        }
    }

    if (!foundTiled)
        return {};

    double minPan = std::min(0.0, minX);
    double maxPan = std::max(0.0, maxX - pMonitor->m_size.x);

    if (maxPan - minPan > 0.5) {
        minPan = std::min(minPan, minCenterPan);
        maxPan = std::max(maxPan, maxCenterPan);
    }

    return {
        .min = minPan,
        .max = maxPan,
    };
}

double CScrollOverview::horizontalPanForWorkspace(const SP<SWorkspaceEntry>& workspace) const {
    if (!workspace || !workspace->pWorkspace)
        return 0.0;

    const auto IT = workspaceContentPan.find(workspace->pWorkspace->m_id);
    if (IT == workspaceContentPan.end() || !IT->second)
        return 0.0;

    const auto RANGE = horizontalPanRangeForWorkspace(workspace);
    return std::clamp(sc<double>(IT->second->value()), RANGE.min, RANGE.max);
}

bool CScrollOverview::setHorizontalPanForWorkspace(const SP<SWorkspaceEntry>& workspace, double pan, bool animate) {
    if (!workspace || !workspace->pWorkspace || !workspaceUsesScrollingLayout(workspace->pWorkspace)) {
        Hyprview::telemetryLogLazy([&] {
            return std::format("event=pan-set-skip reason=invalid-or-not-scrolling workspace={} requestedPan={:.2f} animate={}",
                               workspaceID(workspace ? workspace->pWorkspace : PHLWORKSPACE{}), pan, Hyprview::boolToken(animate));
        });
        return false;
    }

    const auto RANGE   = horizontalPanRangeForWorkspace(workspace);
    const auto CLAMPED = std::clamp(pan, RANGE.min, RANGE.max);
    const auto KEY     = workspace->pWorkspace->m_id;
    auto&      ANIM    = workspaceContentPan[KEY];

    if (!ANIM) {
        g_pAnimationManager->createAnimation(0.F, ANIM, Config::animationTree()->getAnimationPropertyConfig("windowsMove"), AVARDAMAGE_NONE);
        ANIM->setUpdateCallback(damageOverviewForPan);
    }

    const auto CURRENT = sc<double>(ANIM->value());

    Hyprview::telemetryLogLazy([&] {
        return std::format("event=pan-set workspace={} requestedPan={:.2f} clampedPan={:.2f} currentPan={:.2f} range={:.2f},{:.2f} animate={}", workspaceID(workspace->pWorkspace),
                           pan, CLAMPED, CURRENT, RANGE.min, RANGE.max, Hyprview::boolToken(animate));
    });

    if (std::abs(CURRENT - CLAMPED) < 0.5) {
        Hyprview::telemetryLogLazy(
            [&] { return std::format("event=pan-set-skip reason=noop workspace={} currentPan={:.2f} clampedPan={:.2f}", workspaceID(workspace->pWorkspace), CURRENT, CLAMPED); });
        return false;
    }

    if (animate)
        *ANIM = sc<float>(CLAMPED);
    else
        ANIM->setValueAndWarp(sc<float>(CLAMPED));

    markGeometryCacheDirty(GEOMETRY_DIRTY_WORKSPACE_PAN | GEOMETRY_DIRTY_WINDOW_GEOMETRY);
    damage();
    if (pMonitor)
        g_pCompositor->scheduleFrameForMonitor(pMonitor.lock());

    return true;
}

void CScrollOverview::pruneWorkspaceContentPans() {
    std::erase_if(workspaceContentPan, [this](const auto& entry) {
        return !std::ranges::any_of(workspaceEntries, [&entry](const auto& image) { return image && image->pWorkspace && image->pWorkspace->m_id == entry.first; });
    });
}

bool CScrollOverview::workspaceHasDisplayableWindows(PHLWORKSPACE workspace) const {
    if (!workspace)
        return false;

    return !overviewWindowsForWorkspace(workspace, false).empty() || !overviewWindowsForWorkspace(workspace, true).empty();
}

bool CScrollOverview::workspaceVisibleInOverview(PHLWORKSPACE workspace) const {
    if (!workspace || workspace->m_isSpecialWorkspace)
        return false;

    if (workspaceHasDisplayableWindows(workspace))
        return true;

    return pMonitor && workspace == pMonitor->m_activeWorkspace;
}

bool CScrollOverview::windowBelongsToWorkspaceInOverview(PHLWINDOW window, PHLWORKSPACE workspace) const {
    if (!window || !workspace || workspace->m_isSpecialWorkspace)
        return false;

    const auto WORKSPACE_ID = overviewWorkspaceIDForWindow(window);
    return WORKSPACE_ID && *WORKSPACE_ID == workspace->m_id;
}

bool CScrollOverview::windowCanDragAcrossWorkspaces(PHLWINDOW window) const {
    return validMapped(window) && !window->m_pinned;
}

bool CScrollOverview::workspaceIDAllowedOnMonitor(WORKSPACEID id, bool allowOutsideCurrentRange) const {
    if (!pMonitor || id <= 0)
        return false;

    if (!allowOutsideCurrentRange && g_pCompositor->workspaceIDOutOfBounds(id))
        return false;

    if (const auto EXISTING = g_pCompositor->getWorkspaceByID(id); EXISTING)
        return EXISTING->m_monitor == pMonitor && !workspaceHasDisplayableWindows(EXISTING);

    const auto BOUND = Config::workspaceRuleMgr()->getBoundMonitorForWS(std::to_string(id));
    if (BOUND && BOUND->m_id != pMonitor->m_id)
        return false;

    return true;
}

std::vector<WORKSPACEID> CScrollOverview::validWorkspaceInsertionIDs(PHLWORKSPACE before, PHLWORKSPACE after) const {
    std::vector<WORKSPACEID> ids;
    if (!before || !after || before->m_id <= 0 || after->m_id <= 0 || after->m_id <= before->m_id + 1)
        return ids;

    const size_t MAX_MARKERS = sc<size_t>(std::max(0, g_hyprviewConfig.scrolling.insertionMaxMarkers));
    if (MAX_MARKERS == 0)
        return ids;

    for (WORKSPACEID id = before->m_id + 1; id < after->m_id && ids.size() < MAX_MARKERS; ++id) {
        if (workspaceIDAllowedOnMonitor(id))
            ids.emplace_back(id);
    }

    return ids;
}

std::vector<WORKSPACEID> CScrollOverview::appendWorkspaceInsertionIDs() const {
    std::vector<WORKSPACEID> ids;
    const auto               MARKER_COUNT = std::clamp(g_hyprviewConfig.scrolling.appendMarkerCount, 0, 5);
    if (MARKER_COUNT == 0)
        return ids;

    WORKSPACEID lastNumericWorkspace = WORKSPACE_INVALID;

    for (const auto& workspaceEntry : workspaceEntries) {
        if (!workspaceEntry || !workspaceEntry->pWorkspace || workspaceEntry->pWorkspace->m_id <= 0)
            continue;

        lastNumericWorkspace = std::max(lastNumericWorkspace, workspaceEntry->pWorkspace->m_id);
    }

    if (lastNumericWorkspace == WORKSPACE_INVALID)
        return ids;

    for (WORKSPACEID id = lastNumericWorkspace + 1; id < 100000 && ids.size() < sc<size_t>(MARKER_COUNT); ++id) {
        if (workspaceIDAllowedOnMonitor(id, true))
            ids.emplace_back(id);
    }

    return ids;
}

void CScrollOverview::buildInsertionMarkers() {
    insertionMarkers.clear();

    if (!pMonitor || workspaceEntries.empty())
        return;

    auto addMarkerGroup = [this](const CBox& referenceBox, double centerY, const std::vector<WORKSPACEID>& ids) {
        if (ids.empty() || referenceBox.empty())
            return;

        const double INSET        = std::min(64.0, std::max(12.0, referenceBox.w / 12.0));
        const double AVAILABLE    = std::max(1.0, referenceBox.w - INSET * 2.0);
        const double GAP          = std::min(INSERTION_MARKER_GAP, std::max(0.0, AVAILABLE / sc<double>(ids.size() + 1)));
        const double RAW_MARKER_W = (AVAILABLE - GAP * sc<double>(ids.size() - 1)) / sc<double>(ids.size());
        const double MARKER_WIDTH = std::clamp(RAW_MARKER_W, 8.0, INSERTION_MARKER_WIDTH);
        const double TOTAL_WIDTH  = MARKER_WIDTH * sc<double>(ids.size()) + GAP * sc<double>(ids.size() - 1);
        const double START_X      = referenceBox.x + (referenceBox.w - TOTAL_WIDTH) / 2.0;
        const double MARKER_Y     = centerY - INSERTION_MARKER_HEIGHT / 2.0;

        for (size_t i = 0; i < ids.size(); ++i) {
            const auto ID = ids[i];
            insertionMarkers.push_back({
                .workspaceID = ID,
                .box         = {START_X + sc<double>(i) * (MARKER_WIDTH + GAP), MARKER_Y, MARKER_WIDTH, INSERTION_MARKER_HEIGHT},
                .label       = std::to_string(ID),
            });
        }
    };

    std::vector<SP<SWorkspaceEntry>> numericEntries;
    for (const auto& workspaceEntry : workspaceEntries) {
        if (workspaceEntry && workspaceEntry->pWorkspace && workspaceEntry->pWorkspace->m_id > 0)
            numericEntries.emplace_back(workspaceEntry);
    }

    if (numericEntries.empty())
        return;

    for (size_t i = 0; i + 1 < numericEntries.size(); ++i) {
        const auto& BEFORE = numericEntries[i];
        const auto& AFTER  = numericEntries[i + 1];
        const auto  IDS    = validWorkspaceInsertionIDs(BEFORE->pWorkspace, AFTER->pWorkspace);
        const auto  MID_Y  = (BEFORE->overviewBox.y + BEFORE->overviewBox.h + AFTER->overviewBox.y) / 2.0;

        addMarkerGroup(BEFORE->overviewBox, MID_Y, IDS);
    }

    const auto APPEND_IDS = appendWorkspaceInsertionIDs();
    if (!APPEND_IDS.empty()) {
        const auto& LAST = numericEntries.back();
        addMarkerGroup(LAST->overviewBox, LAST->overviewBox.y + LAST->overviewBox.h - INSERTION_MARKER_HEIGHT / 2.0 - 24.0, APPEND_IDS);
    }
}

PHLWORKSPACE CScrollOverview::workspaceForDropTarget(const SDropTarget& target) {
    if (target.type == eDropTargetType::WORKSPACE_BODY)
        return target.workspace ? target.workspace->pWorkspace : nullptr;

    if (target.type != eDropTargetType::WORKSPACE_INSERTION || target.targetWorkspaceID == WORKSPACE_INVALID)
        return nullptr;

    if (const auto EXISTING = g_pCompositor->getWorkspaceByID(target.targetWorkspaceID); EXISTING)
        return EXISTING->m_monitor == pMonitor ? EXISTING : nullptr;

    if (!workspaceIDAllowedOnMonitor(target.targetWorkspaceID, true))
        return nullptr;

    return g_pCompositor->createNewWorkspace(target.targetWorkspaceID, pMonitor->m_id, "", false);
}

std::optional<size_t> CScrollOverview::workspaceEntryIndex(PHLWORKSPACE workspace) const {
    if (!workspace)
        return std::nullopt;

    for (size_t i = 0; i < workspaceEntries.size(); ++i) {
        if (workspaceEntries[i] && workspaceEntries[i]->pWorkspace == workspace)
            return i;
    }

    return std::nullopt;
}

size_t CScrollOverview::activeWorkspaceEntryIndex() const {
    for (size_t i = 0; i < workspaceEntries.size(); ++i) {
        if (workspaceEntries[i]->pWorkspace && workspaceEntries[i]->pWorkspace == startedOn)
            return i;
    }

    return 0;
}
