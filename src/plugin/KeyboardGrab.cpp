#include "KeyboardGrab.hpp"

#include "Globals.hpp"
#include "../overview/IOverview.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace {
    struct SOverviewKeyBind {
        uint32_t     modMask = 0;
        xkb_keysym_t keysym  = XKB_KEY_NoSymbol;
        uint32_t     keycode = 0;
        std::string  mods;
        std::string  key;
        lua_State*   L   = nullptr;
        int          ref = LUA_NOREF;
    };

    std::vector<SOverviewKeyBind> g_keyBinds;
    std::unordered_set<uint32_t>  g_suppressedKeycodes;
    bool                          g_runningCallback = false;

    bool                          parseKeycode(const std::string& key, uint32_t& keycode) {
        std::string_view code = key;
        if (code.starts_with("code:"))
            code.remove_prefix(5);

        if (code.empty())
            return false;

        uint32_t parsed      = 0;
        const auto [ptr, ec] = std::from_chars(code.data(), code.data() + code.size(), parsed);
        if (ec != std::errc{} || ptr != code.data() + code.size())
            return false;

        keycode = parsed;
        return key.starts_with("code:") || parsed > 9;
    }

    bool parseKey(const std::string& key, xkb_keysym_t& keysym, uint32_t& keycode) {
        keycode = 0;
        keysym  = XKB_KEY_NoSymbol;

        if (parseKeycode(key, keycode))
            return true;

        keysym = xkb_keysym_from_name(key.c_str(), XKB_KEYSYM_CASE_INSENSITIVE);
        return keysym != XKB_KEY_NoSymbol;
    }

    void unrefBind(SOverviewKeyBind& bind) {
        if (bind.L && bind.ref != LUA_NOREF && bind.ref != LUA_REFNIL)
            luaL_unref(bind.L, LUA_REGISTRYINDEX, bind.ref);

        bind.ref = LUA_NOREF;
        bind.L   = nullptr;
    }

    void removeBind(uint32_t modMask, xkb_keysym_t keysym, uint32_t keycode) {
        std::erase_if(g_keyBinds, [modMask, keysym, keycode](auto& bind) {
            if (bind.modMask != modMask)
                return false;

            const bool MATCH = keycode != 0 ? bind.keycode == keycode : bind.keysym == keysym;
            if (MATCH)
                unrefBind(bind);

            return MATCH;
        });
    }

    bool bindMatches(const SOverviewKeyBind& bind, uint32_t mods, xkb_keysym_t keysym, uint32_t keycode) {
        if (bind.modMask != mods)
            return false;

        if (bind.keycode != 0)
            return bind.keycode == keycode;

        return bind.keysym != XKB_KEY_NoSymbol && bind.keysym == keysym;
    }

    void runBind(SOverviewKeyBind& bind) {
        if (!bind.L || bind.ref == LUA_NOREF || bind.ref == LUA_REFNIL || g_runningCallback)
            return;

        g_runningCallback = true;
        lua_rawgeti(bind.L, LUA_REGISTRYINDEX, bind.ref);
        if (lua_pcall(bind.L, 0, 0, 0) != LUA_OK) {
            const char* ERROR = lua_tostring(bind.L, -1);
            HyprlandAPI::addNotification(PHANDLE, std::string{"[hyprview] key bind callback failed: "} + (ERROR ? ERROR : "unknown error"), CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
            lua_pop(bind.L, 1);
        }
        g_runningCallback = false;
    }
}

namespace Hyprview {

    int bindLua(lua_State* L) {
        if (!lua_isstring(L, 1) || !lua_isstring(L, 2) || (!lua_isfunction(L, 3) && !(lua_isboolean(L, 3) && !lua_toboolean(L, 3))))
            return luaL_error(L, "hyprview.bind: expected mods string, key string, and function or false");

        const std::string MODS = lua_tostring(L, 1);
        const std::string KEY  = lua_tostring(L, 2);
        const uint32_t    MASK = g_pKeybindManager->stringToModMask(MODS);

        xkb_keysym_t      keysym  = XKB_KEY_NoSymbol;
        uint32_t          keycode = 0;
        if (!parseKey(KEY, keysym, keycode))
            return luaL_error(L, "%s", std::format("hyprview.bind: unknown key {}", KEY).c_str());

        removeBind(MASK, keysym, keycode);

        if (lua_isboolean(L, 3) && !lua_toboolean(L, 3))
            return 0;

        lua_pushvalue(L, 3);
        g_keyBinds.push_back({
            .modMask = MASK,
            .keysym  = keysym,
            .keycode = keycode,
            .mods    = MODS,
            .key     = KEY,
            .L       = L,
            .ref     = luaL_ref(L, LUA_REGISTRYINDEX),
        });

        return 0;
    }

    void handleKeyboardGrab(const IKeyboard::SKeyEvent& event, Event::SCallbackInfo& info) {
        const uint32_t KEYCODE = event.keycode + 8;

        if (event.state == WL_KEYBOARD_KEY_STATE_RELEASED && g_suppressedKeycodes.erase(KEYCODE) > 0) {
            info.cancelled = true;
            return;
        }

        if (info.cancelled || !g_pOverview || !g_hyprviewConfig.keyboard.enabled || !g_hyprviewConfig.keyboard.grab || g_keyBinds.empty())
            return;

        if (event.state != WL_KEYBOARD_KEY_STATE_PRESSED)
            return;

        const auto KEYBOARD = g_pSeatManager->m_keyboard.lock();
        if (!KEYBOARD || !KEYBOARD->m_xkbState)
            return;

        const xkb_keysym_t KEYSYM = xkb_state_key_get_one_sym(KEYBOARD->m_xkbState, KEYCODE);
        const uint32_t     MODS   = g_pInputManager->getModsFromAllKBs();

        for (auto it = g_keyBinds.rbegin(); it != g_keyBinds.rend(); ++it) {
            if (!bindMatches(*it, MODS, KEYSYM, KEYCODE))
                continue;

            info.cancelled = true;
            g_suppressedKeycodes.insert(KEYCODE);
            runBind(*it);
            return;
        }
    }

    void resetKeyboardGrabBinds() {
        for (auto& bind : g_keyBinds) {
            unrefBind(bind);
        }
        g_keyBinds.clear();
        g_suppressedKeycodes.clear();
        g_runningCallback = false;
    }

}
