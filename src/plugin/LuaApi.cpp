#include "LuaApi.hpp"

#include "Dispatchers.hpp"
#include "Globals.hpp"
#include "LuaConfig.hpp"
#include "../gestures/GestureRegistration.hpp"
#include "../overview/IOverview.hpp"

#include <stdexcept>
#include <string>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace Hyprview {

    int overviewLua(lua_State* L) {
        std::string action = "toggle";

        if (lua_istable(L, 1)) {
            lua_getfield(L, 1, "action");
            if (lua_isstring(L, -1))
                action = lua_tostring(L, -1);
            lua_pop(L, 1);
        } else if (lua_isstring(L, 1))
            action = lua_tostring(L, 1);
        else if (!lua_isnoneornil(L, 1))
            return luaL_error(L, "hyprview.overview: expected an action string or table { action }");

        const auto result = onOverviewDispatcher(action);
        if (!result.success)
            return luaL_error(L, result.error.empty() ? "hyprview.overview failed" : result.error.c_str());

        return 0;
    }

    int closeLua(lua_State* L) {
        bool switchToSelection = true;

        if (lua_isboolean(L, 1))
            switchToSelection = lua_toboolean(L, 1);
        else if (lua_istable(L, 1)) {
            lua_getfield(L, 1, "select");
            if (lua_isboolean(L, -1))
                switchToSelection = lua_toboolean(L, -1);
            lua_pop(L, 1);
        } else if (!lua_isnoneornil(L, 1))
            return luaL_error(L, "hyprview.close: expected boolean or table { select }");

        if (g_pOverview)
            g_pOverview->close(switchToSelection);

        return 0;
    }

    int moveHoveredWindowLua(lua_State* L) {
        if (!lua_isnoneornil(L, 1))
            return luaL_error(L, "hyprview.move_hovered_window: expected no arguments");

        const auto result = moveHoveredWindowToActiveWorkspace();
        if (!result.success)
            return luaL_error(L, result.error.empty() ? "hyprview.move_hovered_window failed" : result.error.c_str());

        return 0;
    }

    void registerLuaApi() {
        if (!HyprlandAPI::addLuaFunction(PHANDLE, "hyprview", "overview", overviewLua))
            throw std::runtime_error("[he] failed to register hl.plugin.hyprview.overview");
        if (!HyprlandAPI::addLuaFunction(PHANDLE, "hyprview", "gesture", overviewGestureLua))
            throw std::runtime_error("[he] failed to register hl.plugin.hyprview.gesture");
        if (!HyprlandAPI::addLuaFunction(PHANDLE, "hyprview", "configure", configureLua))
            throw std::runtime_error("[he] failed to register hl.plugin.hyprview.configure");
        if (!HyprlandAPI::addLuaFunction(PHANDLE, "hyprview", "close", closeLua))
            throw std::runtime_error("[he] failed to register hl.plugin.hyprview.close");
        if (!HyprlandAPI::addLuaFunction(PHANDLE, "hyprview", "move_hovered_window", moveHoveredWindowLua))
            throw std::runtime_error("[he] failed to register hl.plugin.hyprview.move_hovered_window");
    }

}
