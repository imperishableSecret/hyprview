#define WLR_USE_UNSTABLE

#include "Dispatchers.hpp"
#include "Globals.hpp"
#include "LuaApi.hpp"
#include "PluginHooks.hpp"
#include "PluginRuntime.hpp"

#include <stdexcept>
#include <string>

#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/render/Renderer.hpp>

// Do NOT change this function.
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

    if (HASH != CLIENT_HASH) {
        Hyprview::failNotif("Version mismatch (headers ver is not equal to running hyprland ver)");
        throw std::runtime_error("[he] Version mismatch");
    }

    if (Config::mgr()->type() != Config::CONFIG_LUA) {
        Hyprview::failNotif("hyprview requires Hyprland Lua config");
        throw std::runtime_error("[he] hyprview requires Hyprland Lua config");
    }

    Hyprview::installPluginHooks();
    Hyprview::registerPluginEventListeners();

    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprview:overview", Hyprview::onOverviewDispatcher);
    Hyprview::registerLuaApi();

    HyprlandAPI::reloadConfig();

    return {"hyprview", "A plugin for the hyprview", "Vaxry", "1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_pHyprRenderer->m_renderPass.removeAllOfType("COverviewPassElement");

    Hyprview::setUnloading(true);

    Config::mgr()->reload(); // we need to reload now to clear all the gestures
}
