#pragma once

#include <string>

#include <hyprland/src/plugins/PluginAPI.hpp>

namespace Hyprview {

    SDispatchResult onOverviewDispatcher(std::string arg);
    SDispatchResult moveHoveredWindowToActiveWorkspace();

}
