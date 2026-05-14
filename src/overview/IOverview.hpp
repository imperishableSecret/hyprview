#pragma once

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprutils/math/Region.hpp>
#include <hyprutils/math/Vector2D.hpp>

class CWLSurfaceResource;

class IOverview {
  public:
    IOverview()          = default;
    virtual ~IOverview() = default;

    virtual void render()           = 0;
    virtual void damage()           = 0;
    virtual void onDamageReported() = 0;
    virtual void onDamageReported(const Hyprutils::Math::CRegion& damage) {
        onDamageReported();
    }
    virtual bool shouldHandleSurfaceDamage(SP<CWLSurfaceResource> surface) {
        return true;
    }
    virtual bool shouldAllowSurfaceFrame(SP<CWLSurfaceResource> surface, const Time::steady_tp& now) {
        return true;
    }
    virtual bool shouldAllowRealtimePreviewSchedule() {
        return true;
    }
    virtual bool shouldSuppressRenderDamage() const {
        return false;
    }
    virtual void      onPreRender() = 0;

    virtual void      setClosing(bool closing) = 0;

    virtual void      resetSwipe()                = 0;
    virtual void      onSwipeUpdate(double delta) = 0;
    virtual void      onSwipeEnd()                = 0;

    virtual void      close(bool switchToSelection = true) = 0;
    virtual void      selectHoveredWorkspace()             = 0;
    virtual int64_t   selectedWorkspaceID() const          = 0;
    virtual PHLWINDOW selectedWindow() const {
        return nullptr;
    }
    virtual bool moveSelection(Vector2D direction) {
        return false;
    }
    virtual bool activateSelection() {
        return false;
    }

    virtual void  fullRender() = 0;

    bool          blockOverviewRendering = false;
    bool          blockDamageReporting   = false;

    PHLMONITORREF pMonitor;
    bool          m_isSwiping = false;
};

inline SP<IOverview> g_pOverview;
