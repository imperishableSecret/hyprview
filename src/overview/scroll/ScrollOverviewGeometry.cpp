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

namespace {
    constexpr double GEOMETRY_CACHE_EPSILON = 0.01;

    bool             geometryValueChanged(double a, double b) {
        return std::abs(a - b) > GEOMETRY_CACHE_EPSILON;
    }

    bool geometryValueChanged(const Vector2D& a, const Vector2D& b) {
        return geometryValueChanged(a.x, b.x) || geometryValueChanged(a.y, b.y);
    }
}

void CScrollOverview::rebuildGeometryCache() {
    if (!pMonitor) {
        markGeometryCacheDirty(GEOMETRY_DIRTY_ALL);
        return;
    }

    const auto MONITOR = pMonitor.lock();
    if (!MONITOR) {
        markGeometryCacheDirty(GEOMETRY_DIRTY_ALL);
        return;
    }

    const auto DIRTY_FLAGS            = geometryDirtyFlags;
    bool       renderedWindowRemapped = false;
    const auto VIEWPORT_CENTER        = CBox{{}, MONITOR->m_size}.middle();
    const auto WINDOW_GAP             = (std::max(0.0, static_cast<double>(g_hyprviewConfig.scrolling.windowGap)) / 2.0) * overviewStyleProgress();
    const auto WORKSPACE_STEP         = workspaceOverviewStep() * scale->value();
    float      yoff                   = -sc<float>(activeWorkspaceEntryIndex()) * WORKSPACE_STEP;

    for (const auto& workspaceEntry : workspaceEntries) {
        if (!workspaceEntry)
            continue;

        const auto CONTENT_PAN_X = horizontalPanForWorkspace(workspaceEntry);

        workspaceEntry->overviewBox = CBox{{}, MONITOR->m_size};
        workspaceEntry->overviewBox.translate(-VIEWPORT_CENTER).scale(scale->value()).translate(VIEWPORT_CENTER).translate(-viewOffset->value() * scale->value());
        workspaceEntry->overviewBox.translate({0.F, yoff});
        workspaceEntry->hitBox = workspaceEntry->overviewBox;

        for (const auto& entry : workspaceEntry->windowEntries) {
            if (!entry || !entry->pWindow)
                continue;

            const auto WINDOW = windowForEntry(entry);
            if (!WINDOW)
                continue;

            if (entry->lastRenderedWindow != nullptr && entry->lastRenderedWindow != WINDOW.get())
                renderedWindowRemapped = true;

            const auto WINDOW_PAN = WINDOW->m_isFloating ? 0.0 : CONTENT_PAN_X;
            const CBox BASE_BOX   = {WINDOW->m_realPosition->value() - MONITOR->m_position, WINDOW->m_realSize->value()};

            CBox       rawOverviewBox = CBox{BASE_BOX.pos() - Vector2D{WINDOW_PAN, 0.0}, BASE_BOX.size()};
            rawOverviewBox.translate(-VIEWPORT_CENTER).scale(scale->value()).translate(VIEWPORT_CENTER).translate(-viewOffset->value() * scale->value());
            rawOverviewBox.translate({0.F, yoff});

            entry->overviewBox          = rawOverviewBox;
            entry->overviewBox          = shrinkBox(entry->overviewBox, WINDOW_GAP);
            entry->liveRenderable       = windowEntryRenderable(entry);
            entry->liveVisible          = windowEntryVisible(entry, workspaceEntry);
            entry->lastRenderedWindow   = WINDOW.get();
            entry->lastGeometryPosition = WINDOW->m_realPosition->value();
            entry->lastGeometrySize     = WINDOW->m_realSize->value();

            CBox clippedHitBox = entry->overviewBox.intersection(workspaceRenderClipBox(workspaceEntry));
            clippedHitBox.noNegativeSize();
            workspaceEntry->hitBox = boxUnion(workspaceEntry->hitBox, clippedHitBox);
        }

        workspaceEntry->lastContentPan = CONTENT_PAN_X;
        yoff += WORKSPACE_STEP;
    }

    if (DIRTY_FLAGS == GEOMETRY_DIRTY_NONE || (DIRTY_FLAGS & (GEOMETRY_DIRTY_VIEWPORT | GEOMETRY_DIRTY_WORKSPACE_LIST | GEOMETRY_DIRTY_INSERTION_MARKERS)))
        buildInsertionMarkers();
    if (renderedWindowRemapped)
        invalidateWindowEntryLookups();

    geometryCacheState = {
        .monitor              = MONITOR.get(),
        .monitorPosition      = MONITOR->m_position,
        .monitorSize          = MONITOR->m_size,
        .scaleValue           = scale->value(),
        .viewOffsetValue      = viewOffset->value(),
        .activeWorkspaceIndex = activeWorkspaceEntryIndex(),
        .workspaceCount       = workspaceEntries.size(),
    };
    geometryDirtyFlags = GEOMETRY_DIRTY_NONE;
}

void CScrollOverview::ensureGeometryCache() {
    if (geometryCacheNeedsRebuild())
        rebuildGeometryCache();
}

void CScrollOverview::markGeometryCacheDirty(uint32_t flags) {
    geometryDirtyFlags |= flags;
}

bool CScrollOverview::geometryCacheNeedsRebuild() const {
    if (geometryDirtyFlags != GEOMETRY_DIRTY_NONE)
        return true;

    const auto MONITOR = pMonitor.lock();
    if (!MONITOR || !scale || !viewOffset)
        return true;

    if (geometryCacheState.monitor != MONITOR.get() || geometryValueChanged(geometryCacheState.monitorPosition, MONITOR->m_position) ||
        geometryValueChanged(geometryCacheState.monitorSize, MONITOR->m_size))
        return true;

    if (scale->isBeingAnimated() || viewOffset->isBeingAnimated())
        return true;

    if (geometryValueChanged(geometryCacheState.scaleValue, scale->value()) || geometryValueChanged(geometryCacheState.viewOffsetValue, viewOffset->value()))
        return true;

    if (geometryCacheState.workspaceCount != workspaceEntries.size() || geometryCacheState.activeWorkspaceIndex != activeWorkspaceEntryIndex())
        return true;

    for (const auto& workspaceEntry : workspaceEntries) {
        if (!workspaceEntry || !workspaceEntry->pWorkspace)
            return true;

        const auto PAN      = horizontalPanForWorkspace(workspaceEntry);
        const auto PAN_ANIM = workspaceContentPan.find(workspaceEntry->pWorkspace->m_id);
        if ((PAN_ANIM != workspaceContentPan.end() && PAN_ANIM->second && PAN_ANIM->second->isBeingAnimated()) || geometryValueChanged(workspaceEntry->lastContentPan, PAN))
            return true;

        for (const auto& entry : workspaceEntry->windowEntries) {
            if (!entry || !entry->pWindow)
                return true;

            const auto WINDOW = windowForEntry(entry);
            if (!WINDOW || !WINDOW->m_realPosition || !WINDOW->m_realSize)
                return true;

            if (entry->lastRenderedWindow != WINDOW.get())
                return true;

            if (WINDOW->m_realPosition->isBeingAnimated() || WINDOW->m_realSize->isBeingAnimated())
                return true;

            if (geometryValueChanged(entry->lastGeometryPosition, WINDOW->m_realPosition->value()) || geometryValueChanged(entry->lastGeometrySize, WINDOW->m_realSize->value()))
                return true;

            const bool RENDERABLE = windowEntryRenderable(entry);
            if (entry->liveRenderable != RENDERABLE)
                return true;

            if (entry->liveVisible != windowEntryVisible(entry, workspaceEntry))
                return true;
        }
    }

    return false;
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

SP<CScrollOverview::SWorkspaceEntry> CScrollOverview::workspaceAt(const Vector2D& local) {
    for (auto it = workspaceEntries.rbegin(); it != workspaceEntries.rend(); ++it) {
        const auto& workspaceEntry = *it;
        if (workspaceEntry && workspaceEntry->pWorkspace && workspaceEntry->hitBox.containsPoint(local))
            return workspaceEntry;
    }

    return nullptr;
}

SP<CScrollOverview::SWindowEntry> CScrollOverview::windowAtExact(const Vector2D& local) {
    for (auto wit = workspaceEntries.rbegin(); wit != workspaceEntries.rend(); ++wit) {
        const auto& workspaceEntry = *wit;
        if (!workspaceEntry || !workspaceEntry->hitBox.containsPoint(local))
            continue;

        for (auto it = workspaceEntry->windowEntries.rbegin(); it != workspaceEntry->windowEntries.rend(); ++it) {
            const auto& entry = *it;
            if (entry && entry->pWindow && entry->liveVisible && entry->overviewBox.containsPoint(local))
                return entry;
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

CBox CScrollOverview::expandedWindowHitBox(const SP<SWindowEntry>& entry) const {
    if (!entry || !entry->liveVisible || entry->overviewBox.empty())
        return {};

    CBox box = entry->overviewBox.intersection(workspaceRenderClipBox(workspaceEntryForWindowEntry(entry)));
    box.noNegativeSize();
    if (box.empty())
        return {};

    const double EXPANSION = std::max(0.0, sc<double>(g_hyprviewConfig.mouse.hitboxExpansion));
    box.x -= EXPANSION;
    box.y -= EXPANSION;
    box.w += EXPANSION * 2.0;
    box.h += EXPANSION * 2.0;
    return box;
}

SP<CScrollOverview::SWindowEntry> CScrollOverview::windowNear(const Vector2D& local) {
    if (g_hyprviewConfig.mouse.hitboxExpansion <= 0)
        return nullptr;

    SP<SWindowEntry> best;
    double           bestDistance = std::numeric_limits<double>::max();

    for (auto wit = workspaceEntries.rbegin(); wit != workspaceEntries.rend(); ++wit) {
        const auto& workspaceEntry = *wit;
        if (!workspaceEntry)
            continue;

        for (auto it = workspaceEntry->windowEntries.rbegin(); it != workspaceEntry->windowEntries.rend(); ++it) {
            const auto& entry = *it;
            if (!entry || !entry->pWindow || !entry->liveVisible)
                continue;

            const auto HITBOX = expandedWindowHitBox(entry);
            if (HITBOX.empty() || !HITBOX.containsPoint(local))
                continue;

            if (!g_hyprviewConfig.mouse.nearestHitbox)
                return entry;

            const auto DISTANCE = distanceToBox(local, entry->overviewBox);
            if (DISTANCE < bestDistance) {
                best         = entry;
                bestDistance = DISTANCE;
            }
        }
    }

    return best;
}

SP<CScrollOverview::SWindowEntry> CScrollOverview::windowAt(const Vector2D& local) {
    if (auto exact = windowAtExact(local))
        return exact;

    return windowNear(local);
}

SP<CScrollOverview::SWorkspaceEntry> CScrollOverview::workspaceEntryForWorkspace(PHLWORKSPACE w) {
    for (const auto& i : workspaceEntries) {
        if (i->pWorkspace == w)
            return i;
    }
    return nullptr;
}

SP<CScrollOverview::SWindowEntry> CScrollOverview::windowEntryForWindow(PHLWINDOW w) const {
    if (!w)
        return nullptr;

    rebuildWindowEntryLookups();

    const auto IT = rawWindowEntryLookup.find(w.get());
    return IT == rawWindowEntryLookup.end() ? nullptr : IT->second;
}
