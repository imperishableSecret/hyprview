#pragma once

#include <string>

#include <hyprland/src/devices/IKeyboard.hpp>

struct lua_State;

namespace Event {
    struct SCallbackInfo;
}

namespace Hyprview {

    int  bindLua(lua_State* L);
    void handleKeyboardGrab(const IKeyboard::SKeyEvent& event, Event::SCallbackInfo& info);
    void resetKeyboardGrabBinds();

}
