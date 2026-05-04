#pragma once

#include <string>

namespace Hyprview {

    class COverviewRenderGuard {
      public:
        COverviewRenderGuard();
        ~COverviewRenderGuard();

        COverviewRenderGuard(const COverviewRenderGuard&)            = delete;
        COverviewRenderGuard& operator=(const COverviewRenderGuard&) = delete;
    };

    bool isOverviewRendering();
    bool isUnloading();
    void setUnloading(bool unloading);
    void damageOverviewMonitor();
    void failNotif(const std::string& reason);
    void registerPluginEventListeners();

}
