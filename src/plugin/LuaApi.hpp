#pragma once

struct lua_State;

namespace Hyprview {

    int  overviewLua(lua_State* L);
    int  closeLua(lua_State* L);
    int  moveHoveredWindowLua(lua_State* L);
    void registerLuaApi();

}
