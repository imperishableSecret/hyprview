#include "ScrollOverview.hpp"
#include "../../plugin/Telemetry.hpp"
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

namespace {
    std::string windowID(PHLWINDOW window) {
        return window ? Hyprview::formatRawPtr(window.get()) : "0";
    }

    std::string workspaceID(PHLWORKSPACE workspace) {
        return workspace ? std::to_string(workspace->m_id) : "<none>";
    }
}

void CScrollOverview::clearWindowHighlights() {
    for (const auto& workspaceEntry : workspaceEntries) {
        if (!workspaceEntry)
            continue;

        for (const auto& entry : workspaceEntry->windowEntries) {
            if (entry)
                entry->highlight = false;
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
        return !WINDOW || !windowEntryForWindow(WINDOW);
    });
}

SP<CScrollOverview::SWorkspaceEntry> CScrollOverview::workspaceEntryForWindow(PHLWINDOW window) const {
    if (!window)
        return nullptr;

    for (const auto& workspaceEntry : workspaceEntries) {
        if (!workspaceEntry || !workspaceEntry->pWorkspace)
            continue;

        if (windowBelongsToWorkspaceInOverview(window, workspaceEntry->pWorkspace))
            return workspaceEntry;
    }

    return nullptr;
}

SP<CScrollOverview::SWorkspaceEntry> CScrollOverview::workspaceEntryForWindowEntry(const SP<SWindowEntry>& image) const {
    if (!image)
        return nullptr;

    rebuildWindowEntryLookups();

    const auto IT = windowEntryWorkspaceLookup.find(image.get());
    return IT == windowEntryWorkspaceLookup.end() ? nullptr : IT->second;
}

SP<CScrollOverview::SWindowEntry> CScrollOverview::selectableEntryForWorkspace(const SP<SWorkspaceEntry>& workspace) const {
    if (!workspace || !workspace->pWorkspace)
        return nullptr;

    if (g_hyprviewConfig.keyboard.rememberSelection) {
        const auto REMEMBERED = rememberedSelection.find(workspace->pWorkspace->m_id);
        if (REMEMBERED != rememberedSelection.end()) {
            if (const auto WINDOW = REMEMBERED->second.lock()) {
                if (const auto ENTRY = windowEntryForWindow(WINDOW); ENTRY && ENTRY->liveRenderable && workspaceEntryForWindowEntry(ENTRY) == workspace)
                    return ENTRY;
            }
        }
    }

    if (const auto FOCUSED = Desktop::focusState()->window()) {
        if (windowBelongsToWorkspaceInOverview(FOCUSED, workspace->pWorkspace)) {
            if (const auto ENTRY = windowEntryForWindow(FOCUSED); ENTRY && ENTRY->liveRenderable)
                return ENTRY;
        }
    }

    for (const auto& entry : workspace->windowEntries) {
        if (entry && entry->pWindow && entry->liveRenderable)
            return entry;
    }

    return nullptr;
}

SP<CScrollOverview::SWindowEntry> CScrollOverview::entryForKeyboardSelection() const {
    const auto WINDOW = keyboardSelectedWindow.lock();
    if (!WINDOW)
        return nullptr;

    return windowEntryForWindow(WINDOW);
}

void CScrollOverview::centerWindowEntryInScrollingWorkspace(SP<SWindowEntry> image, bool animate) {
    if (!pMonitor || !image || !image->pWindow) {
        Hyprview::telemetryLogLazy([&] { return std::format("event=selection-center-skip reason=invalid-input animate={}", Hyprview::boolToken(animate)); });
        return;
    }

    const auto WINDOW = image->pWindow.lock();
    if (!WINDOW || WINDOW->m_isFloating || !WINDOW->m_realPosition || !WINDOW->m_realSize) {
        Hyprview::telemetryLogLazy([&] {
            return std::format("event=selection-center-skip reason=invalid-window window={} floating={} animate={}", windowID(WINDOW),
                               Hyprview::boolToken(WINDOW && WINDOW->m_isFloating), Hyprview::boolToken(animate));
        });
        return;
    }

    const auto WORKSPACE = workspaceEntryForWindowEntry(image);
    if (!WORKSPACE || !workspaceUsesScrollingLayout(WORKSPACE->pWorkspace)) {
        Hyprview::telemetryLogLazy([&] {
            return std::format("event=selection-center-skip reason=not-scrolling window={} workspace={} animate={}", windowID(WINDOW),
                               workspaceID(WORKSPACE ? WORKSPACE->pWorkspace : PHLWORKSPACE{}), Hyprview::boolToken(animate));
        });
        return;
    }

    const auto   TARGET_POS    = WINDOW->m_realPosition->goal();
    const auto   TARGET_SIZE   = WINDOW->m_realSize->goal();
    const double TARGET_CENTER = TARGET_POS.x - pMonitor->m_position.x + TARGET_SIZE.x / 2.0;
    const double TARGET_PAN    = TARGET_CENTER - pMonitor->m_size.x / 2.0;
    const auto   RANGE         = horizontalPanRangeForWorkspace(WORKSPACE);
    const double CURRENT_PAN   = horizontalPanForWorkspace(WORKSPACE);
    const double delta         = TARGET_PAN - CURRENT_PAN;

    Hyprview::telemetryLogLazy([&] {
        return std::format("event=selection-center-window window={} workspace={} targetPos={} targetSize={} targetCenter={:.2f} requestedPan={:.2f} currentPan={:.2f} "
                           "delta={:.2f} range={:.2f},{:.2f} animate={}",
                           windowID(WINDOW), workspaceID(WORKSPACE->pWorkspace), Hyprview::formatVector(TARGET_POS), Hyprview::formatVector(TARGET_SIZE), TARGET_CENTER, TARGET_PAN,
                           CURRENT_PAN, delta, RANGE.min, RANGE.max, Hyprview::boolToken(animate));
    });

    if (std::abs(delta) < 0.5) {
        Hyprview::telemetryLogLazy([&] {
            return std::format("event=selection-center-skip reason=already-centered window={} workspace={} currentPan={:.2f} requestedPan={:.2f}", windowID(WINDOW),
                               workspaceID(WORKSPACE->pWorkspace), CURRENT_PAN, TARGET_PAN);
        });
        return;
    }

    setHorizontalPanForWorkspace(WORKSPACE, TARGET_PAN, animate);
}

void CScrollOverview::ensureSelectionVisible(SP<SWindowEntry> image) {
    centerWindowEntryInScrollingWorkspace(image, true);
}

void CScrollOverview::setKeyboardSelection(SP<SWindowEntry> image, bool lockedToKeyboard, bool damageOnChange) {
    const auto OLD_SELECTION = keyboardSelectedWindow;
    const auto WINDOW        = image && image->pWindow ? image->pWindow.lock() : PHLWINDOW{};
    const auto ACTIVE        = Desktop::focusState()->window();

    Hyprview::telemetryLogLazy([&] {
        return std::format("event=selection-set old={} new={} newWorkspace={} locked={} damage={} focusFollowsSelection={} active={} activeWorkspace={}",
                           windowID(OLD_SELECTION.lock()), windowID(WINDOW), workspaceID(WINDOW ? WINDOW->m_workspace : PHLWORKSPACE{}), Hyprview::boolToken(lockedToKeyboard),
                           Hyprview::boolToken(damageOnChange), Hyprview::boolToken(g_hyprviewConfig.keyboard.focusFollowsSelection), windowID(ACTIVE),
                           workspaceID(ACTIVE ? ACTIVE->m_workspace : PHLWORKSPACE{}));
    });

    if (WINDOW) {
        keyboardSelectedWindow  = WINDOW;
        keyboardSelectionLocked = lockedToKeyboard;
        if (WINDOW == ACTIVE)
            rememberWindowSelection(WINDOW);
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
    Hyprview::telemetryLogLazy([&] {
        return std::format("event=selection-sync-start enabled={} workspaceEntries={} viewportIndex={} old={} locked={} damage={}",
                           Hyprview::boolToken(g_hyprviewConfig.keyboard.enabled), workspaceEntries.size(), viewportCurrentWorkspace, windowID(keyboardSelectedWindow.lock()),
                           Hyprview::boolToken(keyboardSelectionLocked), Hyprview::boolToken(damageOnChange));
    });

    if (!g_hyprviewConfig.keyboard.enabled) {
        setKeyboardSelection(nullptr, false, damageOnChange);
        rememberedSelection.clear();
        return;
    }

    pruneRememberedSelections();

    if (workspaceEntries.empty() || viewportCurrentWorkspace >= workspaceEntries.size()) {
        setKeyboardSelection(nullptr, false, damageOnChange);
        return;
    }

    const auto OLD_SELECTION = keyboardSelectedWindow.lock();
    setKeyboardSelection(selectableEntryForWorkspace(workspaceEntries[viewportCurrentWorkspace]), keyboardSelectionLocked, damageOnChange);

    const auto NEW_SELECTION = keyboardSelectedWindow.lock();

    Hyprview::telemetryLogLazy([&] {
        return std::format("event=selection-sync-end viewportIndex={} viewportWorkspace={} old={} new={}", viewportCurrentWorkspace,
                           workspaceID(workspaceEntries[viewportCurrentWorkspace] ? workspaceEntries[viewportCurrentWorkspace]->pWorkspace : PHLWORKSPACE{}),
                           windowID(OLD_SELECTION), windowID(NEW_SELECTION));
    });
}

bool CScrollOverview::moveHorizontalSelection(bool right) {
    rebuildGeometryCache();
    pruneRememberedSelections();

    auto CURRENT = entryForKeyboardSelection();
    if (!CURRENT) {
        syncSelectionToViewport(false);
        CURRENT = entryForKeyboardSelection();
    }

    auto WORKSPACE = workspaceEntryForWindowEntry(CURRENT);
    if (!WORKSPACE && viewportCurrentWorkspace < workspaceEntries.size())
        WORKSPACE = workspaceEntries[viewportCurrentWorkspace];

    if (!CURRENT || !WORKSPACE)
        return false;

    const auto CURRENT_CENTER = CURRENT->overviewBox.middle();
    auto       best           = SP<SWindowEntry>{};
    auto       bestScore      = std::numeric_limits<double>::max();

    for (const auto& candidate : WORKSPACE->windowEntries) {
        if (!candidate || !candidate->pWindow || !candidate->liveRenderable || candidate == CURRENT)
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
        for (const auto& candidate : WORKSPACE->windowEntries) {
            if (!candidate || !candidate->pWindow || !candidate->liveRenderable || candidate == CURRENT)
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
    if (!g_hyprviewConfig.keyboard.enabled || closing || workspaceEntries.empty())
        return false;

    rememberWindowSelection(keyboardSelectedWindow.lock());
    keyboardSelectionLocked = true;
    keyboardTakeoverMouse();

    if (std::abs(direction.x) >= std::abs(direction.y))
        return moveHorizontalSelection(direction.x > 0.0);

    const bool MOVE_DOWN = direction.y > 0.0;
    bool       moved     = moveViewportWorkspace(MOVE_DOWN);

    if (!moved && g_hyprviewConfig.keyboard.wrap && !workspaceEntries.empty()) {
        if (MOVE_DOWN && viewportCurrentWorkspace == workspaceEntries.size() - 1)
            moved = setViewportWorkspace(0, false, true);
        else if (!MOVE_DOWN && viewportCurrentWorkspace == 0)
            moved = setViewportWorkspace(workspaceEntries.size() - 1, false, true);
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

    auto ENTRY = entryForKeyboardSelection();
    if (!ENTRY) {
        syncSelectionToViewport(false);
        ENTRY = entryForKeyboardSelection();
    }

    const auto WINDOW = ENTRY && ENTRY->pWindow ? ENTRY->pWindow.lock() : PHLWINDOW{};
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
    refreshWorkspaceEntries(WINDOW->m_workspace, false);
    return true;
}
