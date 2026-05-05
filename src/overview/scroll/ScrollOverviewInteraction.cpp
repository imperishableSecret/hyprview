#include "ScrollOverview.hpp"
#include <algorithm>
#include <any>
#include <cmath>
#include <linux/input-event-codes.h>
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

bool                    CScrollOverview::pointerOverBlockingLayerSurface(const Vector2D& local) const {
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
        if (!inputState.draggedImage || !inputState.draggedWindow) {
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

    inputState.mode          = ePointerMode::PRESS_PENDING;
    inputState.draggedImage  = windowAt(lastMousePosLocal);
    inputState.draggedWindow = inputState.draggedImage && inputState.draggedImage->pWindow ? inputState.draggedImage->pWindow : PHLWINDOWREF{};

    if (!windowCanDragAcrossWorkspaces(inputState.draggedWindow.lock())) {
        inputState.draggedImage.reset();
        inputState.draggedWindow.reset();
    }

    if (inputState.draggedImage)
        inputState.dragOffsetLocal = lastMousePosLocal - inputState.draggedImage->overviewBox.pos();
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
    if (!inputState.draggedImage || !windowCanDragAcrossWorkspaces(inputState.draggedWindow.lock())) {
        cancelPointerInteraction();
        return;
    }

    raiseFloatingWindow(inputState.draggedWindow.lock());

    inputState.mode = ePointerMode::WINDOW_DRAG;
    updateWindowDrag(inputState.lastPosLocal);
}

void CScrollOverview::updateWindowDrag(const Vector2D& local) {
    inputState.lastPosLocal = local;
    rebuildGeometryCache();
    inputState.dropTarget = dropTargetAt(local);
    damage();
}

void CScrollOverview::updateViewportPan(const Vector2D& local) {
    if (!inputState.pannedWorkspace || !workspaceUsesScrollingLayout(inputState.pannedWorkspace->pWorkspace))
        return;

    const auto DELTA_X = (local.x - inputState.pressPosLocal.x) / scale->value();
    setHorizontalPanForWorkspace(inputState.pannedWorkspace, inputState.contentPanOnPress - DELTA_X);
    highlightHoverDebug(false);
}

bool CScrollOverview::snapMousePanForWorkspace(const SP<SWorkspaceImage>& workspace, int direction) {
    if (!workspace || !workspace->pWorkspace || direction == 0)
        return false;

    const auto RANGE = horizontalPanRangeForWorkspace(workspace);
    if (RANGE.max - RANGE.min <= 0.5)
        return false;

    const double     SCALE        = std::max(0.1F, scale->value());
    const double     TARGET_X     = workspace->overviewBox.middle().x;
    const double     CURRENT_PAN  = horizontalPanForWorkspace(workspace);
    const double     CENTER_GRACE = 1.0;

    SP<SWindowImage> target;
    double           bestScore = std::numeric_limits<double>::max();

    for (const auto& image : workspace->windowImages) {
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

    if (!target)
        return false;

    const double DELTA = target->overviewBox.middle().x - TARGET_X;
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

    SP<SWorkspaceImage> WORKSPACE;
    for (auto it = images.rbegin(); it != images.rend(); ++it) {
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

    const auto WORKSPACE = workspaceAt(local);
    if (!WORKSPACE || !WORKSPACE->pWorkspace)
        return {};

    return {.type = eDropTargetType::WORKSPACE_BODY, .workspace = WORKSPACE, .markerBox = WORKSPACE->overviewBox};
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

bool CScrollOverview::moveDraggedWindowToDropTarget() {
    if (inputState.mode != ePointerMode::WINDOW_DRAG || !hasDropTarget())
        return false;

    const auto DROP_TARGET      = inputState.dropTarget;
    const auto WINDOW           = inputState.draggedWindow.lock();
    const auto TARGET_WORKSPACE = workspaceForDropTarget(DROP_TARGET);
    if (!windowCanDragAcrossWorkspaces(WINDOW) || !TARGET_WORKSPACE)
        return false;

    const auto OLD_WORKSPACE = WINDOW->m_workspace;

    if (OLD_WORKSPACE != TARGET_WORKSPACE)
        g_pCompositor->moveWindowToWorkspaceSafe(WINDOW, TARGET_WORKSPACE);

    activateWorkspace(TARGET_WORKSPACE, false);
    raiseFloatingWindow(WINDOW);
    Desktop::focusState()->fullWindowFocus(WINDOW, Desktop::FOCUS_REASON_KEYBIND);

    refreshWorkspaceImages(TARGET_WORKSPACE, false);

    return OLD_WORKSPACE != TARGET_WORKSPACE;
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
        if (!imageForKeyboardSelection())
            syncSelectionToViewport(false);

        if (const auto SELECTED = imageForKeyboardSelection())
            SELECTED->highlight = true;
    }

    if (damageOnChange && (OLD_HOVERED_WINDOW != hoveredWindow || OLD_HOVERED_WORKSPACE != hoveredWorkspace || OLD_KEYBOARD_WINDOW != keyboardSelectedWindow))
        damage();
}
