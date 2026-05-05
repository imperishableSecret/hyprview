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
    struct SWindowImage;
    struct SWorkspaceImage;

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

    struct SOverviewSurfaceOwner {
        eOverviewSurfaceOwner type = eOverviewSurfaceOwner::UNKNOWN;
        PHLWINDOW             window;
        PHLLS                 layer;
        PHLMONITOR            monitor;
    };

    struct SDropTarget {
        eDropTargetType     type = eDropTargetType::NONE;
        SP<SWorkspaceImage> workspace;
        CBox                markerBox;
        WORKSPACEID         targetWorkspaceID = WORKSPACE_INVALID;
        std::string         label;
    };

    struct SInsertionMarker {
        WORKSPACEID workspaceID = WORKSPACE_INVALID;
        CBox        box;
        std::string label;
    };

    void                     redrawWorkspace(PHLWORKSPACE w, bool forcelowres = false);
    void                     redrawAll(bool forcelowres = false);
    void                     refreshWorkspaceImages(PHLWORKSPACE preferredViewport = nullptr, bool warpViewport = true);
    void                     queueRefreshWorkspaceImages(PHLWORKSPACE preferredViewport = nullptr, bool warpViewport = true);
    void                     onWorkspaceChange();
    void                     queueViewportAnchorNormalization(PHLWORKSPACE workspace);
    void                     normalizeViewportAnchor(PHLWORKSPACE workspace);
    void                     queueFocusedWindowCursorSync(PHLWINDOW window);
    bool                     centerCursorOnWindowImage(PHLWINDOW window);
    void                     clearFocusedWindowCursorSync();
    void                     highlightHoverDebug(bool damageOnChange = true);
    void                     clearWindowHighlights();
    void                     rememberWindowSelection(PHLWINDOW window);
    void                     pruneRememberedSelections();
    void                     keyboardTakeoverMouse();
    void                     releaseKeyboardTakeoverMouse(bool allowHoverSelection);
    void                     syncSelectionToViewport(bool damageOnChange = true);
    void                     setKeyboardSelection(SP<SWindowImage> image, bool lockedToKeyboard, bool damageOnChange = true);
    SP<SWindowImage>         imageForKeyboardSelection() const;
    SP<SWindowImage>         selectableImageForWorkspace(const SP<SWorkspaceImage>& workspace) const;
    SP<SWorkspaceImage>      workspaceImageForWindow(PHLWINDOW window) const;
    SP<SWorkspaceImage>      workspaceImageForWindowImage(const SP<SWindowImage>& image) const;
    void                     ensureSelectionVisible(SP<SWindowImage> image);
    void                     centerWindowImageInScrollingWorkspace(SP<SWindowImage> image, bool animate = true);
    bool                     moveHorizontalSelection(bool right);
    bool                     moveViewportWorkspace(bool up);
    bool                     setViewportWorkspace(size_t index, bool warp = false, bool activate = false);
    bool                     focusWorkspaceInViewport(SP<SWorkspaceImage> workspace, bool warp = false, bool activate = false);
    bool                     moveViewportBy(double deltaY, bool warp = true, bool activate = false);
    bool                     setViewportOffset(Vector2D offset, bool warp = false, bool activate = false);
    void                     syncViewportWorkspaceFromOffset(const Vector2D& offset);
    void                     activateWorkspace(PHLWORKSPACE workspace, bool focus = true);
    void                     activateViewportWorkspace();
    double                   viewOffsetForWorkspaceIndex(size_t index) const;
    Vector2D                 clampedViewOffset(Vector2D offset) const;
    void                     rebuildGeometryCache();
    bool                     workspaceUsesScrollingLayout(PHLWORKSPACE workspace) const;
    SWorkspacePanRange       horizontalPanRangeForWorkspace(const SP<SWorkspaceImage>& workspace) const;
    double                   horizontalPanForWorkspace(const SP<SWorkspaceImage>& workspace) const;
    bool                     setHorizontalPanForWorkspace(const SP<SWorkspaceImage>& workspace, double pan, bool animate = false);
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
    bool                     snapMousePanForWorkspace(const SP<SWorkspaceImage>& workspace, int direction);
    void                     updateEdgeAutoscroll(uint64_t nowMs);
    void                     updateHoverActivation(uint64_t nowMs);
    void                     resetDragNavigationState();
    bool                     finishWindowDrag();
    void                     cancelPointerInteraction(bool damageOnChange = true);
    bool                     hasDropTarget() const;
    bool                     workspaceHasDisplayableWindows(PHLWORKSPACE workspace) const;
    bool                     workspaceVisibleInOverview(PHLWORKSPACE workspace) const;
    bool                     windowBelongsToWorkspaceInOverview(PHLWINDOW window, PHLWORKSPACE workspace) const;
    bool                     windowCanDragAcrossWorkspaces(PHLWINDOW window) const;
    bool                     workspaceIDAllowedOnMonitor(WORKSPACEID id, bool allowOutsideCurrentRange = false) const;
    std::vector<WORKSPACEID> validWorkspaceInsertionIDs(PHLWORKSPACE before, PHLWORKSPACE after) const;
    std::vector<WORKSPACEID> appendWorkspaceInsertionIDs() const;
    void                     buildInsertionMarkers();
    PHLWORKSPACE             workspaceForDropTarget(const SDropTarget& target);
    std::optional<size_t>    workspaceImageIndex(PHLWORKSPACE workspace) const;
    std::string              workspaceAnnotationText(PHLWORKSPACE workspace) const;
    SP<Render::ITexture>     labelTexture(const std::string& text, int64_t color, int fontSize);
    Vector2D                 labelTextureSize(SP<Render::ITexture> texture) const;
    void                     renderTextureLabel(SP<Render::ITexture> texture, const CBox& box, double alpha = 1.0);
    void                     renderWorkspaceAnnotations();
    void                     renderWorkspaceShadows();
    void                     renderInsertionMarkers();
    bool                     redrawDirtyWindowImages();
    bool                     markDirtyWindowImagesForDamage(const CRegion& damage);
    SOverviewSurfaceOwner    overviewSurfaceOwner(SP<CWLSurfaceResource> surface) const;
    bool                     surfaceOwnerBelongsToOverviewMonitor(const SOverviewSurfaceOwner& owner, PHLMONITOR monitor) const;
    bool                     overviewWindowVisible(PHLWINDOW window) const;
    bool                     surfaceTreeHasFrameCallbacks(SP<CWLSurfaceResource> surface) const;
    void                     surfaceTreePresent(SP<CWLSurfaceResource> surface, PHLMONITOR monitor, const Time::steady_tp& now);
    void                     sendOverviewFrameCallbacks(const Time::steady_tp& now);
    bool                     shouldAllowRealtimePreviewFrame() const;
    void                     schedulePreviewFrameAfter(std::chrono::milliseconds delay);
    void                     scheduleMinimumPreviewFrame();
    void                     scheduleRealtimePreviewFrame();
    static int               realtimePreviewTimerCallback(void* data);
    void                     renderWindowImage(SP<SWindowImage> img, const CBox& box, double alpha = 1.0);
    void                     renderFocusIndicator(SP<SWindowImage> img);
    void                     renderActiveWindowIndicator(SP<SWindowImage> img);
    bool                     windowImageIsSelected(const SP<SWindowImage>& img) const;
    bool                     windowImageIsActive(const SP<SWindowImage>& img) const;
    void                     renderDropTargetFeedback();
    void                     renderDragPreview();
    CBox                     draggedWindowBox() const;
    bool                     windowImageRenderable(PHLWINDOW window) const;

    size_t                   activeWorkspaceImageIndex() const;
    static void              clearCurrentRenderTarget(const CHyprColor& color);
    static CBox              shrinkBox(CBox box, double amount);
    static CBox              boxUnion(const CBox& a, const CBox& b);

    bool                     damageDirty              = false;
    size_t                   viewportCurrentWorkspace = 0;

    struct SWindowImage {
        PHLWINDOWREF             pWindow;
        SP<Render::IFramebuffer> fb;
        CBox                     overviewBox;
        bool                     highlight = false;
        bool                     dirty     = false;
        UP<CHyprSignalListener>  windowCommit;
        Vector2D                 lastWindowPosition, lastWindowSize;
    };

    void redrawWindowImage(SP<SWindowImage>);

    struct SWorkspaceImage {
        PHLWORKSPACE                  pWorkspace;
        CBox                          box;
        CBox                          overviewBox;
        CBox                          hitBox;
        std::vector<SP<SWindowImage>> windowImages;
    };

    struct SInputState {
        ePointerMode        mode = ePointerMode::IDLE;
        Vector2D            pressPosLocal;
        Vector2D            lastPosLocal;
        Vector2D            dragOffsetLocal;
        SP<SWorkspaceImage> pannedWorkspace;
        double              contentPanOnPress = 0.0;
        PHLWINDOWREF        draggedWindow;
        SP<SWindowImage>    draggedImage;
        SDropTarget         dropTarget;
        uint32_t            pressedButton = 0;
    };

    SDropTarget                                           dropTargetAt(const Vector2D& local);
    bool                                                  moveDraggedWindowToDropTarget();
    void                                                  raiseFloatingWindow(PHLWINDOW window);

    SP<Render::IFramebuffer>                              backgroundFb;
    Vector2D                                              lastMousePosLocal = Vector2D{};
    SInputState                                           inputState;
    double                                                wheelWorkspaceScrollAccum = 0.0;
    uint64_t                                              lastEdgeScrollMs          = 0;
    uint64_t                                              hoverActivateStartedMs    = 0;
    SP<SWorkspaceImage>                                   hoverActivateWorkspace;
    std::vector<SInsertionMarker>                         insertionMarkers;
    std::unordered_map<std::string, SP<Render::ITexture>> labelTextureCache;
    std::unordered_map<WORKSPACEID, PHLANIMVAR<float>>    workspaceContentPan;
    std::unordered_map<WORKSPACEID, PHLWINDOWREF>         rememberedSelection;
    int                                                   mouseEdgeNavigationDirection  = 0;
    int                                                   mouseSnapPanDirection         = 0;
    bool                                                  mouseSnapPanBlockedUntilExit  = false;
    bool                                                  realtimePreviewTimerArmed     = false;
    bool                                                  realtimePreviewFrameQueued    = false;
    bool                                                  sendingOverviewFrameCallbacks = false;

    PHLWINDOWREF                                          closeOnWindow;
    PHLWORKSPACEREF                                       closeOnWorkspace;
    PHLWINDOWREF                                          keyboardSelectedWindow;
    PHLWINDOWREF                                          hoveredWindow;
    PHLWORKSPACEREF                                       hoveredWorkspace;
    PHLWORKSPACEREF                                       pendingAnchorWorkspace;
    PHLWORKSPACEREF                                       mouseSnapPanWorkspace;
    PHLWORKSPACEREF                                       restoredSelectionWorkspace;

    std::vector<SP<SWorkspaceImage>>                      images;
    SP<SWorkspaceImage>                                   imageForWorkspace(PHLWORKSPACE w);
    SP<SWindowImage>                                      imageForWindow(PHLWINDOW w) const;
    SP<SWorkspaceImage>                                   workspaceAt(const Vector2D& local);
    SP<SWindowImage>                                      windowAt(const Vector2D& local);
    SP<SWindowImage>                                      windowAtExact(const Vector2D& local);
    SP<SWindowImage>                                      windowNear(const Vector2D& local);
    CBox                                                  expandedWindowHitBox(const SP<SWindowImage>& image) const;
    static double                                         distanceToBox(const Vector2D& point, const CBox& box);

    PHLWORKSPACE                                          startedOn;

    PHLANIMVAR<float>                                     scale;
    PHLANIMVAR<Vector2D>                                  viewOffset;
    Time::steady_tp                                       lastRealtimePreviewFrame = {};
    Time::steady_tp                                       realtimePreviewTimerDue  = {};
    wl_event_source*                                      realtimePreviewTimer     = nullptr;

    bool                                                  refreshQueued = false;
    PHLWORKSPACEREF                                       queuedRefreshWorkspace;
    bool                                                  queuedRefreshWarp = true;
    bool                                                  cursorSyncQueued  = false;
    PHLWINDOWREF                                          queuedCursorWindow;
    uint64_t                                              cursorSyncUntilMs         = 0;
    bool                                                  cursorSyncWarping         = false;
    bool                                                  refreshingWorkspaceImages = false;
    bool                                                  closing                   = false;
    bool                                                  keyboardSelectionLocked   = false;
    Vector2D                                              keyboardTakeoverMousePos;

    CHyprSignalListener                                   mouseMoveHook;
    CHyprSignalListener                                   mouseButtonHook;
    CHyprSignalListener                                   touchMoveHook;
    CHyprSignalListener                                   touchDownHook;
    CHyprSignalListener                                   mouseAxisHook;
    CHyprSignalListener                                   windowOpenHook;
    CHyprSignalListener                                   windowCloseHook;
    CHyprSignalListener                                   windowMoveHook;
    CHyprSignalListener                                   windowActiveHook;
    CHyprSignalListener                                   workspaceActiveHook;

    bool                                                  swipe             = false;
    bool                                                  swipeWasCommenced = false;

    friend class CScrollOverviewPassElement;
};
