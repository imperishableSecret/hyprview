#include "PluginRuntime.hpp"

#include "Globals.hpp"
#include "../overview/IOverview.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/event/EventBus.hpp>

namespace {
    bool g_renderingOverview = false;
    bool g_unloading         = false;
}

namespace Hyprview {

    COverviewRenderGuard::COverviewRenderGuard() {
        g_renderingOverview = true;
    }

    COverviewRenderGuard::~COverviewRenderGuard() {
        g_renderingOverview = false;
    }

    bool isOverviewRendering() {
        return g_renderingOverview;
    }

    bool isUnloading() {
        return g_unloading;
    }

    void setUnloading(bool unloading) {
        g_unloading = unloading;
    }

    void damageOverviewMonitor() {
        if (!g_pOverview || !g_pOverview->pMonitor)
            return;

        g_pOverview->damage();
        g_pCompositor->scheduleFrameForMonitor(g_pOverview->pMonitor.lock());
    }

    void failNotif(const std::string& reason) {
        HyprlandAPI::addNotification(PHANDLE, "[hyprview] Failure in initialization: " + reason, CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
    }

    void registerPluginEventListeners() {
        static auto preRenderListener = Event::bus()->m_events.render.pre.listen([](PHLMONITOR) {
            if (!g_pOverview)
                return;
            g_pOverview->onPreRender();
        });

        static auto preReloadListener = Event::bus()->m_events.config.preReload.listen([] { resetHyprviewConfig(); });
    }

}
