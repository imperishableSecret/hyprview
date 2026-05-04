#include "GestureRegistration.hpp"

#include "HyprviewGesture.hpp"
#include "../plugin/PluginRuntime.hpp"

#include <algorithm>
#include <expected>
#include <format>
#include <string>

#include <hyprland/src/config/lua/bindings/LuaBindingsInternal.hpp>
#include <hyprland/src/helpers/MiscFunctions.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/managers/input/trackpad/GestureTypes.hpp>
#include <hyprland/src/managers/input/trackpad/TrackpadGestures.hpp>

extern "C" {
#include <lua.h>
}

namespace {
    std::expected<void, std::string> registerOverviewGesture(size_t fingerCount, const std::string& directionStr, const std::string& action, uint32_t modMask, float deltaScale,
                                                             bool disableInhibit) {
        if (Hyprview::isUnloading())
            return {};

        if (fingerCount <= 1 || fingerCount >= 10)
            return std::unexpected(std::format("Invalid value {} for finger count", fingerCount));

        const auto direction = g_pTrackpadGestures->dirForString(directionStr);

        if (direction == TRACKPAD_GESTURE_DIR_NONE)
            return std::unexpected(std::format("Invalid direction: {}", directionStr));

        std::expected<void, std::string> resultFromGesture;

        if (action == "overview")
            resultFromGesture = g_pTrackpadGestures->addGesture(makeUnique<CHyprviewGesture>(), fingerCount, direction, modMask, deltaScale, disableInhibit);
        else if (action == "unset")
            resultFromGesture = g_pTrackpadGestures->removeGesture(fingerCount, direction, modMask, deltaScale, disableInhibit);
        else
            return std::unexpected(std::format("Invalid gesture: {}", action));

        return resultFromGesture;
    }
}

namespace Hyprview {

    int overviewGestureLua(lua_State* L) {
        if (!lua_istable(L, 1))
            return Config::Lua::Bindings::Internal::configError(L, "hyprview.gesture: expected a table { fingers, direction, gesture, mod?, scale?, disable_inhibit? }");

        size_t      fingerCount = 0;
        std::string direction;
        std::string action         = "overview";
        uint32_t    modMask        = 0;
        float       deltaScale     = 1.F;
        bool        disableInhibit = false;

        lua_getfield(L, 1, "fingers");
        if (!lua_isinteger(L, -1))
            return Config::Lua::Bindings::Internal::configError(L, "hyprview.gesture: fingers must be an integer");
        fingerCount = sc<size_t>(lua_tointeger(L, -1));
        lua_pop(L, 1);

        lua_getfield(L, 1, "direction");
        if (!lua_isstring(L, -1))
            return Config::Lua::Bindings::Internal::configError(L, "hyprview.gesture: direction must be a string");
        direction = lua_tostring(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 1, "gesture");
        if (lua_isstring(L, -1))
            action = lua_tostring(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 1, "mod");
        if (lua_isstring(L, -1))
            modMask = g_pKeybindManager->stringToModMask(lua_tostring(L, -1));
        lua_pop(L, 1);

        lua_getfield(L, 1, "scale");
        if (lua_isnumber(L, -1))
            deltaScale = std::clamp(sc<float>(lua_tonumber(L, -1)), 0.1F, 10.F);
        lua_pop(L, 1);

        lua_getfield(L, 1, "disable_inhibit");
        disableInhibit = lua_toboolean(L, -1);
        lua_pop(L, 1);

        auto result = registerOverviewGesture(fingerCount, direction, action, modMask, deltaScale, disableInhibit);
        if (!result)
            return Config::Lua::Bindings::Internal::configError(L, std::format("hyprview.gesture: {}", result.error()));

        return 0;
    }

}
