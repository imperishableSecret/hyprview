#include "ScrollOverview.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

#define private   public
#define protected public
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#undef protected
#undef private

void CScrollOverview::clearWindowHighlights() {
    for (const auto& wimg : images) {
        if (!wimg)
            continue;

        for (const auto& img : wimg->windowImages) {
            if (img)
                img->highlight = false;
        }
    }
}

void CScrollOverview::rememberWindowSelection(PHLWINDOW window) {
    if (!g_hyprviewConfig.keyboard.enabled || !g_hyprviewConfig.keyboard.rememberSelection || !window || !window->m_workspace)
        return;

    rememberedSelection[window->m_workspace->m_id] = window;
}

void CScrollOverview::keyboardTakeoverMouse() {
    if (!pMonitor)
        return;

    keyboardTakeoverMousePos = lastMousePosLocal;
}

void CScrollOverview::releaseKeyboardTakeoverMouse(bool allowHoverSelection) {
    if (allowHoverSelection)
        keyboardSelectionLocked = false;
}

void CScrollOverview::pruneRememberedSelections() {
    std::erase_if(rememberedSelection, [this](const auto& entry) {
        const auto WINDOW = entry.second.lock();
        return !WINDOW || !imageForWindow(WINDOW);
    });
}

SP<CScrollOverview::SWorkspaceImage> CScrollOverview::workspaceImageForWindow(PHLWINDOW window) const {
    if (!window)
        return nullptr;

    for (const auto& wimg : images) {
        if (!wimg || !wimg->pWorkspace)
            continue;

        if (windowBelongsToWorkspaceInOverview(window, wimg->pWorkspace))
            return wimg;
    }

    return nullptr;
}

SP<CScrollOverview::SWorkspaceImage> CScrollOverview::workspaceImageForWindowImage(const SP<SWindowImage>& image) const {
    if (!image)
        return nullptr;

    for (const auto& wimg : images) {
        if (!wimg)
            continue;

        for (const auto& img : wimg->windowImages) {
            if (img == image)
                return wimg;
        }
    }

    return nullptr;
}

SP<CScrollOverview::SWindowImage> CScrollOverview::selectableImageForWorkspace(const SP<SWorkspaceImage>& workspace) const {
    if (!workspace || !workspace->pWorkspace)
        return nullptr;

    if (g_hyprviewConfig.keyboard.rememberSelection) {
        const auto REMEMBERED = rememberedSelection.find(workspace->pWorkspace->m_id);
        if (REMEMBERED != rememberedSelection.end()) {
            if (const auto WINDOW = REMEMBERED->second.lock()) {
                if (const auto IMAGE = imageForWindow(WINDOW); IMAGE && workspaceImageForWindowImage(IMAGE) == workspace)
                    return IMAGE;
            }
        }
    }

    if (const auto FOCUSED = Desktop::focusState()->window()) {
        if (windowBelongsToWorkspaceInOverview(FOCUSED, workspace->pWorkspace)) {
            if (const auto IMAGE = imageForWindow(FOCUSED))
                return IMAGE;
        }
    }

    for (const auto& img : workspace->windowImages) {
        if (img && img->pWindow)
            return img;
    }

    return nullptr;
}

SP<CScrollOverview::SWindowImage> CScrollOverview::imageForKeyboardSelection() const {
    const auto WINDOW = keyboardSelectedWindow.lock();
    if (!WINDOW)
        return nullptr;

    return imageForWindow(WINDOW);
}

void CScrollOverview::centerWindowImageInScrollingWorkspace(SP<SWindowImage> image, bool animate) {
    if (!pMonitor || !image || !image->pWindow)
        return;

    const auto WINDOW = image->pWindow.lock();
    if (!WINDOW || WINDOW->m_isFloating || !WINDOW->m_realPosition || !WINDOW->m_realSize)
        return;

    const auto WORKSPACE = workspaceImageForWindowImage(image);
    if (!WORKSPACE || !workspaceUsesScrollingLayout(WORKSPACE->pWorkspace))
        return;

    const auto   TARGET_POS    = WINDOW->m_realPosition->goal();
    const auto   TARGET_SIZE   = WINDOW->m_realSize->goal();
    const double TARGET_CENTER = TARGET_POS.x - pMonitor->m_position.x + TARGET_SIZE.x / 2.0;
    const double TARGET_PAN    = TARGET_CENTER - pMonitor->m_size.x / 2.0;
    const double delta         = TARGET_PAN - horizontalPanForWorkspace(WORKSPACE);

    if (std::abs(delta) < 0.5)
        return;

    setHorizontalPanForWorkspace(WORKSPACE, TARGET_PAN, animate);
}

void CScrollOverview::ensureSelectionVisible(SP<SWindowImage> image) {
    centerWindowImageInScrollingWorkspace(image, true);
}

void CScrollOverview::setKeyboardSelection(SP<SWindowImage> image, bool lockedToKeyboard, bool damageOnChange) {
    const auto OLD_SELECTION = keyboardSelectedWindow;
    const auto WINDOW        = image && image->pWindow ? image->pWindow.lock() : PHLWINDOW{};

    if (WINDOW) {
        keyboardSelectedWindow  = WINDOW;
        keyboardSelectionLocked = lockedToKeyboard;
        if (lockedToKeyboard) {
            rememberWindowSelection(WINDOW);
            keyboardTakeoverMouse();

            ensureSelectionVisible(image);

            if (g_hyprviewConfig.keyboard.focusFollowsSelection && Desktop::focusState()->window() != WINDOW)
                Desktop::focusState()->fullWindowFocus(WINDOW, Desktop::FOCUS_REASON_KEYBIND);
        }
    } else {
        keyboardSelectedWindow.reset();
        keyboardSelectionLocked = false;
    }

    if (damageOnChange && OLD_SELECTION != keyboardSelectedWindow)
        damage();
}

void CScrollOverview::syncSelectionToViewport(bool damageOnChange) {
    if (!g_hyprviewConfig.keyboard.enabled) {
        setKeyboardSelection(nullptr, false, damageOnChange);
        rememberedSelection.clear();
        restoredSelectionWorkspace.reset();
        return;
    }

    pruneRememberedSelections();

    if (images.empty() || viewportCurrentWorkspace >= images.size()) {
        setKeyboardSelection(nullptr, false, damageOnChange);
        restoredSelectionWorkspace.reset();
        return;
    }

    const auto OLD_SELECTION = keyboardSelectedWindow.lock();
    setKeyboardSelection(selectableImageForWorkspace(images[viewportCurrentWorkspace]), keyboardSelectionLocked, damageOnChange);

    const auto NEW_SELECTION = keyboardSelectedWindow.lock();
    if (OLD_SELECTION != NEW_SELECTION && NEW_SELECTION && NEW_SELECTION->m_workspace)
        restoredSelectionWorkspace = NEW_SELECTION->m_workspace;
}

bool CScrollOverview::moveHorizontalSelection(bool right) {
    rebuildGeometryCache();
    pruneRememberedSelections();

    auto CURRENT = imageForKeyboardSelection();
    if (!CURRENT) {
        syncSelectionToViewport(false);
        CURRENT = imageForKeyboardSelection();
    }

    auto WORKSPACE = workspaceImageForWindowImage(CURRENT);
    if (!WORKSPACE && viewportCurrentWorkspace < images.size())
        WORKSPACE = images[viewportCurrentWorkspace];

    if (!CURRENT || !WORKSPACE)
        return false;

    const auto CURRENT_CENTER = CURRENT->overviewBox.middle();
    auto       best           = SP<SWindowImage>{};
    auto       bestScore      = std::numeric_limits<double>::max();

    for (const auto& candidate : WORKSPACE->windowImages) {
        if (!candidate || !candidate->pWindow || candidate == CURRENT)
            continue;

        const auto CENTER  = candidate->overviewBox.middle();
        const auto PRIMARY = right ? CENTER.x - CURRENT_CENTER.x : CURRENT_CENTER.x - CENTER.x;
        if (PRIMARY <= 0.5)
            continue;

        const auto CROSS = std::abs(CENTER.y - CURRENT_CENTER.y);
        const auto SCORE = PRIMARY * PRIMARY + CROSS * CROSS * 4.0;
        if (SCORE < bestScore) {
            best      = candidate;
            bestScore = SCORE;
        }
    }

    if (!best && g_hyprviewConfig.keyboard.wrap) {
        for (const auto& candidate : WORKSPACE->windowImages) {
            if (!candidate || !candidate->pWindow || candidate == CURRENT)
                continue;

            const auto CENTER = candidate->overviewBox.middle();
            const auto CROSS  = std::abs(CENTER.y - CURRENT_CENTER.y);
            const auto SCORE  = (right ? CENTER.x : -CENTER.x) + CROSS * 0.001;
            if (SCORE < bestScore) {
                best      = candidate;
                bestScore = SCORE;
            }
        }
    }

    if (!best)
        return false;

    setKeyboardSelection(best, true, false);
    highlightHoverDebug(true);
    return true;
}

bool CScrollOverview::moveSelection(Vector2D direction) {
    if (!g_hyprviewConfig.keyboard.enabled || closing || images.empty())
        return false;

    keyboardSelectionLocked = true;
    keyboardTakeoverMouse();

    if (std::abs(direction.x) >= std::abs(direction.y))
        return moveHorizontalSelection(direction.x > 0.0);

    const bool MOVE_DOWN = direction.y > 0.0;
    bool       moved     = moveViewportWorkspace(MOVE_DOWN);

    if (!moved && g_hyprviewConfig.keyboard.wrap && !images.empty()) {
        if (MOVE_DOWN && viewportCurrentWorkspace == images.size() - 1)
            moved = setViewportWorkspace(0, false, true);
        else if (!MOVE_DOWN && viewportCurrentWorkspace == 0)
            moved = setViewportWorkspace(images.size() - 1, false, true);
    }

    if (!moved)
        return false;

    syncSelectionToViewport(false);
    keyboardSelectionLocked = keyboardSelectedWindow.lock() != nullptr;
    rememberWindowSelection(keyboardSelectedWindow.lock());
    highlightHoverDebug(true);
    return true;
}

bool CScrollOverview::activateSelection() {
    if (!g_hyprviewConfig.keyboard.enabled || closing)
        return false;

    rebuildGeometryCache();
    pruneRememberedSelections();

    auto IMAGE = imageForKeyboardSelection();
    if (!IMAGE) {
        syncSelectionToViewport(false);
        IMAGE = imageForKeyboardSelection();
    }

    const auto WINDOW = IMAGE && IMAGE->pWindow ? IMAGE->pWindow.lock() : PHLWINDOW{};
    if (!WINDOW || !WINDOW->m_workspace)
        return false;

    closeOnWindow    = WINDOW;
    closeOnWorkspace = WINDOW->m_workspace;
    rememberWindowSelection(WINDOW);
    raiseFloatingWindow(WINDOW);

    if (g_hyprviewConfig.keyboard.activationClosesOverview) {
        close(true);
        return true;
    }

    activateWorkspace(WINDOW->m_workspace);
    Desktop::focusState()->fullWindowFocus(WINDOW, Desktop::FOCUS_REASON_KEYBIND);
    queueFocusedWindowCursorSync(WINDOW);
    refreshWorkspaceImages(WINDOW->m_workspace, false);
    return true;
}
