#pragma once

struct lua_State;

namespace Hyprview {

    int  overviewLua(lua_State* L);
    int  closeLua(lua_State* L);
    int  moveHoveredWindowLua(lua_State* L);
    int  selectionLeftLua(lua_State* L);
    int  selectionRightLua(lua_State* L);
    int  selectionUpLua(lua_State* L);
    int  selectionDownLua(lua_State* L);
    int  selectionActivateLua(lua_State* L);
    int  bindLua(lua_State* L);
    void registerLuaApi();

}
