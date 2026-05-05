#include "ScrollOverview.hpp"
#include "../../plugin/LuaConfig.hpp"
#include <algorithm>
#include <any>
#include <cmath>
#include <linux/input-event-codes.h>

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

static void removeOverview(WP<Hyprutils::Animation::CBaseAnimatedVariable> thisptr) {
    g_pOverview.reset();
}

static float hyprlerp(const float& from, const float& to, const float perc) {
    return (to - from) * perc + from;
}

static Vector2D hyprlerp(const Vector2D& from, const Vector2D& to, const float perc) {
    return Vector2D{hyprlerp(from.x, to.x, perc), hyprlerp(from.y, to.y, perc)};
}

CScrollOverview::~CScrollOverview() {
    g_pHyprOpenGL->makeEGLCurrent();
    images.clear(); // otherwise we get a vram leak
    Cursor::overrideController->unsetOverride(Cursor::CURSOR_OVERRIDE_SPECIAL_ACTION);
}

CScrollOverview::CScrollOverview(PHLWORKSPACE startedOn_, bool swipe_) : startedOn(startedOn_), swipe(swipe_) {
    const auto PMONITOR = Desktop::focusState()->monitor();
    pMonitor            = PMONITOR;

    for (const auto& w : g_pCompositor->getWorkspaces()) {
        if (w && w->m_monitor == pMonitor && !w->m_isSpecialWorkspace && workspaceVisibleInOverview(w.lock()))
            images.emplace_back(makeShared<SWorkspaceImage>(w.lock()));
    }

    std::sort(images.begin(), images.end(), [](const auto& a, const auto& b) { return a->pWorkspace->m_id < b->pWorkspace->m_id; });

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

        info.cancelled = true;

        if (!cursorSyncWarping && cursorSyncUntilMs != 0 && inputState.mode == ePointerMode::IDLE) {
            const auto NOW = Time::millis(Time::steadyNow());
            if (NOW <= cursorSyncUntilMs) {
                if (const auto WINDOW = queuedCursorWindow.lock(); centerCursorOnWindowImage(WINDOW))
                    return;
            } else
                clearFocusedWindowCursorSync();
        }

        lastMousePosLocal = g_pInputManager->getMouseCoordsInternal() - pMonitor->m_position;
        handlePointerMotion(lastMousePosLocal);
    };

    auto onCursorButton = [this](IPointer::SButtonEvent e, Event::SCallbackInfo& info) {
        if (closing)
            return;

        info.cancelled = true;

        if (e.state == WL_POINTER_BUTTON_STATE_PRESSED)
            handlePointerPress(e.button);
        else if (e.state == WL_POINTER_BUTTON_STATE_RELEASED)
            handlePointerRelease(e.button);
    };

    auto onMouseAxis = [this](IPointer::SAxisEvent e, Event::SCallbackInfo& info) {
        if (closing)
            return;

        info.cancelled = true;
        handlePointerAxis(e);
    };

    auto refreshForWindow = [this](PHLWINDOW window, bool warpViewport) {
        if (closing)
            return;

        queueRefreshWorkspaceImages(window && window->m_workspace ? window->m_workspace : (pMonitor ? pMonitor->m_activeWorkspace : nullptr), warpViewport);
    };

    auto onWindowOpen  = [refreshForWindow](PHLWINDOW window) { refreshForWindow(window, true); };
    auto onWindowClose = [refreshForWindow](PHLWINDOW window) { refreshForWindow(window, false); };

    auto onWindowMove = [this](PHLWINDOW window, PHLWORKSPACE workspace) {
        if (closing || !pMonitor || !workspace)
            return;

        if (workspace->m_monitor == pMonitor || (window && window->m_monitor == pMonitor))
            queueRefreshWorkspaceImages(workspace, false);
    };

    auto onWindowActive = [this](PHLWINDOW window, Desktop::eFocusReason reason) {
        if (closing || !pMonitor || inputState.mode != ePointerMode::IDLE || !window || !window->m_workspace || window->m_workspace->m_monitor != pMonitor)
            return;

        if (reason != Desktop::FOCUS_REASON_KEYBIND)
            return;

        rebuildGeometryCache();

        if (images.empty() || viewportCurrentWorkspace >= images.size() || !images[viewportCurrentWorkspace]) {
            damage();
            return;
        }

        if (images[viewportCurrentWorkspace]->pWorkspace != window->m_workspace) {
            const auto WORKSPACE = workspaceImageForWindow(window);
            if (!WORKSPACE) {
                damage();
                return;
            }

            focusWorkspaceInViewport(WORKSPACE, false, false);
            rebuildGeometryCache();
        }

        if (const auto RESTORED = restoredSelectionWorkspace.lock(); RESTORED && RESTORED == window->m_workspace) {
            const auto SELECTED = keyboardSelectedWindow.lock();
            if (SELECTED && SELECTED != window && SELECTED->m_workspace == window->m_workspace) {
                restoredSelectionWorkspace.reset();
                damage();
                return;
            }
        }

        restoredSelectionWorkspace.reset();

        if (const auto IMAGE = imageForWindow(window))
            setKeyboardSelection(IMAGE, true, false);

        queueFocusedWindowCursorSync(window);

        damage();
    };

    auto onWorkspaceActive = [this](PHLWORKSPACE workspace) {
        if (closing || !pMonitor || !workspace || workspace->m_isSpecialWorkspace || workspace->m_monitor != pMonitor)
            return;

        queueRefreshWorkspaceImages(workspace, false);
    };

    mouseMoveHook = Event::bus()->m_events.input.mouse.move.listen([onCursorMove](Vector2D, Event::SCallbackInfo& info) { onCursorMove(info); });
    touchMoveHook = Event::bus()->m_events.input.touch.motion.listen([onCursorMove](ITouch::SMotionEvent, Event::SCallbackInfo& info) { onCursorMove(info); });
    mouseAxisHook = Event::bus()->m_events.input.mouse.axis.listen([onMouseAxis](IPointer::SAxisEvent e, Event::SCallbackInfo& info) { onMouseAxis(e, info); });

    mouseButtonHook = Event::bus()->m_events.input.mouse.button.listen([onCursorButton](IPointer::SButtonEvent e, Event::SCallbackInfo& info) { onCursorButton(e, info); });
    touchDownHook   = Event::bus()->m_events.input.touch.down.listen([this](ITouch::SDownEvent, Event::SCallbackInfo& info) {
        if (closing)
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

    redrawAll();

    size_t activeIdx = 0;
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i]->pWorkspace && images[i]->pWorkspace == startedOn) {
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
    restoredSelectionWorkspace.reset();
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

    if (switchToSelection && !TARGET_IS_ACTIVE) {
        if (TARGET_WORKSPACE && TARGET_WORKSPACE != pMonitor->m_activeWorkspace) {
            g_pDesktopAnimationManager->startAnimation(pMonitor->m_activeWorkspace, CDesktopAnimationManager::ANIMATION_TYPE_OUT, true, true);
            g_pDesktopAnimationManager->startAnimation(TARGET_WORKSPACE, CDesktopAnimationManager::ANIMATION_TYPE_IN, false, true);
            pMonitor->changeWorkspace(TARGET_WORKSPACE, true, true, true);
        }

        if (TARGET_WINDOW)
            Desktop::focusState()->fullWindowFocus(TARGET_WINDOW, Desktop::FOCUS_REASON_KEYBIND);
    }

    if (TARGET_WINDOW)
        keyboardSelectedWindow = TARGET_WINDOW;

    rebuildGeometryCache();

    const auto FINAL_WORKSPACE = TARGET_WORKSPACE ? TARGET_WORKSPACE : pMonitor->m_activeWorkspace;
    if (const auto WORKSPACE_IMAGE = imageForWorkspace(FINAL_WORKSPACE))
        setHorizontalPanForWorkspace(WORKSPACE_IMAGE, 0.0, true);

    *viewOffset = Vector2D{};

    *scale = 1.F;

    scale->setCallbackOnEnd(removeOverview);
}

void CScrollOverview::onPreRender() {
    if (pMonitor)
        pMonitor->m_solitaryClient.reset();

    if (!closing) {
        damageDirty = false;
        if (redrawDirtyWindowImages())
            damage();
    }

    if (!closing && cursorSyncUntilMs != 0 && inputState.mode == ePointerMode::IDLE) {
        const auto NOW = Time::millis(Time::steadyNow());
        if (NOW <= cursorSyncUntilMs) {
            const auto WINDOW = queuedCursorWindow.lock();

            if (centerCursorOnWindowImage(WINDOW))
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

    queueRefreshWorkspaceImages(pMonitor->m_activeWorkspace, false);
}

void CScrollOverview::render() {
    if (!closing && inputState.mode == ePointerMode::IDLE)
        highlightHoverDebug(false);

    g_pHyprRenderer->m_renderPass.add(makeUnique<COverviewPassElement>());
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
}

void CScrollOverview::onSwipeUpdate(double delta) {
    m_isSwiping = true;

    const auto  DISTANCE = std::max(1, g_hyprviewConfig.gestureDistance);
    const float PERC     = closing ? std::clamp(delta / sc<double>(DISTANCE), 0.0, 1.0) : 1.0 - std::clamp(delta / sc<double>(DISTANCE), 0.0, 1.0);

    scale->setValueAndWarp(hyprlerp(1.F, scrollingDefaultZoom(), PERC));
}

void CScrollOverview::onSwipeEnd() {
    if (closing) {
        close();
        return;
    }

    (*scale)    = scrollingDefaultZoom();
    m_isSwiping = false;
}
