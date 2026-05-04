#include "PluginHooks.hpp"

#include "Globals.hpp"
#include "PluginRuntime.hpp"
#include "../overview/IOverview.hpp"

#include <ctime>
#include <stdexcept>

#include <pixman.h>

#include <hyprland/src/helpers/Monitor.hpp>

namespace {
    CFunctionHook* g_pRenderWorkspaceHook = nullptr;
    CFunctionHook* g_pAddDamageHookA      = nullptr;
    CFunctionHook* g_pAddDamageHookB      = nullptr;

    using origRenderWorkspace = void (*)(void*, PHLMONITOR, PHLWORKSPACE, timespec*, const CBox&);
    using origAddDamageA      = void (*)(void*, const CBox&);
    using origAddDamageB      = void (*)(void*, const pixman_region32_t*);

    void hkRenderWorkspace(void* thisptr, PHLMONITOR pMonitor, PHLWORKSPACE pWorkspace, timespec* now, const CBox& geometry) {
        if (!g_pOverview || Hyprview::isOverviewRendering() || g_pOverview->blockOverviewRendering || g_pOverview->pMonitor != pMonitor)
            ((origRenderWorkspace)(g_pRenderWorkspaceHook->m_original))(thisptr, pMonitor, pWorkspace, now, geometry);
        else
            g_pOverview->render();
    }

    void hkAddDamageA(void* thisptr, const CBox& box) {
        const auto PMONITOR = (CMonitor*)thisptr;

        if (!g_pOverview || g_pOverview->pMonitor != PMONITOR->m_self || g_pOverview->blockDamageReporting) {
            ((origAddDamageA)g_pAddDamageHookA->m_original)(thisptr, box);
            return;
        }

        g_pOverview->onDamageReported(CRegion{box});
    }

    void hkAddDamageB(void* thisptr, const pixman_region32_t* rg) {
        const auto PMONITOR = (CMonitor*)thisptr;

        if (!g_pOverview || g_pOverview->pMonitor != PMONITOR->m_self || g_pOverview->blockDamageReporting) {
            ((origAddDamageB)g_pAddDamageHookB->m_original)(thisptr, rg);
            return;
        }

        g_pOverview->onDamageReported(CRegion{rg});
    }
}

namespace Hyprview {

    void installPluginHooks() {
        auto FNS = HyprlandAPI::findFunctionsByName(PHANDLE, "renderWorkspace");
        if (FNS.empty()) {
            failNotif("no fns for hook renderWorkspace");
            throw std::runtime_error("[he] No fns for hook renderWorkspace");
        }

        g_pRenderWorkspaceHook = HyprlandAPI::createFunctionHook(PHANDLE, FNS[0].address, (void*)hkRenderWorkspace);

        FNS = HyprlandAPI::findFunctionsByName(PHANDLE, "addDamageEPK15pixman_region32");
        if (FNS.empty()) {
            failNotif("no fns for hook addDamageEPK15pixman_region32");
            throw std::runtime_error("[he] No fns for hook addDamageEPK15pixman_region32");
        }

        g_pAddDamageHookB = HyprlandAPI::createFunctionHook(PHANDLE, FNS[0].address, (void*)hkAddDamageB);

        FNS = HyprlandAPI::findFunctionsByName(PHANDLE, "_ZN8CMonitor9addDamageERKN9Hyprutils4Math4CBoxE");
        if (FNS.empty()) {
            failNotif("no fns for hook _ZN8CMonitor9addDamageERKN9Hyprutils4Math4CBoxE");
            throw std::runtime_error("[he] No fns for hook _ZN8CMonitor9addDamageERKN9Hyprutils4Math4CBoxE");
        }

        g_pAddDamageHookA = HyprlandAPI::createFunctionHook(PHANDLE, FNS[0].address, (void*)hkAddDamageA);

        bool success = g_pRenderWorkspaceHook->hook();
        success      = success && g_pAddDamageHookA->hook();
        success      = success && g_pAddDamageHookB->hook();

        if (!success) {
            failNotif("Failed initializing hooks");
            throw std::runtime_error("[he] Failed initializing hooks");
        }
    }

}
