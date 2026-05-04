cmake_minimum_required(VERSION 3.27)

project(hyprview
    DESCRIPTION "hyprview plugin for Hyprland"
    VERSION 0.1
)

set(CMAKE_CXX_STANDARD 23)

file(GLOB_RECURSE SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(hyprview SHARED ${SRC})

find_package(PkgConfig REQUIRED)
pkg_check_modules(deps REQUIRED IMPORTED_TARGET
    hyprland
    libdrm
    libinput
    libudev
    pangocairo
    pixman-1
    wayland-server
    xkbcommon
)
target_link_libraries(hyprview PRIVATE rt PkgConfig::deps)

install(TARGETS hyprview)
