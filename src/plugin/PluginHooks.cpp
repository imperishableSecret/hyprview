#include "PluginHooks.hpp"

#include "Globals.hpp"
#include "PluginRuntime.hpp"
#include "../overview/IOverview.hpp"

#include <ctime>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <pixman.h>

#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <wlr-layer-shell-unstable-v1.hpp>

namespace {
    CFunctionHook* g_pRenderWorkspaceHook = nullptr;
    CFunctionHook* g_pAddDamageHookA      = nullptr;
    CFunctionHook* g_pAddDamageHookB      = nullptr;
    CFunctionHook* g_pDamageSurfaceHook   = nullptr;
    CFunctionHook* g_pScheduleFrameHook   = nullptr;
    CFunctionHook* g_pSendFrameEventsHook = nullptr;
    CFunctionHook* g_pSurfaceFrameHook    = nullptr;

    using origRenderWorkspace            = void (*)(void*, PHLMONITOR, PHLWORKSPACE, const Time::steady_tp&, const CBox&);
    using origAddDamageA                 = void (*)(void*, const CBox&);
    using origAddDamageB                 = void (*)(void*, const pixman_region32_t*);
    using origDamageSurface              = void (*)(void*, SP<CWLSurfaceResource>, double, double, double);
    using origScheduleFrameForMonitor    = void (*)(void*, PHLMONITOR, Aquamarine::IOutput::scheduleFrameReason);
    using origSendFrameEventsToWorkspace = void (*)(void*, PHLMONITOR, PHLWORKSPACE, const Time::steady_tp&);
    using origSurfaceFrame               = void (*)(void*, const Time::steady_tp&);

    bool renderingOverview = false;
    bool damageFromSurface = false;

    bool demangledMatches(const SFunctionMatch& match, std::initializer_list<std::string_view> needles) {
        for (const auto& needle : needles) {
            if (!needle.empty() && match.demangled.find(needle) == std::string::npos)
                return false;
        }

        return true;
    }

    std::string needleList(std::initializer_list<std::string_view> needles) {
        std::string result;

        for (const auto& needle : needles) {
            if (needle.empty())
                continue;

            if (!result.empty())
                result += ", ";

            result += "'";
            result += needle;
            result += "'";
        }

        return result.empty() ? "<none>" : result;
    }

    bool monitorHasRealtimeLayer(PHLMONITOR monitor) {
        if (!monitor)
            return false;

        for (const auto LAYER_LEVEL : {ZWLR_LAYER_SHELL_V1_LAYER_TOP, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY}) {
            for (const auto& layerRef : monitor->m_layerSurfaceLayers[LAYER_LEVEL]) {
                const auto LAYER = layerRef.lock();
                if (!LAYER)
                    continue;

                if (Desktop::View::validMapped(LAYER))
                    return true;
            }
        }

        return false;
    }

    void logHookCandidates(std::string_view hookName, const std::vector<SFunctionMatch>& matches) {
        for (const auto& match : matches)
            Log::logger->log(Log::ERR, "[hyprview] hook {} candidate at {} signature: {} demangled: {}", hookName, match.address, match.signature, match.demangled);
    }

    SFunctionMatch findHookTarget(std::string_view hookName, std::string_view lookupName, std::initializer_list<std::string_view> demangledNeedles) {
        const auto MATCHES = HyprlandAPI::findFunctionsByName(PHANDLE, std::string{lookupName});
        if (MATCHES.empty()) {
            const std::string REASON = "no functions found for hook " + std::string{hookName} + " using lookup " + std::string{lookupName};
            Hyprview::failNotif(REASON);
            throw std::runtime_error("[hyprview] " + REASON);
        }

        std::vector<SFunctionMatch> filtered;
        for (const auto& match : MATCHES) {
            if (demangledMatches(match, demangledNeedles))
                filtered.push_back(match);
        }

        if (filtered.size() != 1) {
            logHookCandidates(hookName, MATCHES);

            const std::string REASON =
                "hook " + std::string{hookName} + " matched " + std::to_string(filtered.size()) + " candidates with demangled needles " + needleList(demangledNeedles);
            Hyprview::failNotif(REASON);
            throw std::runtime_error("[hyprview] " + REASON);
        }

        Log::logger->log(Log::INFO, "[hyprview] hook {} matched {}", hookName, filtered[0].demangled);
        return filtered[0];
    }

    CFunctionHook* createCheckedHook(std::string_view hookName, std::string_view lookupName, std::initializer_list<std::string_view> demangledNeedles, void* hookFn) {
        const auto  TARGET = findHookTarget(hookName, lookupName, demangledNeedles);
        auto* const HOOK   = HyprlandAPI::createFunctionHook(PHANDLE, TARGET.address, hookFn);

        if (!HOOK) {
            const std::string REASON = "failed creating hook " + std::string{hookName};
            Hyprview::failNotif(REASON);
            throw std::runtime_error("[hyprview] " + REASON);
        }

        return HOOK;
    }

    void enableCheckedHook(std::string_view hookName, CFunctionHook* hook) {
        if (hook && hook->hook())
            return;

        const std::string REASON = "failed enabling hook " + std::string{hookName};
        Hyprview::failNotif(REASON);
        throw std::runtime_error("[hyprview] " + REASON);
    }

    void hkRenderWorkspace(void* thisptr, PHLMONITOR pMonitor, PHLWORKSPACE pWorkspace, const Time::steady_tp& now, const CBox& geometry) {
        if (!g_pOverview || renderingOverview || Hyprview::isOverviewRendering() || g_pOverview->blockOverviewRendering || g_pOverview->pMonitor != pMonitor)
            ((origRenderWorkspace)(g_pRenderWorkspaceHook->m_original))(thisptr, pMonitor, pWorkspace, now, geometry);
        else {
            const bool PREVIOUS = renderingOverview;
            renderingOverview   = true;
            g_pOverview->render();
            renderingOverview = PREVIOUS;
        }
    }

    void hkScheduleFrameForMonitor(void* thisptr, PHLMONITOR monitor, Aquamarine::IOutput::scheduleFrameReason reason) {
        if (g_pOverview && g_pOverview->pMonitor == monitor) {
            using enum Aquamarine::IOutput::scheduleFrameReason;

            const bool THROTTLED_REASON = reason == AQ_SCHEDULE_UNKNOWN || reason == AQ_SCHEDULE_CLIENT_UNKNOWN || reason == AQ_SCHEDULE_NEEDS_FRAME ||
                reason == AQ_SCHEDULE_RENDER_MONITOR || reason == AQ_SCHEDULE_DAMAGE;

            const bool HAS_REALTIME_LAYER = monitorHasRealtimeLayer(monitor);

            if (THROTTLED_REASON && !damageFromSurface && !HAS_REALTIME_LAYER && !g_pOverview->blockDamageReporting && !g_pOverview->shouldAllowRealtimePreviewSchedule())
                return;
        }

        ((origScheduleFrameForMonitor)g_pScheduleFrameHook->m_original)(thisptr, monitor, reason);
    }

    void hkDamageSurface(void* thisptr, SP<CWLSurfaceResource> surface, double x, double y, double scale) {
        const bool ALLOW = !g_pOverview || g_pOverview->blockDamageReporting || g_pOverview->shouldHandleSurfaceDamage(surface);

        if (ALLOW) {
            const bool PREVIOUS = damageFromSurface;
            damageFromSurface   = !!g_pOverview;
            ((origDamageSurface)g_pDamageSurfaceHook->m_original)(thisptr, surface, x, y, scale);
            damageFromSurface = PREVIOUS;
        }
    }

    void hkSendFrameEventsToWorkspace(void* thisptr, PHLMONITOR monitor, PHLWORKSPACE workspace, const Time::steady_tp& now) {
        if (g_pOverview && g_pOverview->pMonitor == monitor)
            return;

        ((origSendFrameEventsToWorkspace)g_pSendFrameEventsHook->m_original)(thisptr, monitor, workspace, now);
    }

    void hkSurfaceFrame(void* thisptr, const Time::steady_tp& now) {
        const auto SURFACE = static_cast<CWLSurfaceResource*>(thisptr)->m_self.lock();
        const bool ALLOW   = !g_pOverview || g_pOverview->shouldAllowSurfaceFrame(SURFACE, now);

        if (!ALLOW)
            return;

        ((origSurfaceFrame)g_pSurfaceFrameHook->m_original)(thisptr, now);
    }

    void hkAddDamageA(void* thisptr, const CBox& box) {
        const auto PMONITOR = (CMonitor*)thisptr;

        if (g_pOverview && g_pOverview->pMonitor == PMONITOR->m_self && renderingOverview && !damageFromSurface && g_pOverview->shouldSuppressRenderDamage())
            return;

        if (!g_pOverview || g_pOverview->pMonitor != PMONITOR->m_self || g_pOverview->blockDamageReporting || damageFromSurface) {
            ((origAddDamageA)g_pAddDamageHookA->m_original)(thisptr, box);
            return;
        }

        g_pOverview->onDamageReported(CRegion{box});
        ((origAddDamageA)g_pAddDamageHookA->m_original)(thisptr, box);
    }

    void hkAddDamageB(void* thisptr, const pixman_region32_t* rg) {
        const auto PMONITOR = (CMonitor*)thisptr;

        if (g_pOverview && g_pOverview->pMonitor == PMONITOR->m_self && renderingOverview && !damageFromSurface && g_pOverview->shouldSuppressRenderDamage())
            return;

        if (!g_pOverview || g_pOverview->pMonitor != PMONITOR->m_self || g_pOverview->blockDamageReporting || damageFromSurface) {
            ((origAddDamageB)g_pAddDamageHookB->m_original)(thisptr, rg);
            return;
        }

        g_pOverview->onDamageReported(CRegion{rg});
        ((origAddDamageB)g_pAddDamageHookB->m_original)(thisptr, rg);
    }
}

namespace Hyprview {

    void installPluginHooks() {
        g_pRenderWorkspaceHook =
            createCheckedHook("renderWorkspace", "renderWorkspace", {"Render::IHyprRenderer::renderWorkspace(", "Hyprutils::Math::CBox const&"}, (void*)hkRenderWorkspace);
        g_pScheduleFrameHook = createCheckedHook("scheduleFrameForMonitor", "scheduleFrameForMonitor", {"CCompositor::scheduleFrameForMonitor("}, (void*)hkScheduleFrameForMonitor);
        g_pDamageSurfaceHook = createCheckedHook("damageSurface", "damageSurface", {"Render::IHyprRenderer::damageSurface("}, (void*)hkDamageSurface);
        g_pSendFrameEventsHook = createCheckedHook("sendFrameEventsToWorkspace", "sendFrameEventsToWorkspace", {"Render::IHyprRenderer::sendFrameEventsToWorkspace("},
                                                   (void*)hkSendFrameEventsToWorkspace);
        g_pSurfaceFrameHook    = createCheckedHook("CWLSurfaceResource::frame", "frame", {"CWLSurfaceResource::frame("}, (void*)hkSurfaceFrame);
        g_pAddDamageHookA      = createCheckedHook("addDamage(CBox)", "addDamage", {"CMonitor::addDamage(Hyprutils::Math::CBox const&"}, (void*)hkAddDamageA);
        g_pAddDamageHookB      = createCheckedHook("addDamage(pixman_region32)", "addDamage", {"CMonitor::addDamage(pixman_region32 const*)"}, (void*)hkAddDamageB);

        enableCheckedHook("renderWorkspace", g_pRenderWorkspaceHook);
        enableCheckedHook("scheduleFrameForMonitor", g_pScheduleFrameHook);
        enableCheckedHook("damageSurface", g_pDamageSurfaceHook);
        enableCheckedHook("sendFrameEventsToWorkspace", g_pSendFrameEventsHook);
        enableCheckedHook("CWLSurfaceResource::frame", g_pSurfaceFrameHook);
        enableCheckedHook("addDamage(CBox)", g_pAddDamageHookA);
        enableCheckedHook("addDamage(pixman_region32)", g_pAddDamageHookB);
    }

}
