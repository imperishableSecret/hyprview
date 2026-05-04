#include "LuaConfig.hpp"

#include "Globals.hpp"
#include "PluginRuntime.hpp"

#include <expected>
#include <format>
#include <string>

#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/lua/bindings/LuaBindingsInternal.hpp>
#include <hyprland/src/helpers/MiscFunctions.hpp>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace {
    enum class eLuaCallbackUpdate {
        UNCHANGED = 0,
        CLEAR,
        SET,
    };

    struct SLuaCallbackUpdate {
        eLuaCallbackUpdate action = eLuaCallbackUpdate::UNCHANGED;
        int                ref    = LUA_NOREF;
    };

    lua_State* g_luaState       = nullptr;
    int        g_onCloseRef     = LUA_NOREF;
    bool       g_runningOnClose = false;

    void       unrefLuaCallback(lua_State* L, int& ref) {
        if (L && ref != LUA_NOREF && ref != LUA_REFNIL)
            luaL_unref(L, LUA_REGISTRYINDEX, ref);

        ref = LUA_NOREF;
    }

    std::expected<void, std::string> readLuaIntField(lua_State* L, int tableIdx, const char* key, int& value) {
        const int TABLE = lua_absindex(L, tableIdx);
        lua_getfield(L, TABLE, key);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            return {};
        }

        if (!lua_isinteger(L, -1)) {
            lua_pop(L, 1);
            return std::unexpected(std::format("{} must be an integer", key));
        }

        value = sc<int>(lua_tointeger(L, -1));
        lua_pop(L, 1);
        return {};
    }

    std::expected<void, std::string> readLuaFloatField(lua_State* L, int tableIdx, const char* key, float& value) {
        const int TABLE = lua_absindex(L, tableIdx);
        lua_getfield(L, TABLE, key);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            return {};
        }

        if (!lua_isnumber(L, -1)) {
            lua_pop(L, 1);
            return std::unexpected(std::format("{} must be a number", key));
        }

        value = sc<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        return {};
    }

    std::expected<void, std::string> readLuaBoolField(lua_State* L, int tableIdx, const char* key, bool& value) {
        const int TABLE = lua_absindex(L, tableIdx);
        lua_getfield(L, TABLE, key);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            return {};
        }

        if (lua_isboolean(L, -1))
            value = lua_toboolean(L, -1);
        else if (lua_isinteger(L, -1))
            value = lua_tointeger(L, -1) != 0;
        else {
            lua_pop(L, 1);
            return std::unexpected(std::format("{} must be a boolean", key));
        }

        lua_pop(L, 1);
        return {};
    }

    std::expected<void, std::string> readLuaStringField(lua_State* L, int tableIdx, const char* key, std::string& value) {
        const int TABLE = lua_absindex(L, tableIdx);
        lua_getfield(L, TABLE, key);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            return {};
        }

        if (!lua_isstring(L, -1)) {
            lua_pop(L, 1);
            return std::unexpected(std::format("{} must be a string", key));
        }

        value = lua_tostring(L, -1);
        lua_pop(L, 1);
        return {};
    }

    std::expected<void, std::string> readLuaColorField(lua_State* L, int tableIdx, const char* key, int64_t& value) {
        const int TABLE = lua_absindex(L, tableIdx);
        lua_getfield(L, TABLE, key);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            return {};
        }

        if (lua_isinteger(L, -1)) {
            value = sc<int64_t>(lua_tointeger(L, -1));
            lua_pop(L, 1);
            return {};
        }

        if (lua_isstring(L, -1)) {
            const auto PARSED = configStringToInt(lua_tostring(L, -1));
            lua_pop(L, 1);
            if (!PARSED)
                return std::unexpected(std::format("{} must be a color: {}", key, PARSED.error()));
            value = *PARSED;
            return {};
        }

        lua_pop(L, 1);
        return std::unexpected(std::format("{} must be a color string or integer", key));
    }

    std::expected<void, std::string> applyLuaScrollingConfig(lua_State* L, int tableIdx, SHyprviewScrollingConfig& config) {
        if (auto result = readLuaBoolField(L, tableIdx, "scroll_moves_up_down", config.scrollMovesUpDown); !result)
            return result;
        if (auto result = readLuaFloatField(L, tableIdx, "default_zoom", config.defaultZoom); !result)
            return result;
        if (auto result = readLuaIntField(L, tableIdx, "window_gap", config.windowGap); !result)
            return result;
        if (auto result = readLuaBoolField(L, tableIdx, "background_blur", config.backgroundBlur); !result)
            return result;
        if (auto result = readLuaColorField(L, tableIdx, "backdrop_col", config.backdropColor); !result)
            return result;
        if (auto result = readLuaColorField(L, tableIdx, "workspace_shadow_col", config.workspaceShadowColor); !result)
            return result;
        if (auto result = readLuaIntField(L, tableIdx, "workspace_shadow_size", config.workspaceShadowSize); !result)
            return result;
        if (auto result = readLuaStringField(L, tableIdx, "focus_indicator", config.focusIndicator); !result)
            return result;
        if (auto result = readLuaIntField(L, tableIdx, "active_border_size", config.activeBorderSize); !result)
            return result;
        if (auto result = readLuaColorField(L, tableIdx, "hover_col", config.hoverColor); !result)
            return result;
        if (auto result = readLuaColorField(L, tableIdx, "drop_target_col", config.dropTargetColor); !result)
            return result;
        if (auto result = readLuaFloatField(L, tableIdx, "drag_alpha", config.dragAlpha); !result)
            return result;
        if (auto result = readLuaFloatField(L, tableIdx, "invalid_drag_alpha", config.invalidDragAlpha); !result)
            return result;
        if (auto result = readLuaIntField(L, tableIdx, "edge_scroll_zone", config.edgeScrollZone); !result)
            return result;
        if (auto result = readLuaFloatField(L, tableIdx, "edge_scroll_speed", config.edgeScrollSpeed); !result)
            return result;
        if (auto result = readLuaIntField(L, tableIdx, "hover_activate_ms", config.hoverActivateMs); !result)
            return result;
        if (auto result = readLuaStringField(L, tableIdx, "workspace_annotation", config.workspaceAnnotation); !result)
            return result;
        if (auto result = readLuaStringField(L, tableIdx, "workspace_annotation_position", config.workspaceAnnotationPosition); !result)
            return result;
        if (auto result = readLuaColorField(L, tableIdx, "workspace_annotation_color", config.workspaceAnnotationColor); !result)
            return result;
        if (auto result = readLuaColorField(L, tableIdx, "workspace_annotation_bg_col", config.workspaceAnnotationBgColor); !result)
            return result;
        if (auto result = readLuaIntField(L, tableIdx, "workspace_annotation_font_size", config.workspaceAnnotationFontSize); !result)
            return result;
        if (auto result = readLuaBoolField(L, tableIdx, "insertion_marker_labels", config.insertionMarkerLabels); !result)
            return result;
        if (auto result = readLuaIntField(L, tableIdx, "insertion_max_markers", config.insertionMaxMarkers); !result)
            return result;
        if (auto result = readLuaIntField(L, tableIdx, "append_marker_count", config.appendMarkerCount); !result)
            return result;
        if (auto result = readLuaColorField(L, tableIdx, "insertion_marker_col", config.insertionMarkerColor); !result)
            return result;
        if (auto result = readLuaColorField(L, tableIdx, "invalid_insertion_marker_col", config.invalidInsertionMarkerColor); !result)
            return result;

        return {};
    }

    std::expected<void, std::string> applyLuaKeyboardConfig(lua_State* L, int tableIdx, SHyprviewKeyboardConfig& config) {
        if (auto result = readLuaBoolField(L, tableIdx, "enabled", config.enabled); !result)
            return result;
        if (auto result = readLuaBoolField(L, tableIdx, "grab", config.grab); !result)
            return result;
        if (auto result = readLuaBoolField(L, tableIdx, "remember_selection", config.rememberSelection); !result)
            return result;
        if (auto result = readLuaBoolField(L, tableIdx, "wrap", config.wrap); !result)
            return result;
        if (auto result = readLuaBoolField(L, tableIdx, "activation_closes_overview", config.activationClosesOverview); !result)
            return result;

        return {};
    }

    std::expected<void, std::string> applyLuaMouseConfig(lua_State* L, int tableIdx, SHyprviewMouseConfig& config) {
        if (auto result = readLuaBoolField(L, tableIdx, "select_follows_hover", config.selectFollowsHover); !result)
            return result;
        if (auto result = readLuaBoolField(L, tableIdx, "edge_navigation", config.edgeNavigation); !result)
            return result;
        if (auto result = readLuaFloatField(L, tableIdx, "edge_navigation_speed", config.edgeNavigationSpeed); !result)
            return result;

        return {};
    }

    std::expected<void, std::string> applyLuaHyprviewConfig(lua_State* L, int tableIdx, SHyprviewConfig& config) {
        const int TABLE = lua_absindex(L, tableIdx);

        if (auto result = readLuaIntField(L, TABLE, "gesture_distance", config.gestureDistance); !result)
            return result;

        lua_getfield(L, TABLE, "scrolling");
        if (!lua_isnil(L, -1)) {
            if (!lua_istable(L, -1)) {
                lua_pop(L, 1);
                return std::unexpected("scrolling must be a table");
            }

            auto result = applyLuaScrollingConfig(L, lua_gettop(L), config.scrolling);
            lua_pop(L, 1);
            if (!result)
                return result;
        } else
            lua_pop(L, 1);

        lua_getfield(L, TABLE, "keyboard");
        if (!lua_isnil(L, -1)) {
            if (!lua_istable(L, -1)) {
                lua_pop(L, 1);
                return std::unexpected("keyboard must be a table");
            }

            auto result = applyLuaKeyboardConfig(L, lua_gettop(L), config.keyboard);
            lua_pop(L, 1);
            if (!result)
                return result;
        } else
            lua_pop(L, 1);

        lua_getfield(L, TABLE, "mouse");
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            return {};
        }

        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            return std::unexpected("mouse must be a table");
        }

        auto result = applyLuaMouseConfig(L, lua_gettop(L), config.mouse);
        lua_pop(L, 1);
        return result;
    }

    std::expected<SLuaCallbackUpdate, std::string> readOnCloseCallback(lua_State* L, int tableIdx) {
        const int TABLE = lua_absindex(L, tableIdx);
        lua_getfield(L, TABLE, "on_close");

        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            return SLuaCallbackUpdate{};
        }

        if (lua_isboolean(L, -1) && !lua_toboolean(L, -1)) {
            lua_pop(L, 1);
            return SLuaCallbackUpdate{.action = eLuaCallbackUpdate::CLEAR};
        }

        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            return std::unexpected("on_close must be a function, false, or nil");
        }

        const int REF = luaL_ref(L, LUA_REGISTRYINDEX);
        return SLuaCallbackUpdate{.action = eLuaCallbackUpdate::SET, .ref = REF};
    }

    void applyOnCloseCallback(lua_State* L, SLuaCallbackUpdate update) {
        if (update.action == eLuaCallbackUpdate::UNCHANGED)
            return;

        unrefLuaCallback(g_luaState, g_onCloseRef);

        if (update.action == eLuaCallbackUpdate::SET) {
            g_luaState   = L;
            g_onCloseRef = update.ref;
        } else
            g_luaState = nullptr;
    }
}

namespace Hyprview {

    int configureLua(lua_State* L) {
        if (!lua_istable(L, 1))
            return Config::Lua::Bindings::Internal::configError(L, "hyprview.configure: expected a table");

        auto nextConfig = g_hyprviewConfig;
        auto result     = applyLuaHyprviewConfig(L, 1, nextConfig);
        if (!result)
            return Config::Lua::Bindings::Internal::configError(L, std::format("hyprview.configure: {}", result.error()));

        auto callbackUpdate = readOnCloseCallback(L, 1);
        if (!callbackUpdate)
            return Config::Lua::Bindings::Internal::configError(L, std::format("hyprview.configure: {}", callbackUpdate.error()));

        g_hyprviewConfig = nextConfig;
        applyOnCloseCallback(L, *callbackUpdate);
        damageOverviewMonitor();
        return 0;
    }

    void runOnCloseLuaCallback() {
        if (!g_luaState || g_onCloseRef == LUA_NOREF || g_onCloseRef == LUA_REFNIL || g_runningOnClose)
            return;

        g_runningOnClose = true;
        lua_rawgeti(g_luaState, LUA_REGISTRYINDEX, g_onCloseRef);
        if (lua_pcall(g_luaState, 0, 0, 0) != LUA_OK) {
            const char* ERROR = lua_tostring(g_luaState, -1);
            HyprlandAPI::addNotification(PHANDLE, std::string{"[hyprview] on_close callback failed: "} + (ERROR ? ERROR : "unknown error"), CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
            lua_pop(g_luaState, 1);
        }
        g_runningOnClose = false;
    }

    void resetLuaCallbacks() {
        unrefLuaCallback(g_luaState, g_onCloseRef);
        g_luaState       = nullptr;
        g_runningOnClose = false;
    }

}
