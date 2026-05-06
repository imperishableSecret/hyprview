#include "ScrollOverview.hpp"
#include "../../plugin/Telemetry.hpp"
#include <algorithm>
#include <any>
#include <cmath>
#include <limits>
#include <linux/input-event-codes.h>
#include <string>
#include <wlr-layer-shell-unstable-v1.hpp>
#define private   public
#define protected public
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/shared/workspace/WorkspaceRuleManager.hpp>
#include <hyprland/src/config/shared/animation/AnimationTree.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/algorithm/TiledAlgorithm.hpp>
#include <hyprland/src/layout/algorithm/tiled/scrolling/ScrollingAlgorithm.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
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

static constexpr double DRAG_THRESHOLD                    = 8.0;
static constexpr double SMOOTH_SCROLL_WORKSPACE_THRESHOLD = 120.0;

namespace {
    std::string workspaceID(PHLWORKSPACE workspace) {
        return workspace ? std::to_string(workspace->m_id) : "<none>";
    }

    std::string windowID(PHLWINDOW window) {
        return window ? Hyprview::formatRawPtr(window.get()) : "0";
    }

    CBox workspaceGlobalBox(PHLWORKSPACE workspace, PHLMONITOR fallbackMonitor) {
        const auto MONITOR = workspace && workspace->m_monitor ? workspace->m_monitor.lock() : fallbackMonitor;
        if (!MONITOR)
            return {};

        return {MONITOR->m_position, MONITOR->m_size};
    }

    CBox centerBoxInWorkspace(CBox box, PHLWORKSPACE workspace, PHLMONITOR fallbackMonitor) {
        const auto WORKSPACE_BOX = workspaceGlobalBox(workspace, fallbackMonitor);
        if (WORKSPACE_BOX.w <= 0.0 || WORKSPACE_BOX.h <= 0.0)
            return box;

        box.x = WORKSPACE_BOX.x + std::max(0.0, WORKSPACE_BOX.w - box.w) / 2.0;
        box.y = WORKSPACE_BOX.y + std::max(0.0, WORKSPACE_BOX.h - box.h) / 2.0;

        return box;
    }

    CBox clampBoxToWorkspace(CBox box, PHLWORKSPACE workspace, PHLMONITOR fallbackMonitor, double margin = 0.0) {
        const auto WORKSPACE_BOX = workspaceGlobalBox(workspace, fallbackMonitor);
        if (WORKSPACE_BOX.w <= 0.0 || WORKSPACE_BOX.h <= 0.0)
            return box;

        const double CLAMP_MARGIN = std::max(0.0, margin);
        const double MIN_X        = WORKSPACE_BOX.x + CLAMP_MARGIN;
        const double MIN_Y        = WORKSPACE_BOX.y + CLAMP_MARGIN;
        const double MAX_X        = WORKSPACE_BOX.x + std::max(0.0, WORKSPACE_BOX.w - box.w - 2.0 * CLAMP_MARGIN) + CLAMP_MARGIN;
        const double MAX_Y        = WORKSPACE_BOX.y + std::max(0.0, WORKSPACE_BOX.h - box.h - 2.0 * CLAMP_MARGIN) + CLAMP_MARGIN;

        box.x = std::clamp(box.x, MIN_X, std::max(MIN_X, MAX_X));
        box.y = std::clamp(box.y, MIN_Y, std::max(MIN_Y, MAX_Y));

        return box;
    }

    Layout::Tiled::CScrollingAlgorithm* scrollingAlgorithmForTarget(const SP<Layout::ITarget>& target) {
        if (!target || !target->space() || !target->space()->algorithm())
            return nullptr;

        return dynamic_cast<Layout::Tiled::CScrollingAlgorithm*>(target->space()->algorithm()->m_tiled.get());
    }

    bool moveScrollingTargetToHorizontalEdge(const SP<Layout::ITarget>& target, int side) {
        if (!target || side == 0)
            return false;

        const auto ALGO = scrollingAlgorithmForTarget(target);
        if (!ALGO || !ALGO->m_scrollingData)
            return false;

        const auto DATA = ALGO->dataFor(target);
        if (!DATA)
            return false;

        const auto SOURCE_COLUMN = DATA->column.lock();
        if (!SOURCE_COLUMN)
            return false;

        SOURCE_COLUMN->remove(target);

        const int64_t INSERT_AFTER = side < 0 ? -1 : sc<int64_t>(ALGO->m_scrollingData->columns.size()) - 1;
        const auto    NEW_COLUMN   = ALGO->m_scrollingData->add(INSERT_AFTER);
        NEW_COLUMN->add(DATA);
        ALGO->m_scrollingData->centerOrFitCol(NEW_COLUMN);
        ALGO->m_scrollingData->recalculate();
        ALGO->focusTargetUpdate(target);

        return true;
    }

    bool moveScrollingTargetNextToWindow(const SP<Layout::ITarget>& target, const PHLWINDOW& anchor, const std::string& direction) {
        if (!target || !anchor || !anchor->layoutTarget() || direction.empty())
            return false;

        const auto ALGO = scrollingAlgorithmForTarget(target);
        if (!ALGO || !ALGO->m_scrollingData)
            return false;

        const auto DATA        = ALGO->dataFor(target);
        const auto ANCHOR_DATA = ALGO->dataFor(anchor->layoutTarget());
        if (!DATA || !ANCHOR_DATA)
            return false;

        const auto SOURCE_COLUMN = DATA->column.lock();
        const auto ANCHOR_COLUMN = ANCHOR_DATA->column.lock();
        if (!SOURCE_COLUMN || !ANCHOR_COLUMN)
            return false;

        if (direction == "l" || direction == "r") {
            SOURCE_COLUMN->remove(target);

            const auto ANCHOR_COLUMN_INDEX = ALGO->m_scrollingData->idx(ANCHOR_COLUMN);
            if (ANCHOR_COLUMN_INDEX < 0)
                return false;

            const int64_t INSERT_AFTER = direction == "l" ? ANCHOR_COLUMN_INDEX - 1 : ANCHOR_COLUMN_INDEX;
            const auto    NEW_COLUMN   = ALGO->m_scrollingData->add(INSERT_AFTER);
            NEW_COLUMN->add(DATA);
            ALGO->m_scrollingData->centerOrFitCol(NEW_COLUMN);
            ALGO->m_scrollingData->recalculate();
            ALGO->focusTargetUpdate(target);

            return true;
        }

        if (direction != "u" && direction != "d")
            return false;

        SOURCE_COLUMN->remove(target);

        const auto ANCHOR_INDEX = ANCHOR_COLUMN->idx(anchor->layoutTarget());
        const int  INSERT_AFTER = direction == "u" ? sc<int>(ANCHOR_INDEX) - 1 : sc<int>(ANCHOR_INDEX);
        ANCHOR_COLUMN->add(DATA, INSERT_AFTER);
        ALGO->m_scrollingData->centerOrFitCol(ANCHOR_COLUMN);
        ALGO->m_scrollingData->recalculate();
        ALGO->focusTargetUpdate(target);

        return true;
    }
}

bool CScrollOverview::pointerOverBlockingLayerSurface(const Vector2D& local) const {
    if (!pMonitor || inputState.mode != ePointerMode::IDLE)
        return false;

    const auto MONITOR = pMonitor.lock();
    if (!MONITOR)
        return false;

    const auto GLOBAL = MONITOR->m_position + local;

    Vector2D   surfaceCoords;
    PHLLS      foundLayer;

    if (const auto SURFACE = g_pCompositor->vectorToLayerPopupSurface(GLOBAL, MONITOR, &surfaceCoords, &foundLayer); SURFACE) {
        const auto OWNER = overviewSurfaceOwner(SURFACE);
        if (surfaceOwnerBelongsToOverviewMonitor(OWNER, MONITOR) && (OWNER.type == eOverviewSurfaceOwner::LAYER || OWNER.type == eOverviewSurfaceOwner::LAYER_POPUP) &&
            OWNER.layer && OWNER.layer->m_layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP)
            return true;
    }

    if (g_pCompositor->vectorToLayerSurface(GLOBAL, &MONITOR->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY], &surfaceCoords, &foundLayer))
        return true;

    if (g_pCompositor->vectorToLayerSurface(GLOBAL, &MONITOR->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_TOP], &surfaceCoords, &foundLayer))
        return true;

    return false;
}

void CScrollOverview::selectHoveredWorkspace() {
    rebuildGeometryCache();

    closeOnWindow.reset();
    closeOnWorkspace.reset();

    if (const auto WINDOW = windowAt(lastMousePosLocal); WINDOW && WINDOW->pWindow) {
        closeOnWindow    = WINDOW->pWindow;
        closeOnWorkspace = WINDOW->pWindow->m_workspace;
        rememberWindowSelection(WINDOW->pWindow.lock());
        raiseFloatingWindow(WINDOW->pWindow.lock());
        return;
    }

    if (const auto WORKSPACE = workspaceAt(lastMousePosLocal); WORKSPACE && WORKSPACE->pWorkspace)
        closeOnWorkspace = WORKSPACE->pWorkspace;
}

int64_t CScrollOverview::selectedWorkspaceID() const {
    if (closeOnWindow && closeOnWindow->m_workspace)
        return closeOnWindow->m_workspace->m_id;

    if (closeOnWorkspace)
        return closeOnWorkspace->m_id;

    return startedOn ? startedOn->m_id : WORKSPACE_INVALID;
}

PHLWINDOW CScrollOverview::selectedWindow() const {
    return closeOnWindow.lock();
}

void CScrollOverview::handlePointerMotion(const Vector2D& local) {
    inputState.lastPosLocal = local;

    if (keyboardSelectionLocked && local.distance(keyboardTakeoverMousePos) >= 12.0) {
        releaseKeyboardTakeoverMouse(g_hyprviewConfig.mouse.selectFollowsHover);
        mouseSnapPanBlockedUntilExit = true;
    }

    updateMouseEdgeNavigation(local);

    if (inputState.mode == ePointerMode::VIEW_PAN) {
        updateViewportPan(local);
        return;
    }

    if (inputState.mode == ePointerMode::PRESS_PENDING) {
        if (!inputState.windowDrag || !inputState.windowDrag->window) {
            if (inputState.pressPosLocal.distance(local) >= DRAG_THRESHOLD)
                cancelPointerInteraction();
            return;
        }

        if (inputState.pressPosLocal.distance(local) >= DRAG_THRESHOLD)
            beginWindowDrag();
    }

    if (inputState.mode == ePointerMode::WINDOW_DRAG) {
        updateWindowDrag(local);
        return;
    }

    highlightHoverDebug();
}

void CScrollOverview::handlePointerPress(uint32_t button) {
    if (inputState.mode != ePointerMode::IDLE)
        return;

    rebuildGeometryCache();

    inputState               = {};
    inputState.pressPosLocal = lastMousePosLocal;
    inputState.lastPosLocal  = lastMousePosLocal;
    inputState.pressedButton = button;

    if (button == BTN_RIGHT) {
        inputState.mode              = ePointerMode::VIEW_PAN;
        inputState.pannedWorkspace   = workspaceAt(lastMousePosLocal);
        inputState.contentPanOnPress = horizontalPanForWorkspace(inputState.pannedWorkspace);
        return;
    }

    if (button != BTN_LEFT) {
        inputState = {};
        return;
    }

    inputState.mode        = ePointerMode::PRESS_PENDING;
    const auto DRAG_ENTRY  = windowAt(lastMousePosLocal);
    const auto DRAG_WINDOW = DRAG_ENTRY ? windowForEntry(DRAG_ENTRY) : PHLWINDOW{};

    if (!windowCanDragAcrossWorkspaces(DRAG_WINDOW)) {
        inputState.windowDrag.reset();
        return;
    }

    inputState.windowDrag = SWindowDragState{.window          = DRAG_WINDOW,
                                             .sourceBox       = DRAG_ENTRY ? DRAG_ENTRY->overviewBox : CBox{},
                                             .grabOffsetLocal = DRAG_ENTRY ? lastMousePosLocal - DRAG_ENTRY->overviewBox.pos() : Vector2D{}};
}

void CScrollOverview::handlePointerRelease(uint32_t button) {
    if (button != inputState.pressedButton)
        return;

    if (inputState.mode == ePointerMode::VIEW_PAN) {
        cancelPointerInteraction(false);
        highlightHoverDebug();
        return;
    }

    if (inputState.mode == ePointerMode::WINDOW_DRAG) {
        finishWindowDrag();
        return;
    }

    if (inputState.mode == ePointerMode::PRESS_PENDING) {
        cancelPointerInteraction(false);
        selectHoveredWorkspace();
        close();
    }
}

void CScrollOverview::handlePointerAxis(IPointer::SAxisEvent event) {
    if (event.axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
        return;

    if (!g_hyprviewConfig.scrolling.scrollMovesUpDown) {
        const auto VAL            = std::clamp(sc<float>(scale->value() + event.delta / -500.F), 0.05F, 0.95F);
        *scale                    = VAL;
        wheelWorkspaceScrollAccum = 0.0;
    } else if (event.source == WL_POINTER_AXIS_SOURCE_WHEEL || event.mouse || event.deltaDiscrete != 0) {
        wheelWorkspaceScrollAccum = 0.0;
        moveViewportWorkspace(event.delta > 0);
    } else {
        if (wheelWorkspaceScrollAccum != 0.0 && std::signbit(wheelWorkspaceScrollAccum) != std::signbit(event.delta))
            wheelWorkspaceScrollAccum = 0.0;

        wheelWorkspaceScrollAccum += event.delta;

        if (std::abs(wheelWorkspaceScrollAccum) >= SMOOTH_SCROLL_WORKSPACE_THRESHOLD) {
            moveViewportWorkspace(wheelWorkspaceScrollAccum > 0.0);
            wheelWorkspaceScrollAccum = 0.0;
        }
    }

    if (inputState.mode == ePointerMode::WINDOW_DRAG)
        updateWindowDrag(lastMousePosLocal);
    else if (inputState.mode == ePointerMode::IDLE)
        highlightHoverDebug();
}

void CScrollOverview::beginWindowDrag() {
    const auto DRAG_WINDOW = inputState.windowDrag ? inputState.windowDrag->window.lock() : PHLWINDOW{};
    const auto TARGET      = DRAG_WINDOW ? DRAG_WINDOW->layoutTarget() : nullptr;
    if (!inputState.windowDrag || !windowCanDragAcrossWorkspaces(DRAG_WINDOW) || !TARGET) {
        cancelPointerInteraction();
        return;
    }

    closeOnWindow    = DRAG_WINDOW;
    closeOnWorkspace = DRAG_WINDOW->m_workspace;
    rememberWindowSelection(DRAG_WINDOW);

    inputState.windowDrag->originalWorkspace    = DRAG_WINDOW->m_workspace;
    inputState.windowDrag->startedTiled         = !TARGET->floating();
    inputState.windowDrag->originalFloatingSize = TARGET->lastFloatingSize();
    inputState.windowDrag->originalGlobalBox    = TARGET->position();

    raiseFloatingWindow(DRAG_WINDOW);

    inputState.mode = ePointerMode::WINDOW_DRAG;
    updateWindowDrag(inputState.lastPosLocal);
}

void CScrollOverview::updateWindowDrag(const Vector2D& local) {
    inputState.lastPosLocal = local;
    rebuildGeometryCache();

    if (inputState.windowDrag) {
        if (const auto ENTRY = renderedWindowEntryForWindow(inputState.windowDrag->window.lock()); ENTRY && !ENTRY->overviewBox.empty())
            inputState.windowDrag->sourceBox = ENTRY->overviewBox;
    }

    inputState.dropTarget = dropTargetAt(local);
    damage();
}

void CScrollOverview::updateViewportPan(const Vector2D& local) {
    if (!inputState.pannedWorkspace || !workspaceUsesScrollingLayout(inputState.pannedWorkspace->pWorkspace)) {
        Hyprview::telemetryLog(std::format("event=pointer-pan-skip reason=invalid-or-not-scrolling workspace={} local={}",
                                           workspaceID(inputState.pannedWorkspace ? inputState.pannedWorkspace->pWorkspace : PHLWORKSPACE{}), Hyprview::formatVector(local)));
        return;
    }

    const auto DELTA_X = (local.x - inputState.pressPosLocal.x) / scale->value();
    Hyprview::telemetryLog(std::format("event=pointer-pan workspace={} press={} local={} scale={:.5f} contentPanOnPress={:.2f} deltaX={:.2f} requestedPan={:.2f}",
                                       workspaceID(inputState.pannedWorkspace->pWorkspace), Hyprview::formatVector(inputState.pressPosLocal), Hyprview::formatVector(local),
                                       scale->value(), inputState.contentPanOnPress, DELTA_X, inputState.contentPanOnPress - DELTA_X));
    setHorizontalPanForWorkspace(inputState.pannedWorkspace, inputState.contentPanOnPress - DELTA_X);
    highlightHoverDebug(false);
}

bool CScrollOverview::snapMousePanForWorkspace(const SP<SWorkspaceEntry>& workspace, int direction) {
    if (!workspace || !workspace->pWorkspace || direction == 0)
        return false;

    const auto RANGE = horizontalPanRangeForWorkspace(workspace);
    if (RANGE.max - RANGE.min <= 0.5) {
        Hyprview::telemetryLog(std::format("event=mouse-snap-pan-skip reason=no-pan-range workspace={} direction={} range={:.2f},{:.2f}", workspaceID(workspace->pWorkspace),
                                           direction, RANGE.min, RANGE.max));
        return false;
    }

    const double     SCALE        = std::max(0.1F, scale->value());
    const double     TARGET_X     = workspace->overviewBox.middle().x;
    const double     CURRENT_PAN  = horizontalPanForWorkspace(workspace);
    const double     CENTER_GRACE = 1.0;

    SP<SWindowEntry> target;
    double           bestScore = std::numeric_limits<double>::max();

    for (const auto& image : workspace->windowEntries) {
        if (!image || !image->pWindow || image->overviewBox.empty())
            continue;

        const auto WINDOW = image->pWindow.lock();
        if (!WINDOW || WINDOW->m_isFloating)
            continue;

        const double CENTER = image->overviewBox.middle().x;
        if (direction > 0 && CENTER <= TARGET_X + CENTER_GRACE)
            continue;
        if (direction < 0 && CENTER >= TARGET_X - CENTER_GRACE)
            continue;

        const double score = std::abs(CENTER - TARGET_X);

        if (score < bestScore) {
            target    = image;
            bestScore = score;
        }
    }

    if (!target) {
        Hyprview::telemetryLog(std::format("event=mouse-snap-pan-skip reason=no-target workspace={} direction={} currentPan={:.2f} targetX={:.2f} range={:.2f},{:.2f}",
                                           workspaceID(workspace->pWorkspace), direction, CURRENT_PAN, TARGET_X, RANGE.min, RANGE.max));
        return false;
    }

    const double DELTA = target->overviewBox.middle().x - TARGET_X;
    Hyprview::telemetryLog(
        std::format("event=mouse-snap-pan workspace={} direction={} targetWindow={} targetBox={} targetX={:.2f} currentPan={:.2f} delta={:.2f} requestedPan={:.2f} "
                    "scale={:.5f}",
                    workspaceID(workspace->pWorkspace), direction, windowID(target->pWindow.lock()), Hyprview::formatBox(target->overviewBox), TARGET_X, CURRENT_PAN, DELTA,
                    CURRENT_PAN + DELTA / SCALE, SCALE));
    return setHorizontalPanForWorkspace(workspace, CURRENT_PAN + DELTA / SCALE, true);
}

bool CScrollOverview::updateMouseWorkspaceEdgeNavigation(const Vector2D& local) {
    if (!g_hyprviewConfig.mouse.edgeNavigation || keyboardSelectionLocked || inputState.mode != ePointerMode::IDLE || !pMonitor)
        return false;

    const double ZONE = std::max(0.0, sc<double>(g_hyprviewConfig.scrolling.edgeScrollZone));
    if (ZONE <= 0.0)
        return false;

    int    direction = 0;
    double strength  = 0.0;

    if (local.y < ZONE) {
        direction = -1;
        strength  = (ZONE - std::max(0.0, local.y)) / ZONE;
    } else if (local.y > pMonitor->m_size.y - ZONE) {
        direction = 1;
        strength  = (local.y - (pMonitor->m_size.y - ZONE)) / ZONE;
    }

    if (direction == 0) {
        mouseEdgeNavigationDirection = 0;
        return false;
    }

    if (g_hyprviewConfig.mouse.edgeNavigationSnap) {
        if (mouseEdgeNavigationDirection == direction)
            return true;

        mouseEdgeNavigationDirection = direction;
        moveViewportWorkspace(direction > 0);
        return true;
    }

    mouseEdgeNavigationDirection = 0;

    const auto SPEED = std::max(0.0F, g_hyprviewConfig.mouse.edgeNavigationSpeed) * 0.08 * pMonitor->m_size.y;
    moveViewportBy(direction * strength * SPEED, false, false);
    return true;
}

void CScrollOverview::updateMouseSnapPanNavigation(const Vector2D& local) {
    if (!g_hyprviewConfig.mouse.snapPan || keyboardSelectionLocked || inputState.mode != ePointerMode::IDLE || !pMonitor) {
        mouseSnapPanDirection = 0;
        mouseSnapPanWorkspace.reset();
        return;
    }

    const double ZONE = std::max(0.0, sc<double>(g_hyprviewConfig.mouse.snapPanZone));
    if (ZONE <= 0.0) {
        mouseSnapPanDirection = 0;
        mouseSnapPanWorkspace.reset();
        mouseSnapPanBlockedUntilExit = false;
        return;
    }

    int direction = 0;
    if (local.x < ZONE)
        direction = -1;
    else if (local.x > pMonitor->m_size.x - ZONE)
        direction = 1;

    if (direction == 0) {
        mouseSnapPanDirection = 0;
        mouseSnapPanWorkspace.reset();
        mouseSnapPanBlockedUntilExit = false;
        return;
    }

    if (mouseSnapPanBlockedUntilExit) {
        mouseSnapPanDirection = direction;
        mouseSnapPanWorkspace.reset();
        return;
    }

    SP<SWorkspaceEntry> WORKSPACE;
    for (auto it = workspaceEntries.rbegin(); it != workspaceEntries.rend(); ++it) {
        const auto& image = *it;
        if (!image || !image->pWorkspace)
            continue;

        if (local.y >= image->overviewBox.y && local.y <= image->overviewBox.y + image->overviewBox.h) {
            WORKSPACE = image;
            break;
        }
    }

    if (!WORKSPACE || !workspaceUsesScrollingLayout(WORKSPACE->pWorkspace)) {
        mouseSnapPanDirection = 0;
        mouseSnapPanWorkspace.reset();
        return;
    }

    if (mouseSnapPanWorkspace == WORKSPACE->pWorkspace && mouseSnapPanDirection == direction)
        return;

    if (snapMousePanForWorkspace(WORKSPACE, direction)) {
        mouseSnapPanWorkspace = WORKSPACE->pWorkspace;
        mouseSnapPanDirection = direction;
        highlightHoverDebug(false);
    }
}

void CScrollOverview::updateMouseEdgeNavigation(const Vector2D& local) {
    if (updateMouseWorkspaceEdgeNavigation(local))
        return;

    updateMouseSnapPanNavigation(local);
}

void CScrollOverview::updateEdgeAutoscroll(uint64_t nowMs) {
    if (inputState.mode != ePointerMode::WINDOW_DRAG || !pMonitor) {
        lastEdgeScrollMs = 0;
        return;
    }

    const auto ZONE = std::max(0.0, sc<double>(g_hyprviewConfig.scrolling.edgeScrollZone));
    if (ZONE <= 0.0) {
        lastEdgeScrollMs = 0;
        return;
    }

    double direction = 0.0;
    double strength  = 0.0;

    if (lastMousePosLocal.y < ZONE) {
        direction = -1.0;
        strength  = (ZONE - std::max(0.0, lastMousePosLocal.y)) / ZONE;
    } else if (lastMousePosLocal.y > pMonitor->m_size.y - ZONE) {
        direction = 1.0;
        strength  = (lastMousePosLocal.y - (pMonitor->m_size.y - ZONE)) / ZONE;
    }

    if (direction == 0.0) {
        lastEdgeScrollMs = 0;
        return;
    }

    if (lastEdgeScrollMs == 0) {
        lastEdgeScrollMs = nowMs;
        g_pCompositor->scheduleFrameForMonitor(pMonitor.lock());
        return;
    }

    const auto DELTA_MS = nowMs > lastEdgeScrollMs ? nowMs - lastEdgeScrollMs : 0;
    lastEdgeScrollMs    = nowMs;

    if (DELTA_MS == 0) {
        g_pCompositor->scheduleFrameForMonitor(pMonitor.lock());
        return;
    }

    const auto DT_SECONDS = std::min(sc<double>(DELTA_MS) / 1000.0, 0.05);
    const auto SPEED      = std::max(0.0F, g_hyprviewConfig.scrolling.edgeScrollSpeed) * pMonitor->m_size.y;

    if (moveViewportBy(direction * strength * SPEED * DT_SECONDS, true))
        updateWindowDrag(lastMousePosLocal);

    g_pCompositor->scheduleFrameForMonitor(pMonitor.lock());
}

void CScrollOverview::updateHoverActivation(uint64_t nowMs) {
    if (inputState.mode != ePointerMode::WINDOW_DRAG || inputState.dropTarget.type != eDropTargetType::WORKSPACE_BODY || !hasDropTarget()) {
        hoverActivateWorkspace.reset();
        hoverActivateStartedMs = 0;
        return;
    }

    const auto TARGET = inputState.dropTarget.workspace;
    if (hoverActivateWorkspace != TARGET) {
        hoverActivateWorkspace = TARGET;
        hoverActivateStartedMs = nowMs;
        return;
    }

    const auto DELAY_MS = std::max(0, g_hyprviewConfig.scrolling.hoverActivateMs);
    if (DELAY_MS == 0 || nowMs - hoverActivateStartedMs < sc<uint64_t>(DELAY_MS))
        return;

    if (focusWorkspaceInViewport(TARGET, true))
        updateWindowDrag(lastMousePosLocal);

    hoverActivateStartedMs = nowMs;
}

void CScrollOverview::resetDragNavigationState() {
    lastEdgeScrollMs       = 0;
    hoverActivateStartedMs = 0;
    hoverActivateWorkspace.reset();
}

CScrollOverview::SDropTarget CScrollOverview::dropTargetAt(const Vector2D& local) {
    for (auto it = insertionMarkers.rbegin(); it != insertionMarkers.rend(); ++it) {
        if (it->box.containsPoint(local))
            return {.type = eDropTargetType::WORKSPACE_INSERTION, .markerBox = it->box, .targetWorkspaceID = it->workspaceID, .label = it->label};
    }

    const auto DRAG_WINDOW       = inputState.windowDrag ? inputState.windowDrag->window.lock() : PHLWINDOW{};
    const bool DRAGGING_FLOATING = DRAG_WINDOW && DRAG_WINDOW->layoutTarget() && DRAG_WINDOW->layoutTarget()->floating();

    for (auto it = workspaceEntries.rbegin(); it != workspaceEntries.rend(); ++it) {
        const auto& WORKSPACE = *it;
        if (!WORKSPACE || !WORKSPACE->pWorkspace)
            continue;

        const auto DROP_BOX = DRAGGING_FLOATING ? WORKSPACE->overviewBox : workspaceDropBox(WORKSPACE);
        if (DROP_BOX.empty() || !DROP_BOX.containsPoint(local))
            continue;

        return {.type = eDropTargetType::WORKSPACE_BODY, .workspace = WORKSPACE, .markerBox = DROP_BOX};
    }

    return {};
}

bool CScrollOverview::finishWindowDrag() {
    const bool MOVED = moveDraggedWindowToDropTarget();
    cancelPointerInteraction(false);
    highlightHoverDebug(false);
    damage();
    if (pMonitor)
        g_pCompositor->scheduleFrameForMonitor(pMonitor.lock());

    return MOVED;
}

void CScrollOverview::cancelPointerInteraction(bool damageOnChange) {
    const bool HAD_INTERACTION = inputState.mode != ePointerMode::IDLE;
    inputState                 = {};
    resetDragNavigationState();

    if (damageOnChange && HAD_INTERACTION)
        damage();
}

CBox CScrollOverview::workspaceDropBox(const SP<SWorkspaceEntry>& workspace) const {
    if (!workspace || !workspace->pWorkspace)
        return {};

    CBox box = workspace->overviewBox;
    if (!workspaceUsesScrollingLayout(workspace->pWorkspace))
        return box;

    const auto   RANGE = horizontalPanRangeForWorkspace(workspace);
    const double PAN   = horizontalPanForWorkspace(workspace);
    const double SCALE = std::max(0.01, scale ? sc<double>(scale->value()) : 1.0);

    box.x -= std::max(0.0, PAN - RANGE.min) * SCALE;
    box.w += std::max(0.0, RANGE.max - RANGE.min) * SCALE;

    return box;
}

SP<CScrollOverview::SWindowEntry> CScrollOverview::dropAnchorEntry(const SP<SWorkspaceEntry>& workspace, PHLWINDOW ignoredWindow, CBox* anchorBox) const {
    if (!workspace)
        return nullptr;

    const auto       IGNORED = overviewWindowToRender(ignoredWindow);

    SP<SWindowEntry> best;
    CBox             bestBox;
    double           bestDistance = std::numeric_limits<double>::max();

    for (const bool floating : {true, false}) {
        for (auto it = workspace->windowEntries.rbegin(); it != workspace->windowEntries.rend(); ++it) {
            const auto& ENTRY  = *it;
            const auto  WINDOW = windowForEntry(ENTRY);
            if (!ENTRY || !WINDOW || WINDOW == IGNORED || WINDOW->m_isFloating != floating || !ENTRY->liveVisible)
                continue;

            if (ENTRY->overviewBox.containsPoint(lastMousePosLocal)) {
                if (anchorBox)
                    *anchorBox = ENTRY->overviewBox;
                return ENTRY;
            }

            const auto HITBOX = expandedWindowHitBox(ENTRY);
            if (HITBOX.empty() || !HITBOX.containsPoint(lastMousePosLocal))
                continue;

            const auto DISTANCE = distanceToBox(lastMousePosLocal, ENTRY->overviewBox);
            if (DISTANCE >= bestDistance)
                continue;

            best         = ENTRY;
            bestBox      = ENTRY->overviewBox;
            bestDistance = DISTANCE;
        }

        if (best)
            break;
    }

    if (best && anchorBox)
        *anchorBox = bestBox;

    return best;
}

Vector2D CScrollOverview::overviewPointToGlobal(const SP<SWorkspaceEntry>& workspace, const Vector2D& local) const {
    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || !workspace || !workspace->pWorkspace || !scale || !viewOffset)
        return local;

    const auto INDEX = workspaceEntryIndex(workspace->pWorkspace);
    if (!INDEX)
        return local;

    const double   SAFE_SCALE      = std::max(sc<double>(scale->value()), 0.01);
    const auto     VIEWPORT_CENTER = CBox{{}, MONITOR->m_size}.middle();
    const double   WORKSPACE_YOFF  = (sc<double>(*INDEX) - sc<double>(activeWorkspaceEntryIndex())) * workspaceOverviewStep() * SAFE_SCALE;
    const Vector2D UNPROJECTED     = (local - Vector2D{0.0, WORKSPACE_YOFF} + viewOffset->value() * SAFE_SCALE - VIEWPORT_CENTER) * (1.0 / SAFE_SCALE);

    return UNPROJECTED + VIEWPORT_CENTER + MONITOR->m_position;
}

CBox CScrollOverview::floatingDropGlobalBox(PHLWINDOW window, PHLWORKSPACE targetWorkspace, const SP<SWorkspaceEntry>& targetEntry) const {
    const auto MONITOR = pMonitor.lock();

    Vector2D   size = window && window->layoutTarget() ? window->layoutTarget()->position().size() : Vector2D{};
    if (inputState.windowDrag && inputState.windowDrag->originalGlobalBox.w > 0.0 && inputState.windowDrag->originalGlobalBox.h > 0.0)
        size = inputState.windowDrag->originalGlobalBox.size();

    if (size.x <= 0.0 || size.y <= 0.0)
        size = window ? window->m_realSize->goal() : Vector2D{1.0, 1.0};

    CBox box;
    if (targetEntry) {
        const auto DROP_BOX = draggedLiveWindowBox();
        box                 = {overviewPointToGlobal(targetEntry, DROP_BOX.pos()), size};
    } else {
        box = centerBoxInWorkspace({Vector2D{}, size}, targetWorkspace, MONITOR);
    }

    return clampBoxToWorkspace(box, targetWorkspace, MONITOR, window ? window->getRealBorderSize() : 0.0);
}

bool CScrollOverview::commitFloatingWindowDrop(PHLWINDOW window, PHLWORKSPACE targetWorkspace, const SP<SWorkspaceEntry>& targetEntry) {
    if (!window || !targetWorkspace || !window->layoutTarget())
        return false;

    const auto TARGET     = window->layoutTarget();
    const auto GLOBAL_BOX = floatingDropGlobalBox(window, targetWorkspace, targetEntry);
    const bool MOVING     = window->m_workspace != targetWorkspace;

    TARGET->damageEntire();

    if (MOVING)
        g_pCompositor->moveWindowToWorkspaceSafe(window, targetWorkspace);

    TARGET->rememberFloatingSize(GLOBAL_BOX.size());
    TARGET->setPositionGlobal(GLOBAL_BOX);
    TARGET->warpPositionSize();
    TARGET->damageEntire();

    return true;
}

bool CScrollOverview::commitTiledWindowDrop(PHLWINDOW window, PHLWORKSPACE targetWorkspace, const SP<SWorkspaceEntry>& targetEntry) {
    if (!window || !targetWorkspace || !window->layoutTarget())
        return false;

    const auto TARGET = window->layoutTarget();
    const bool MOVING = window->m_workspace != targetWorkspace;

    CBox       anchorBox;
    const auto ANCHOR_ENTRY  = dropAnchorEntry(targetEntry, window, &anchorBox);
    const auto ANCHOR_WINDOW = ANCHOR_ENTRY ? windowForEntry(ANCHOR_ENTRY) : PHLWINDOW{};

    int        horizontalDropSide = 0;
    if (targetEntry) {
        if (lastMousePosLocal.x < targetEntry->overviewBox.x)
            horizontalDropSide = -1;
        else if (lastMousePosLocal.x > targetEntry->overviewBox.x + targetEntry->overviewBox.w)
            horizontalDropSide = 1;
    }

    if (!ANCHOR_ENTRY && horizontalDropSide == 0 && targetEntry) {
        double minWindowX = std::numeric_limits<double>::max();
        double maxWindowX = std::numeric_limits<double>::lowest();
        bool   found      = false;

        for (const auto& ENTRY : targetEntry->windowEntries) {
            const auto WINDOW = windowForEntry(ENTRY);
            if (!ENTRY || !WINDOW || WINDOW == window || !ENTRY->liveVisible)
                continue;

            minWindowX = std::min(minWindowX, ENTRY->overviewBox.x);
            maxWindowX = std::max(maxWindowX, ENTRY->overviewBox.x + ENTRY->overviewBox.w);
            found      = true;
        }

        if (found) {
            if (lastMousePosLocal.x < minWindowX)
                horizontalDropSide = -1;
            else if (lastMousePosLocal.x > maxWindowX)
                horizontalDropSide = 1;
        }
    }

    std::string dropDirection;
    if (ANCHOR_ENTRY && !anchorBox.empty()) {
        const auto LOCAL_X = lastMousePosLocal.x - anchorBox.x;
        const auto LOCAL_Y = lastMousePosLocal.y - anchorBox.y;

        if (LOCAL_X < anchorBox.w / 3.0)
            dropDirection = "l";
        else if (LOCAL_X > anchorBox.w * 2.0 / 3.0)
            dropDirection = "r";
        else
            dropDirection = LOCAL_Y < anchorBox.h / 2.0 ? "u" : "d";
    }

    TARGET->damageEntire();

    if (MOVING)
        g_pCompositor->moveWindowToWorkspaceSafe(window, targetWorkspace);

    bool committed = MOVING;

    if (ANCHOR_WINDOW && ANCHOR_WINDOW != window && ANCHOR_WINDOW->layoutTarget()) {
        if (scrollingAlgorithmForTarget(TARGET)) {
            committed = moveScrollingTargetNextToWindow(TARGET, ANCHOR_WINDOW, dropDirection) || committed;
        } else if (TARGET->space() && ANCHOR_WINDOW->layoutTarget()->space() == TARGET->space()) {
            ANCHOR_WINDOW->layoutTarget()->damageEntire();
            g_layoutManager->switchTargets(TARGET, ANCHOR_WINDOW->layoutTarget(), true);
            ANCHOR_WINDOW->layoutTarget()->damageEntire();
            committed = true;
        }
    } else if (scrollingAlgorithmForTarget(TARGET)) {
        committed = moveScrollingTargetToHorizontalEdge(TARGET, horizontalDropSide) || committed;
    }

    if (inputState.windowDrag)
        TARGET->rememberFloatingSize(inputState.windowDrag->originalFloatingSize);

    TARGET->warpPositionSize();
    TARGET->damageEntire();

    if (const auto WORKSPACE = TARGET->workspace())
        WORKSPACE->updateWindows();

    return committed;
}

bool CScrollOverview::moveDraggedWindowToDropTarget() {
    if (inputState.mode != ePointerMode::WINDOW_DRAG || !hasDropTarget() || !inputState.windowDrag)
        return false;

    const auto DROP_TARGET      = inputState.dropTarget;
    const auto WINDOW           = inputState.windowDrag->window.lock();
    const auto TARGET_WORKSPACE = workspaceForDropTarget(DROP_TARGET);
    if (!windowCanDragAcrossWorkspaces(WINDOW) || !TARGET_WORKSPACE)
        return false;

    const auto TARGET_ENTRY = DROP_TARGET.type == eDropTargetType::WORKSPACE_BODY ? DROP_TARGET.workspace : workspaceEntryForWorkspace(TARGET_WORKSPACE);
    const bool COMMITTED =
        inputState.windowDrag->startedTiled ? commitTiledWindowDrop(WINDOW, TARGET_WORKSPACE, TARGET_ENTRY) : commitFloatingWindowDrop(WINDOW, TARGET_WORKSPACE, TARGET_ENTRY);

    if (!COMMITTED)
        return false;

    activateWorkspace(TARGET_WORKSPACE, false);
    if (WINDOW->m_isFloating)
        raiseFloatingWindow(WINDOW);
    Desktop::focusState()->fullWindowFocus(WINDOW, Desktop::FOCUS_REASON_KEYBIND);
    refreshWorkspaceEntries(TARGET_WORKSPACE, false);

    return true;
}

void CScrollOverview::raiseFloatingWindow(PHLWINDOW window) {
    if (!validMapped(window) || !window->m_isFloating)
        return;

    g_pCompositor->changeWindowZOrder(window, true);
}

bool CScrollOverview::hasDropTarget() const {
    if (inputState.mode != ePointerMode::WINDOW_DRAG || inputState.dropTarget.type == eDropTargetType::NONE || inputState.dropTarget.markerBox.empty())
        return false;

    if (inputState.dropTarget.type == eDropTargetType::WORKSPACE_BODY)
        return inputState.dropTarget.workspace && inputState.dropTarget.workspace->pWorkspace;

    return inputState.dropTarget.type == eDropTargetType::WORKSPACE_INSERTION && inputState.dropTarget.targetWorkspaceID != WORKSPACE_INVALID;
}

void CScrollOverview::highlightHoverDebug(bool damageOnChange) {
    rebuildGeometryCache();

    const auto OLD_HOVERED_WINDOW    = hoveredWindow;
    const auto OLD_HOVERED_WORKSPACE = hoveredWorkspace;
    const auto OLD_KEYBOARD_WINDOW   = keyboardSelectedWindow;

    hoveredWindow.reset();
    hoveredWorkspace.reset();

    clearWindowHighlights();

    if (const auto WINDOW = windowAt(lastMousePosLocal); WINDOW && WINDOW->pWindow) {
        hoveredWindow    = WINDOW->pWindow;
        hoveredWorkspace = WINDOW->pWindow->m_workspace;

        if (!g_hyprviewConfig.keyboard.enabled)
            WINDOW->highlight = true;
        else if (g_hyprviewConfig.mouse.selectFollowsHover && !keyboardSelectionLocked)
            setKeyboardSelection(WINDOW, false, false);
    } else if (const auto WORKSPACE = workspaceAt(lastMousePosLocal); WORKSPACE && WORKSPACE->pWorkspace) {
        hoveredWorkspace = WORKSPACE->pWorkspace;
    }

    if (g_hyprviewConfig.keyboard.enabled) {
        if (!entryForKeyboardSelection())
            syncSelectionToViewport(false);

        if (const auto SELECTED = entryForKeyboardSelection())
            SELECTED->highlight = true;
    }

    if (damageOnChange && (OLD_HOVERED_WINDOW != hoveredWindow || OLD_HOVERED_WORKSPACE != hoveredWorkspace || OLD_KEYBOARD_WINDOW != keyboardSelectedWindow))
        damage();
}
