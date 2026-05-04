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

    std::expected<void, std::string> applyLuaHyprviewConfig(lua_State* L, int tableIdx, SHyprviewConfig& config) {
        const int TABLE = lua_absindex(L, tableIdx);

        if (auto result = readLuaIntField(L, TABLE, "gesture_distance", config.gestureDistance); !result)
            return result;

        lua_getfield(L, TABLE, "scrolling");
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            return {};
        }

        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            return std::unexpected("scrolling must be a table");
        }

        auto result = applyLuaScrollingConfig(L, lua_gettop(L), config.scrolling);
        lua_pop(L, 1);
        return result;
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

        g_hyprviewConfig = nextConfig;
        damageOverviewMonitor();
        return 0;
    }

}
