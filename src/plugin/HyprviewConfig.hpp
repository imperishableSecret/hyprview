#pragma once

#include <cstdint>
#include <string>

struct SHyprviewScrollingConfig {
    bool        scrollMovesUpDown           = true;
    int         windowGap                   = 0;
    bool        backgroundBlur              = false;
    int64_t     backdropColor               = 0xFF000000;
    int64_t     workspaceShadowColor        = 0x00000000;
    int         workspaceShadowSize         = 0;
    std::string focusIndicator              = "overview_box";
    int         activeBorderSize            = 4;
    int64_t     hoverColor                  = 0x33A7C7FF;
    int64_t     dropTargetColor             = 0x22A7C7FF;
    float       dragAlpha                   = 1.0F;
    float       invalidDragAlpha            = 0.55F;
    int         edgeScrollZone              = 64;
    float       edgeScrollSpeed             = 1.0F;
    float       defaultZoom                 = 0.5F;
    int         hoverActivateMs             = 600;
    std::string workspaceAnnotation         = "id";
    std::string workspaceAnnotationPosition = "top_left";
    int64_t     workspaceAnnotationColor    = 0xFFFFFFFF;
    int64_t     workspaceAnnotationBgColor  = 0x99000000;
    int         workspaceAnnotationFontSize = 14;
    bool        insertionMarkerLabels       = true;
    int         insertionMaxMarkers         = 8;
    int         appendMarkerCount           = 1;
    int64_t     insertionMarkerColor        = 0x44A7C7FF;
    int64_t     invalidInsertionMarkerColor = 0x55FF5C5C;
};

struct SHyprviewKeyboardConfig {
    bool enabled                  = true;
    bool grab                     = false;
    bool rememberSelection        = true;
    bool wrap                     = true;
    bool activationClosesOverview = true;
};

struct SHyprviewMouseConfig {
    bool  selectFollowsHover  = true;
    bool  edgeNavigation      = true;
    bool  edgeNavigationSnap  = true;
    float edgeNavigationSpeed = 1.0F;
    int   hitboxExpansion     = 16;
    bool  nearestHitbox       = true;
    bool  snapPan             = true;
    int   snapPanZone         = 96;
};

struct SHyprviewConfig {
    int                      gestureDistance = 200;
    SHyprviewScrollingConfig scrolling;
    SHyprviewKeyboardConfig  keyboard;
    SHyprviewMouseConfig     mouse;
};

inline auto g_hyprviewConfig = SHyprviewConfig{};

inline void resetHyprviewConfig() {
    g_hyprviewConfig = SHyprviewConfig{};
}
