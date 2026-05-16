#include "ScrollOverview.hpp"
#include "../../plugin/LuaConfig.hpp"
#include <algorithm>
#include <any>
#include <cmath>
#include <linux/input-event-codes.h>
#include <wayland-server-core.h>
#include <wlr-layer-shell-unstable-v1.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

#define private   public
#define protected public
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/shared/animation/AnimationTree.hpp>
#include <hyprland/src/managers/animation/AnimationManager.hpp>
#include <hyprland/src/managers/animation/DesktopAnimationManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/cursor/CursorShapeOverrideController.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>
#undef protected
#undef private

#include "../OverviewPassElement.hpp"

using Render::GL::g_pHyprOpenGL;

static float scrollingDefaultZoom() {
    return std::clamp(g_hyprviewConfig.scrolling.defaultZoom, 0.1F, 0.9F);
}

static void damageMonitor(WP<Hyprutils::Animation::CBaseAnimatedVariable> thisptr) {
    g_pOverview->damage();
}

static void removeOverviewNow() {
    const auto OVERVIEW = g_pOverview;
    const auto MONITOR  = OVERVIEW && OVERVIEW->pMonitor ? OVERVIEW->pMonitor.lock() : PHLMONITOR{};

    g_pHyprRenderer->m_renderPass.removeAllOfType("COverviewPassElement");
    g_pOverview.reset();

    if (MONITOR) {
        g_pHyprRenderer->damageMonitor(MONITOR);
        g_pCompositor->scheduleFrameForMonitor(MONITOR);
    }
}

static void requestNativeWorkspaceHandoff(WP<Hyprutils::Animation::CBaseAnimatedVariable> thisptr) {
    const auto OVERVIEW = g_pOverview;
    const auto MONITOR  = OVERVIEW && OVERVIEW->pMonitor ? OVERVIEW->pMonitor.lock() : PHLMONITOR{};

    if (!OVERVIEW || !MONITOR) {
        removeOverviewNow();
        return;
    }

    g_pHyprRenderer->damageMonitor(MONITOR);
    g_pCompositor->scheduleFrameForMonitor(MONITOR);
}

static float hyprlerp(const float& from, const float& to, const float perc) {
    return (to - from) * perc + from;
}

static Vector2D hyprlerp(const Vector2D& from, const Vector2D& to, const float perc) {
    return Vector2D{hyprlerp(from.x, to.x, perc), hyprlerp(from.y, to.y, perc)};
}

static bool focusReasonShouldSyncSelection(Desktop::eFocusReason reason) {
    return Desktop::isHardInputFocusReason(reason) || reason == Desktop::FOCUS_REASON_SWITCH_TO_WINDOW_SOFT || reason == Desktop::FOCUS_REASON_DISPATCH_FOCUSWINDOW ||
        reason == Desktop::FOCUS_REASON_GROUP_CURRENT_WINDOW_CHANGE || reason == Desktop::FOCUS_REASON_DISPATCH_MOVEWINDOWINTOGROUP;
}

CScrollOverview::~CScrollOverview() {
    g_pHyprOpenGL->makeEGLCurrent();
    resetSurfacePolicyCache();
    resetLayerRenderCache();
    if (realtimePreviewTimer) {
        wl_event_source_remove(realtimePreviewTimer);
        realtimePreviewTimer = nullptr;
    }
    workspaceEntries.clear(); // otherwise we get a vram leak
    restoreForcedSurfaceVisibility();
    restoreForcedWindowVisibility();
    Cursor::overrideController->unsetOverride(Cursor::CURSOR_OVERRIDE_SPECIAL_ACTION);
}

CScrollOverview::CScrollOverview(PHLWORKSPACE startedOn_, bool swipe_) : startedOn(startedOn_), swipe(swipe_) {
    const auto PMONITOR = Desktop::focusState()->monitor();
    pMonitor            = PMONITOR;

    realtimePreviewTimer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, realtimePreviewTimerCallback, this);

    for (const auto& w : g_pCompositor->getWorkspaces()) {
        if (w && w->m_monitor == pMonitor && !w->m_isSpecialWorkspace && workspaceVisibleInOverview(w.lock()))
            workspaceEntries.emplace_back(makeShared<SWorkspaceEntry>(w.lock()));
    }

    std::sort(workspaceEntries.begin(), workspaceEntries.end(), [](const auto& a, const auto& b) { return a->pWorkspace->m_id < b->pWorkspace->m_id; });

    g_pAnimationManager->createAnimation(1.F, scale, Config::animationTree()->getAnimationPropertyConfig("windowsMove"), AVARDAMAGE_NONE);
    g_pAnimationManager->createAnimation({}, viewOffset, Config::animationTree()->getAnimationPropertyConfig("windowsMove"), AVARDAMAGE_NONE);

    scale->setUpdateCallback(damageMonitor);
    viewOffset->setUpdateCallback(damageMonitor);

    if (!swipe)
        *scale = scrollingDefaultZoom();

    lastMousePosLocal = g_pInputManager->getMouseCoordsInternal() - pMonitor->m_position;

    auto onCursorMove = [this](Event::SCallbackInfo& info) {
        if (closing)
            return;

        const auto LOCAL = g_pInputManager->getMouseCoordsInternal() - pMonitor->m_position;
        if (pointerOverBlockingLayerSurface(LOCAL)) {
            lastMousePosLocal = LOCAL;
            return;
        }

        info.cancelled = true;

        if (!cursorSyncWarping && cursorSyncUntilMs != 0 && inputState.mode == ePointerMode::IDLE) {
            const auto NOW = Time::millis(Time::steadyNow());
            if (NOW <= cursorSyncUntilMs) {
                if (const auto WINDOW = queuedCursorWindow.lock(); centerCursorOnWindowEntry(WINDOW))
                    return;
            } else
                clearFocusedWindowCursorSync();
        }

        lastMousePosLocal = LOCAL;
        handlePointerMotion(lastMousePosLocal);
    };

    auto onCursorButton = [this](IPointer::SButtonEvent e, Event::SCallbackInfo& info) {
        if (closing)
            return;

        const auto LOCAL = g_pInputManager->getMouseCoordsInternal() - pMonitor->m_position;
        if (pointerOverBlockingLayerSurface(LOCAL)) {
            lastMousePosLocal = LOCAL;
            return;
        }

        info.cancelled    = true;
        lastMousePosLocal = LOCAL;

        if (e.state == WL_POINTER_BUTTON_STATE_PRESSED)
            handlePointerPress(e.button);
        else if (e.state == WL_POINTER_BUTTON_STATE_RELEASED)
            handlePointerRelease(e.button);
    };

    auto onMouseAxis = [this](IPointer::SAxisEvent e, Event::SCallbackInfo& info) {
        if (closing)
            return;

        const auto LOCAL = g_pInputManager->getMouseCoordsInternal() - pMonitor->m_position;
        if (pointerOverBlockingLayerSurface(LOCAL)) {
            lastMousePosLocal = LOCAL;
            return;
        }

        info.cancelled    = true;
        lastMousePosLocal = LOCAL;
        handlePointerAxis(e);
    };

    auto refreshForWindow = [this](PHLWINDOW window, bool warpViewport) {
        if (closing)
            return;

        queueRefreshWorkspaceEntries(window && window->m_workspace ? window->m_workspace : (pMonitor ? pMonitor->m_activeWorkspace : nullptr), warpViewport);
    };

    auto onWindowOpen  = [refreshForWindow](PHLWINDOW window) { refreshForWindow(window, true); };
    auto onWindowClose = [this, refreshForWindow](PHLWINDOW window) {
        if (closing)
            return;

        if (window && renderedWindowEntryForWindow(window)) {
            resetSurfacePolicyCache();
            markGeometryCacheDirty(GEOMETRY_DIRTY_WINDOW_GEOMETRY);
            damage();
            if (pMonitor)
                g_pCompositor->scheduleFrameForMonitor(pMonitor.lock());
            return;
        }

        refreshForWindow(window, false);
    };

    auto onWindowMove = [this](PHLWINDOW window, PHLWORKSPACE workspace) {
        if (closing || !pMonitor || !workspace)
            return;

        if (workspace->m_monitor == pMonitor || (window && window->m_monitor == pMonitor))
            queueRefreshWorkspaceEntries(workspace, false);
    };

    auto onWindowActive = [this](PHLWINDOW window, Desktop::eFocusReason reason) {
        if (closing || !pMonitor || inputState.mode != ePointerMode::IDLE || !window || !window->m_workspace || window->m_workspace->m_monitor != pMonitor)
            return;

        if (!focusReasonShouldSyncSelection(reason))
            return;

        rebuildGeometryCache();

        if (workspaceEntries.empty() || viewportCurrentWorkspace >= workspaceEntries.size() || !workspaceEntries[viewportCurrentWorkspace]) {
            damage();
            return;
        }

        if (workspaceEntries[viewportCurrentWorkspace]->pWorkspace != window->m_workspace) {
            const auto WORKSPACE = workspaceEntryForWindow(window);
            if (!WORKSPACE) {
                damage();
                return;
            }

            focusWorkspaceInViewport(WORKSPACE, false, false);
            rebuildGeometryCache();
        }

        if (const auto ENTRY = windowEntryForWindow(window))
            setKeyboardSelection(ENTRY, true, false);

        queueFocusedWindowCursorSync(window);

        damage();
    };

    auto onWorkspaceActive = [this](PHLWORKSPACE workspace) {
        if (closing || !pMonitor || !workspace || workspace->m_isSpecialWorkspace || workspace->m_monitor != pMonitor)
            return;

        queueRefreshWorkspaceEntries(workspace, false);
    };

    mouseMoveHook = Event::bus()->m_events.input.mouse.move.listen([onCursorMove](Vector2D, Event::SCallbackInfo& info) { onCursorMove(info); });
    touchMoveHook = Event::bus()->m_events.input.touch.motion.listen([onCursorMove](ITouch::SMotionEvent, Event::SCallbackInfo& info) { onCursorMove(info); });
    mouseAxisHook = Event::bus()->m_events.input.mouse.axis.listen([onMouseAxis](IPointer::SAxisEvent e, Event::SCallbackInfo& info) { onMouseAxis(e, info); });

    mouseButtonHook = Event::bus()->m_events.input.mouse.button.listen([onCursorButton](IPointer::SButtonEvent e, Event::SCallbackInfo& info) { onCursorButton(e, info); });
    touchDownHook   = Event::bus()->m_events.input.touch.down.listen([this](ITouch::SDownEvent e, Event::SCallbackInfo& info) {
        if (closing)
            return;

        if (pMonitor && pointerOverBlockingLayerSurface(e.pos * pMonitor->m_size))
            return;

        info.cancelled = true;

        selectHoveredWorkspace();
        close();
    });

    windowOpenHook      = Event::bus()->m_events.window.open.listen(onWindowOpen);
    windowCloseHook     = Event::bus()->m_events.window.close.listen(onWindowClose);
    windowMoveHook      = Event::bus()->m_events.window.moveToWorkspace.listen(onWindowMove);
    windowActiveHook    = Event::bus()->m_events.window.active.listen(onWindowActive);
    workspaceActiveHook = Event::bus()->m_events.workspace.active.listen(onWorkspaceActive);

    Cursor::overrideController->setOverride("left_ptr", Cursor::CURSOR_OVERRIDE_SPECIAL_ACTION);

    rebuildAllWorkspaceEntries();

    size_t activeIdx = 0;
    for (size_t i = 0; i < workspaceEntries.size(); ++i) {
        if (workspaceEntries[i]->pWorkspace && workspaceEntries[i]->pWorkspace == startedOn) {
            activeIdx = i;
            break;
        }
    }

    viewportCurrentWorkspace = activeIdx;
    syncSelectionToViewport(false);
    highlightHoverDebug();
}

void CScrollOverview::close(bool switchToSelection) {
    const bool WAS_CLOSING = closing;

    cancelPointerInteraction(false);
    pendingAnchorWorkspace.reset();
    clearFocusedWindowCursorSync();
    releaseKeyboardTakeoverMouse(false);
    viewOffset->setCallbackOnEnd(nullptr);
    closing = true;

    if (!WAS_CLOSING)
        Hyprview::runOnCloseLuaCallback();

    const auto FOCUSED_WINDOW = Desktop::focusState()->window();
    if (!closeOnWindow && !closeOnWorkspace)
        closeOnWindow = FOCUSED_WINDOW;

    const auto TARGET_WINDOW    = switchToSelection ? closeOnWindow.lock() : FOCUSED_WINDOW;
    const auto TARGET_WORKSPACE = switchToSelection ? (closeOnWindow && closeOnWindow->m_workspace ? closeOnWindow->m_workspace : closeOnWorkspace.lock()) :
                                                      (FOCUSED_WINDOW && FOCUSED_WINDOW->m_workspace ? FOCUSED_WINDOW->m_workspace : pMonitor->m_activeWorkspace);
    const bool TARGET_IS_ACTIVE = (!TARGET_WINDOW || TARGET_WINDOW == FOCUSED_WINDOW) && TARGET_WORKSPACE == pMonitor->m_activeWorkspace;
    const auto FINAL_WORKSPACE  = TARGET_WORKSPACE ? TARGET_WORKSPACE : pMonitor->m_activeWorkspace;
    closeFrameWindow            = TARGET_WINDOW;

    if (switchToSelection && !TARGET_IS_ACTIVE) {
        if (TARGET_WORKSPACE && TARGET_WORKSPACE != pMonitor->m_activeWorkspace)
            activateWorkspace(TARGET_WORKSPACE, false);

        if (TARGET_WINDOW)
            Desktop::focusState()->fullWindowFocus(TARGET_WINDOW, Desktop::FOCUS_REASON_KEYBIND);
    }

    if (TARGET_WINDOW)
        keyboardSelectedWindow = TARGET_WINDOW;
    if (FINAL_WORKSPACE)
        reanchorViewportToWorkspace(FINAL_WORKSPACE, true);

    rebuildGeometryCache();

    if (const auto WORKSPACE_ENTRY = workspaceEntryForWorkspace(FINAL_WORKSPACE))
        setHorizontalPanForWorkspace(WORKSPACE_ENTRY, 0.0, true);

    *viewOffset = Vector2D{};

    *scale = 1.F;
    markGeometryCacheDirty(GEOMETRY_DIRTY_VIEWPORT | GEOMETRY_DIRTY_INSERTION_MARKERS);

    scale->setCallbackOnEnd(requestNativeWorkspaceHandoff);
}

void CScrollOverview::onPreRender() {
    resetSurfacePolicyCache();
    resetLayerRenderCache();

    if (pMonitor)
        pMonitor->m_solitaryClient.reset();

    if (!closing && cursorSyncUntilMs != 0 && inputState.mode == ePointerMode::IDLE) {
        const auto NOW = Time::millis(Time::steadyNow());
        if (NOW <= cursorSyncUntilMs) {
            const auto WINDOW = queuedCursorWindow.lock();

            if (centerCursorOnWindowEntry(WINDOW))
                g_pCompositor->scheduleFrameForMonitor(pMonitor.lock());
        } else
            clearFocusedWindowCursorSync();
    }

    if (inputState.mode != ePointerMode::WINDOW_DRAG) {
        resetDragNavigationState();
        return;
    }

    const auto NOW_MS = Time::millis(Time::steadyNow());
    updateEdgeAutoscroll(NOW_MS);
    updateHoverActivation(NOW_MS);

    if (pMonitor && hasDropTarget())
        g_pCompositor->scheduleFrameForMonitor(pMonitor.lock());
}

void CScrollOverview::onWorkspaceChange() {
    if (!pMonitor || closing)
        return;

    queueRefreshWorkspaceEntries(pMonitor->m_activeWorkspace, false);
}

void CScrollOverview::render() {
    if (!closing && inputState.mode == ePointerMode::IDLE)
        highlightHoverDebug(false);

    renderOverviewLive(Time::steadyNow());
}

bool CScrollOverview::shouldRenderNativeWorkspace() const {
    if (!closing || !scale || !viewOffset)
        return false;

    static constexpr float  SCALE_EPSILON  = 0.002F;
    static constexpr double OFFSET_EPSILON = 0.5;

    return std::abs(scale->value() - 1.F) <= SCALE_EPSILON && viewOffset->value().distance({}) <= OFFSET_EPSILON;
}

void CScrollOverview::finishNativeWorkspaceHandoff() {
    if (!closing)
        return;

    removeOverviewNow();
}

void CScrollOverview::setClosing(bool closing_) {
    closing = closing_;
}

void CScrollOverview::resetSwipe() {
    if (closing) {
        close();
        return;
    }

    (*scale)    = scrollingDefaultZoom();
    m_isSwiping = false;
    markGeometryCacheDirty(GEOMETRY_DIRTY_VIEWPORT | GEOMETRY_DIRTY_INSERTION_MARKERS);
}

void CScrollOverview::onSwipeUpdate(double delta) {
    m_isSwiping = true;

    const auto  DISTANCE = std::max(1, g_hyprviewConfig.gestureDistance);
    const float PERC     = closing ? std::clamp(delta / sc<double>(DISTANCE), 0.0, 1.0) : 1.0 - std::clamp(delta / sc<double>(DISTANCE), 0.0, 1.0);

    scale->setValueAndWarp(hyprlerp(1.F, scrollingDefaultZoom(), PERC));
    markGeometryCacheDirty(GEOMETRY_DIRTY_VIEWPORT | GEOMETRY_DIRTY_INSERTION_MARKERS);
}

void CScrollOverview::onSwipeEnd() {
    if (closing) {
        close();
        return;
    }

    (*scale)    = scrollingDefaultZoom();
    m_isSwiping = false;
    markGeometryCacheDirty(GEOMETRY_DIRTY_VIEWPORT | GEOMETRY_DIRTY_INSERTION_MARKERS);
}
