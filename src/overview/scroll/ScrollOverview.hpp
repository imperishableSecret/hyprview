#pragma once

#define WLR_USE_UNSTABLE

#include "../../plugin/HyprviewConfig.hpp"
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprland/src/helpers/AnimatedVariable.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/helpers/signal/Signal.hpp>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../IOverview.hpp"

class CMonitor;
struct wl_event_source;

class CScrollOverview : public IOverview {
  public:
    CScrollOverview(PHLWORKSPACE startedOn_, bool swipe = false);
    virtual ~CScrollOverview();

    virtual void render();
    virtual void damage();
    virtual void onDamageReported();
    virtual void onDamageReported(const CRegion& damage);
    virtual bool shouldHandleSurfaceDamage(SP<CWLSurfaceResource> surface);
    virtual bool shouldAllowSurfaceFrame(SP<CWLSurfaceResource> surface, const Time::steady_tp& now);
    virtual bool shouldAllowRealtimePreviewSchedule();
    virtual bool shouldSuppressRenderDamage() const;
    virtual bool shouldRenderNativeWorkspace() const;
    virtual void finishNativeWorkspaceHandoff();
    virtual void onPreRender();

    virtual void setClosing(bool closing);

    virtual void resetSwipe();
    virtual void onSwipeUpdate(double delta);
    virtual void onSwipeEnd();

    // close without a selection
    virtual void      close(bool switchToSelection = true);
    virtual void      selectHoveredWorkspace();
    virtual int64_t   selectedWorkspaceID() const;
    virtual PHLWINDOW selectedWindow() const;
    virtual bool      moveSelection(Vector2D direction);
    virtual bool      activateSelection();

    virtual void      fullRender();

  private:
    struct SWindowEntry;
    struct SWorkspaceEntry;

    struct SWorkspacePanRange {
        double min = 0.0;
        double max = 0.0;
    };

    enum class ePointerMode {
        IDLE = 0,
        PRESS_PENDING,
        WINDOW_DRAG,
        VIEW_PAN,
    };

    enum class eDropTargetType {
        NONE = 0,
        WORKSPACE_BODY,
        WORKSPACE_INSERTION,
    };

    enum class eOverviewSurfaceOwner {
        UNKNOWN = 0,
        WINDOW,
        LAYER,
        WINDOW_POPUP,
        LAYER_POPUP,
    };

    static constexpr uint32_t GEOMETRY_DIRTY_NONE              = 0;
    static constexpr uint32_t GEOMETRY_DIRTY_VIEWPORT          = 1 << 0;
    static constexpr uint32_t GEOMETRY_DIRTY_WORKSPACE_LIST    = 1 << 1;
    static constexpr uint32_t GEOMETRY_DIRTY_WINDOW_ENTRIES    = 1 << 2;
    static constexpr uint32_t GEOMETRY_DIRTY_WINDOW_GEOMETRY   = 1 << 3;
    static constexpr uint32_t GEOMETRY_DIRTY_WORKSPACE_PAN     = 1 << 4;
    static constexpr uint32_t GEOMETRY_DIRTY_INSERTION_MARKERS = 1 << 5;
    static constexpr uint32_t GEOMETRY_DIRTY_ALL = GEOMETRY_DIRTY_VIEWPORT | GEOMETRY_DIRTY_WORKSPACE_LIST | GEOMETRY_DIRTY_WINDOW_ENTRIES | GEOMETRY_DIRTY_WINDOW_GEOMETRY |
        GEOMETRY_DIRTY_WORKSPACE_PAN | GEOMETRY_DIRTY_INSERTION_MARKERS;

    struct SOverviewSurfaceOwner {
        eOverviewSurfaceOwner type = eOverviewSurfaceOwner::UNKNOWN;
        PHLWINDOW             window;
        PHLLS                 layer;
        PHLMONITOR            monitor;
    };

    struct SOverviewSurfaceOwnerCacheEntry {
        eOverviewSurfaceOwner type = eOverviewSurfaceOwner::UNKNOWN;
        PHLWINDOWREF          window;
        PHLLSREF              layer;
    };

    struct SForcedSurfaceVisibility {
        SP<CWLSurfaceResource> surface;
        CRegion                visibleRegion;
    };

    struct SForcedWindowVisibility {
        PHLWINDOWREF window;
        bool         hidden = false;
    };

    struct SDropTarget {
        eDropTargetType     type = eDropTargetType::NONE;
        SP<SWorkspaceEntry> workspace;
        CBox                markerBox;
        WORKSPACEID         targetWorkspaceID = WORKSPACE_INVALID;
        std::string         label;
    };

    struct SInsertionMarker {
        WORKSPACEID workspaceID = WORKSPACE_INVALID;
        CBox        box;
        std::string label;
    };

    struct SWindowDragState {
        PHLWINDOWREF    window;
        PHLWORKSPACEREF originalWorkspace;
        CBox            sourceBox;
        CBox            originalGlobalBox;
        Vector2D        grabOffsetLocal;
        Vector2D        originalFloatingSize;
        bool            startedTiled = false;
    };

    void                     rebuildWorkspaceEntries(PHLWORKSPACE workspace);
    void                     rebuildAllWorkspaceEntries();
    void                     refreshWorkspaceEntries(PHLWORKSPACE preferredViewport = nullptr, bool warpViewport = true);
    void                     queueRefreshWorkspaceEntries(PHLWORKSPACE preferredViewport = nullptr, bool warpViewport = true);
    void                     onWorkspaceChange();
    void                     queueViewportAnchorNormalization(PHLWORKSPACE workspace);
    void                     normalizeViewportAnchor(PHLWORKSPACE workspace);
    void                     queueFocusedWindowCursorSync(PHLWINDOW window);
    bool                     centerCursorOnWindowEntry(PHLWINDOW window);
    void                     clearFocusedWindowCursorSync();
    void                     highlightHoverDebug(bool damageOnChange = true);
    void                     clearWindowHighlights();
    void                     rememberWindowSelection(PHLWINDOW window);
    void                     pruneRememberedSelections();
    void                     keyboardTakeoverMouse();
    void                     releaseKeyboardTakeoverMouse(bool allowHoverSelection);
    void                     syncSelectionToViewport(bool damageOnChange = true);
    void                     setKeyboardSelection(SP<SWindowEntry> image, bool lockedToKeyboard, bool damageOnChange = true);
    SP<SWindowEntry>         entryForKeyboardSelection() const;
    SP<SWindowEntry>         selectableEntryForWorkspace(const SP<SWorkspaceEntry>& workspace) const;
    SP<SWorkspaceEntry>      workspaceEntryForWindow(PHLWINDOW window) const;
    SP<SWorkspaceEntry>      workspaceEntryForWindowEntry(const SP<SWindowEntry>& image) const;
    void                     ensureSelectionVisible(SP<SWindowEntry> image);
    void                     centerWindowEntryInScrollingWorkspace(SP<SWindowEntry> image, bool animate = true);
    bool                     moveHorizontalSelection(bool right);
    bool                     moveViewportWorkspace(bool up);
    bool                     setViewportWorkspace(size_t index, bool warp = false, bool activate = false);
    bool                     focusWorkspaceInViewport(SP<SWorkspaceEntry> workspace, bool warp = false, bool activate = false);
    bool                     reanchorViewportToWorkspace(PHLWORKSPACE workspace, bool preserveVisualOffset = true);
    bool                     moveViewportBy(double deltaY, bool warp = true, bool activate = false);
    bool                     setViewportOffset(Vector2D offset, bool warp = false, bool activate = false);
    void                     syncViewportWorkspaceFromOffset(const Vector2D& offset);
    void                     activateWorkspace(PHLWORKSPACE workspace, bool focus = true);
    void                     activateViewportWorkspace();
    double                   workspaceOverviewStep() const;
    double                   viewOffsetForWorkspaceIndex(size_t index) const;
    Vector2D                 clampedViewOffset(Vector2D offset) const;
    void                     rebuildGeometryCache();
    void                     ensureGeometryCache();
    void                     markGeometryCacheDirty(uint32_t flags = GEOMETRY_DIRTY_ALL);
    bool                     geometryCacheNeedsRebuild() const;
    bool                     workspaceUsesScrollingLayout(PHLWORKSPACE workspace) const;
    SWorkspacePanRange       horizontalPanRangeForWorkspace(const SP<SWorkspaceEntry>& workspace) const;
    double                   horizontalPanForWorkspace(const SP<SWorkspaceEntry>& workspace) const;
    bool                     setHorizontalPanForWorkspace(const SP<SWorkspaceEntry>& workspace, double pan, bool animate = false);
    void                     pruneWorkspaceContentPans();
    void                     handlePointerMotion(const Vector2D& local);
    void                     handlePointerPress(uint32_t button);
    void                     handlePointerRelease(uint32_t button);
    void                     handlePointerAxis(IPointer::SAxisEvent event);
    bool                     pointerOverBlockingLayerSurface(const Vector2D& local) const;
    void                     beginWindowDrag();
    void                     updateWindowDrag(const Vector2D& local);
    void                     updateViewportPan(const Vector2D& local);
    void                     updateMouseEdgeNavigation(const Vector2D& local);
    bool                     updateMouseWorkspaceEdgeNavigation(const Vector2D& local);
    void                     updateMouseSnapPanNavigation(const Vector2D& local);
    bool                     snapMousePanForWorkspace(const SP<SWorkspaceEntry>& workspace, int direction);
    void                     updateEdgeAutoscroll(uint64_t nowMs);
    void                     updateHoverActivation(uint64_t nowMs);
    void                     resetDragNavigationState();
    bool                     finishWindowDrag();
    void                     cancelPointerInteraction(bool damageOnChange = true);
    bool                     hasDropTarget() const;
    CBox                     workspaceDropBox(const SP<SWorkspaceEntry>& workspace) const;
    SP<SWindowEntry>         dropAnchorEntry(const SP<SWorkspaceEntry>& workspace, PHLWINDOW ignoredWindow, CBox* anchorBox = nullptr) const;
    Vector2D                 overviewPointToGlobal(const SP<SWorkspaceEntry>& workspace, const Vector2D& local) const;
    CBox                     floatingDropGlobalBox(PHLWINDOW window, PHLWORKSPACE targetWorkspace, const SP<SWorkspaceEntry>& targetEntry) const;
    bool                     commitFloatingWindowDrop(PHLWINDOW window, PHLWORKSPACE targetWorkspace, const SP<SWorkspaceEntry>& targetEntry);
    bool                     commitTiledWindowDrop(PHLWINDOW window, PHLWORKSPACE targetWorkspace, const SP<SWorkspaceEntry>& targetEntry);
    bool                     workspaceHasDisplayableWindows(PHLWORKSPACE workspace) const;
    bool                     workspaceVisibleInOverview(PHLWORKSPACE workspace) const;
    bool                     windowBelongsToWorkspaceInOverview(PHLWINDOW window, PHLWORKSPACE workspace) const;
    bool                     windowCanDragAcrossWorkspaces(PHLWINDOW window) const;
    bool                     workspaceIDAllowedOnMonitor(WORKSPACEID id, bool allowOutsideCurrentRange = false) const;
    std::vector<WORKSPACEID> validWorkspaceInsertionIDs(PHLWORKSPACE before, PHLWORKSPACE after) const;
    std::vector<WORKSPACEID> appendWorkspaceInsertionIDs() const;
    void                     buildInsertionMarkers();
    PHLWORKSPACE             workspaceForDropTarget(const SDropTarget& target);
    std::optional<size_t>    workspaceEntryIndex(PHLWORKSPACE workspace) const;
    std::string              workspaceAnnotationText(PHLWORKSPACE workspace) const;
    SP<Render::ITexture>     labelTexture(const std::string& text, int64_t color, int fontSize);
    Vector2D                 labelTextureSize(SP<Render::ITexture> texture) const;
    void                     renderTextureLabel(SP<Render::ITexture> texture, const CBox& box, double alpha = 1.0);
    void                     renderWorkspaceAnnotations();
    void                     renderWorkspaceShadows();
    void                     renderInsertionMarkers();
    void                     resetSurfacePolicyCache() const;
    SOverviewSurfaceOwner    uncachedOverviewSurfaceOwner(SP<CWLSurfaceResource> surface) const;
    SOverviewSurfaceOwner    overviewSurfaceOwner(SP<CWLSurfaceResource> surface) const;
    bool                     surfaceOwnerBelongsToOverviewMonitor(const SOverviewSurfaceOwner& owner, PHLMONITOR monitor) const;
    bool                     overviewWindowVisible(PHLWINDOW window) const;
    CBox                     overviewViewportBox() const;
    CBox                     expandedOverviewViewportBox(double margin = 0.0) const;
    CBox                     workspaceRenderClipBox(const SP<SWorkspaceEntry>& workspace) const;
    double                   overviewCullMargin() const;
    bool                     overviewBoxIntersectsMonitor(const CBox& box) const;
    bool                     overviewBoxIntersectsViewport(const CBox& box, double margin = 0.0) const;
    bool                     workspaceIntersectsViewport(const SP<SWorkspaceEntry>& workspace, double margin = 0.0) const;
    bool                     windowEntryIntersectsWorkspaceViewport(const SP<SWindowEntry>& image, const SP<SWorkspaceEntry>& workspace, double margin = 0.0) const;
    bool                     overviewWindowOccludedByFullscreen(PHLWINDOW window) const;
    PHLWINDOW                overviewWindowToRender(PHLWINDOW window) const;
    PHLWINDOW                windowForEntry(const SP<SWindowEntry>& image) const;
    void                     rebuildWindowEntryLookups() const;
    void                     invalidateWindowEntryLookups() const;
    SP<SWindowEntry>         renderedWindowEntryForWindow(PHLWINDOW window) const;
    bool                     windowEntryRenderable(const SP<SWindowEntry>& image) const;
    bool                     windowEntryVisible(const SP<SWindowEntry>& image, const SP<SWorkspaceEntry>& workspace = nullptr) const;
    double                   overviewStyleProgress() const;
    bool                     surfaceTreeHasFrameCallbacks(SP<CWLSurfaceResource> surface) const;
    bool                     hasVisibleRealtimePreviewCallbacks() const;
    PHLWINDOW                closeTargetWindow() const;
    bool                     sendCloseTargetFrameCallback(const Time::steady_tp& now);
    void                     surfaceTreePresent(SP<CWLSurfaceResource> surface, PHLMONITOR monitor, const Time::steady_tp& now);
    void                     sendOverviewFrameCallbacks(const Time::steady_tp& now);
    bool                     shouldAllowRealtimePreviewFrame() const;
    void                     schedulePreviewFrameAfter(std::chrono::milliseconds delay);
    void                     scheduleRealtimePreviewFrame();
    static int               realtimePreviewTimerCallback(void* data);
    void                     forceSurfaceVisibility(SP<CWLSurfaceResource> surface);
    void                     forceWindowSurfaceVisibility(PHLWINDOW window);
    void                     forceWindowVisible(PHLWINDOW window);
    void                     restoreForcedSurfaceVisibility();
    void                     restoreForcedWindowVisibility();
    void                     renderOverviewLive(const Time::steady_tp& now);
    void                     renderWorkspaceLive(const SP<SWorkspaceEntry>& workspace, const Time::steady_tp& now);
    bool                     renderWindowLive(PHLWINDOW window, const CBox& box, const Time::steady_tp& now, double alpha = 1.0, const CBox& clipBox = {});
    void                     renderDraggedWindowLive(const Time::steady_tp& now);
    void                     renderPinnedFloatingWindowsLive(const Time::steady_tp& now);
    void                     forceLayerSurfaceTreeVisibility(PHLLS layer, bool popups);
    void                     renderLiveBackdrop(const Time::steady_tp& now);
    void                     renderBackdropLayer(PHLLS layer, const Time::steady_tp& now);
    void                     renderBackdropLayerLevel(uint32_t layer, const Time::steady_tp& now);
    void                     renderWorkspaceLayer(PHLLS layer, const SP<SWorkspaceEntry>& workspace, const Time::steady_tp& now);
    void                     renderWorkspaceLayerLevel(const SP<SWorkspaceEntry>& workspace, uint32_t layer, const Time::steady_tp& now);
    void                     renderHyprlandLayerPhase(const Time::steady_tp& now);
    void                     renderFocusIndicator(SP<SWindowEntry> entry);
    void                     renderActiveWindowIndicator(SP<SWindowEntry> entry);
    bool                     windowEntryIsSelected(const SP<SWindowEntry>& entry) const;
    bool                     windowEntryIsActive(const SP<SWindowEntry>& entry) const;
    void                     renderDropTargetFeedback();
    CBox                     draggedLiveWindowBox() const;
    bool                     windowLiveRenderable(PHLWINDOW window) const;

    size_t                   activeWorkspaceEntryIndex() const;
    static void              clearCurrentRenderTarget(const CHyprColor& color);
    static CBox              shrinkBox(CBox box, double amount);
    static CBox              boxUnion(const CBox& a, const CBox& b);

    size_t                   viewportCurrentWorkspace = 0;

    struct SWindowEntry {
        PHLWINDOWREF            pWindow;
        CBox                    overviewBox;
        bool                    liveRenderable     = false;
        bool                    liveVisible        = false;
        bool                    highlight          = false;
        const void*             lastRenderedWindow = nullptr;
        Vector2D                lastGeometryPosition;
        Vector2D                lastGeometrySize;
        UP<CHyprSignalListener> windowCommit;
    };

    struct SWorkspaceEntry {
        PHLWORKSPACE                  pWorkspace;
        CBox                          box;
        CBox                          overviewBox;
        CBox                          hitBox;
        double                        lastContentPan = 0.0;
        std::vector<SP<SWindowEntry>> windowEntries;
    };

    struct SGeometryCacheSnapshot {
        const void* monitor = nullptr;
        Vector2D    monitorPosition;
        Vector2D    monitorSize;
        float       scaleValue = 0.F;
        Vector2D    viewOffsetValue;
        size_t      activeWorkspaceIndex = 0;
        size_t      workspaceCount       = 0;
    };

    using SSurfaceOwnerCache         = std::unordered_map<const void*, SOverviewSurfaceOwnerCacheEntry>;
    using SSurfaceFrameCallbackCache = std::unordered_map<const void*, bool>;

    struct SInputState {
        ePointerMode                    mode = ePointerMode::IDLE;
        Vector2D                        pressPosLocal;
        Vector2D                        lastPosLocal;
        SP<SWorkspaceEntry>             pannedWorkspace;
        double                          contentPanOnPress = 0.0;
        std::optional<SWindowDragState> windowDrag;
        SDropTarget                     dropTarget;
        uint32_t                        pressedButton = 0;
    };

    SDropTarget                                                  dropTargetAt(const Vector2D& local);
    bool                                                         moveDraggedWindowToDropTarget();
    void                                                         raiseFloatingWindow(PHLWINDOW window);

    Vector2D                                                     lastMousePosLocal = Vector2D{};
    SInputState                                                  inputState;
    double                                                       wheelWorkspaceScrollAccum = 0.0;
    uint64_t                                                     lastEdgeScrollMs          = 0;
    uint64_t                                                     hoverActivateStartedMs    = 0;
    SP<SWorkspaceEntry>                                          hoverActivateWorkspace;
    std::vector<SInsertionMarker>                                insertionMarkers;
    std::unordered_map<std::string, SP<Render::ITexture>>        labelTextureCache;
    std::unordered_map<WORKSPACEID, PHLANIMVAR<float>>           workspaceContentPan;
    std::unordered_map<WORKSPACEID, PHLWINDOWREF>                rememberedSelection;
    mutable std::unordered_map<const void*, SP<SWindowEntry>>    rawWindowEntryLookup;
    mutable std::unordered_map<const void*, SP<SWindowEntry>>    renderedWindowEntryLookup;
    mutable std::unordered_map<const void*, SP<SWorkspaceEntry>> windowEntryWorkspaceLookup;

    mutable SSurfaceOwnerCache                                   surfaceOwnerCache;
    mutable SSurfaceFrameCallbackCache                           surfaceFrameCallbackCache;

    std::vector<SForcedSurfaceVisibility>                        forcedSurfaceVisibility;
    std::vector<SForcedWindowVisibility>                         forcedWindowVisibility;
    int                                                          mouseEdgeNavigationDirection  = 0;
    int                                                          mouseSnapPanDirection         = 0;
    bool                                                         mouseSnapPanBlockedUntilExit  = false;
    bool                                                         realtimePreviewTimerArmed     = false;
    bool                                                         realtimePreviewFrameQueued    = false;
    bool                                                         sendingOverviewFrameCallbacks = false;
    mutable bool                                                 windowEntryLookupsDirty       = true;
    uint32_t                                                     geometryDirtyFlags            = GEOMETRY_DIRTY_ALL;
    SGeometryCacheSnapshot                                       geometryCacheSnapshot;
    uint64_t                                                     unknownSurfaceDamageDecisions = 0;
    uint64_t                                                     unknownSurfaceFrameDecisions  = 0;

    PHLWINDOWREF                                                 closeOnWindow;
    PHLWINDOWREF                                                 closeFrameWindow;
    PHLWORKSPACEREF                                              closeOnWorkspace;
    PHLWINDOWREF                                                 keyboardSelectedWindow;
    PHLWINDOWREF                                                 hoveredWindow;
    PHLWORKSPACEREF                                              hoveredWorkspace;
    PHLWORKSPACEREF                                              pendingAnchorWorkspace;
    PHLWORKSPACEREF                                              mouseSnapPanWorkspace;

    std::vector<SP<SWorkspaceEntry>>                             workspaceEntries;
    SP<SWorkspaceEntry>                                          workspaceEntryForWorkspace(PHLWORKSPACE w);
    SP<SWindowEntry>                                             windowEntryForWindow(PHLWINDOW w) const;
    SP<SWorkspaceEntry>                                          workspaceAt(const Vector2D& local);
    SP<SWindowEntry>                                             windowAt(const Vector2D& local);
    SP<SWindowEntry>                                             windowAtExact(const Vector2D& local);
    SP<SWindowEntry>                                             windowNear(const Vector2D& local);
    CBox                                                         expandedWindowHitBox(const SP<SWindowEntry>& image) const;
    static double                                                distanceToBox(const Vector2D& point, const CBox& box);

    PHLWORKSPACE                                                 startedOn;

    PHLANIMVAR<float>                                            scale;
    PHLANIMVAR<Vector2D>                                         viewOffset;
    Time::steady_tp                                              lastRealtimePreviewFrame = {};
    Time::steady_tp                                              realtimePreviewTimerDue  = {};
    wl_event_source*                                             realtimePreviewTimer     = nullptr;

    bool                                                         refreshQueued = false;
    PHLWORKSPACEREF                                              queuedRefreshWorkspace;
    bool                                                         queuedRefreshWarp = true;
    bool                                                         cursorSyncQueued  = false;
    PHLWINDOWREF                                                 queuedCursorWindow;
    uint64_t                                                     cursorSyncUntilMs          = 0;
    bool                                                         cursorSyncWarping          = false;
    bool                                                         refreshingWorkspaceEntries = false;
    bool                                                         closing                    = false;
    bool                                                         keyboardSelectionLocked    = false;
    Vector2D                                                     keyboardTakeoverMousePos;

    CHyprSignalListener                                          mouseMoveHook;
    CHyprSignalListener                                          mouseButtonHook;
    CHyprSignalListener                                          touchMoveHook;
    CHyprSignalListener                                          touchDownHook;
    CHyprSignalListener                                          mouseAxisHook;
    CHyprSignalListener                                          windowOpenHook;
    CHyprSignalListener                                          windowCloseHook;
    CHyprSignalListener                                          windowMoveHook;
    CHyprSignalListener                                          windowActiveHook;
    CHyprSignalListener                                          workspaceActiveHook;

    bool                                                         swipe             = false;
    bool                                                         swipeWasCommenced = false;

    friend class CScrollOverviewPassElement;
};
