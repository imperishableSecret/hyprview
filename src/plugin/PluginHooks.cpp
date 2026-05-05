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
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/render/Renderer.hpp>

namespace {
    CFunctionHook* g_pRenderWorkspaceHook = nullptr;
    CFunctionHook* g_pAddDamageHookA      = nullptr;
    CFunctionHook* g_pAddDamageHookB      = nullptr;

    using origRenderWorkspace = void (*)(void*, PHLMONITOR, PHLWORKSPACE, const Time::steady_tp&, const CBox&);
    using origAddDamageA      = void (*)(void*, const CBox&);
    using origAddDamageB      = void (*)(void*, const pixman_region32_t*);

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
        g_pRenderWorkspaceHook =
            createCheckedHook("renderWorkspace", "renderWorkspace", {"Render::IHyprRenderer::renderWorkspace(", "Hyprutils::Math::CBox const&"}, (void*)hkRenderWorkspace);
        g_pAddDamageHookA = createCheckedHook("addDamage(CBox)", "addDamage", {"CMonitor::addDamage(Hyprutils::Math::CBox const&"}, (void*)hkAddDamageA);
        g_pAddDamageHookB = createCheckedHook("addDamage(pixman_region32)", "addDamage", {"CMonitor::addDamage(pixman_region32 const*)"}, (void*)hkAddDamageB);

        enableCheckedHook("renderWorkspace", g_pRenderWorkspaceHook);
        enableCheckedHook("addDamage(CBox)", g_pAddDamageHookA);
        enableCheckedHook("addDamage(pixman_region32)", g_pAddDamageHookB);
    }

}
