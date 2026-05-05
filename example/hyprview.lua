-- Example Hyprview Lua config using overview-local key overrides.
--
-- Assumptions:
-- - `hl` is Hyprland's Lua config API object.
-- - Your local Lua config has `hl.bind`, `hl.dispatch`, and `hl.submap`
--   helpers. If your config uses different wrapper names, keep the Hyprview
--   calls and adapt only the bind/submap wrapper syntax.
--
-- Keyboard grab behavior:
-- - `hv.bind` does not register global Hyprland binds.
-- - The binds below are active only while Hyprview is open and
--   `keyboard.grab = true`.
-- - Only matching `hv.bind` keys are cancelled. All other keys continue to
--   normal Hyprland keybind handling.
-- - This lets you reuse SUPER+LEFT/RIGHT/UP/DOWN in Hyprview without replacing
--   your normal window-movement binds outside the overview.

local hv = hl.plugin.hyprview

hv.configure {
    keyboard = {
        enabled = true,
        grab = true,
        remember_selection = true,
        wrap = true,
        activation_closes_overview = true,
        focus_follows_selection = false,
    },

    mouse = {
        select_follows_hover = true,
        edge_navigation = true,
        edge_navigation_snap = true,
        edge_navigation_speed = 1.0,
        hitbox_expansion = 16,
        nearest_hitbox = true,
        snap_pan = true,
        snap_pan_zone = 96,
    },

    scrolling = {
        scroll_moves_up_down = true,
        default_zoom = 0.5,
        window_gap = 0,
        background_blur = false,
        backdrop_col = "rgba(000000ff)",
        workspace_shadow_col = "rgba(00000000)",
        workspace_shadow_size = 0,
        focus_indicator = "overview_box",
        active_border_size = 4,
        hover_col = "rgba(a7c7ff33)",
        drop_target_col = "rgba(a7c7ff22)",
        drag_alpha = 1.0,
        invalid_drag_alpha = 0.55,
        edge_scroll_zone = 64,
        edge_scroll_speed = 1.0,
        hover_activate_ms = 600,
        workspace_annotation = "id",
        workspace_annotation_position = "top_left",
        workspace_annotation_color = "rgba(ffffffff)",
        workspace_annotation_bg_col = "rgba(00000099)",
        workspace_annotation_font_size = 14,
        active_indicator = "corner",
        active_indicator_col = "rgba(ffffffff)",
        insertion_marker_labels = true,
        insertion_max_markers = 8,
        append_marker_count = 1,
        insertion_marker_col = "rgba(a7c7ff44)",
        invalid_insertion_marker_col = "rgba(ff5c5c55)",
    },
}

hv.gesture {
    fingers = 3,
    direction = "down",
    gesture = "overview",
}

local function open_hyprview()
    hv.overview("on")
end

local function close_hyprview_without_selection()
    hv.close(false)
end

hl.bind("SUPER", "TAB", open_hyprview)

-- These can be the same physical keys as your normal window movement binds.
-- While Hyprview is open, these exact combos run the overview callbacks and are
-- not passed to normal Hyprland keybind handling. When Hyprview is closed, they
-- do nothing inside Hyprview and your normal binds work as usual.
hv.bind("SUPER", "LEFT", hv.selection_left)
hv.bind("SUPER", "RIGHT", hv.selection_right)
hv.bind("SUPER", "UP", hv.selection_up)
hv.bind("SUPER", "DOWN", hv.selection_down)

-- Optional unmodified arrows if you want pure overview navigation.
hv.bind("", "LEFT", hv.selection_left)
hv.bind("", "RIGHT", hv.selection_right)
hv.bind("", "UP", hv.selection_up)
hv.bind("", "DOWN", hv.selection_down)

hv.bind("", "RETURN", hv.selection_activate)
hv.bind("SUPER", "RETURN", hv.selection_activate)

hv.bind("", "ESCAPE", close_hyprview_without_selection)
hv.bind("SUPER", "ESCAPE", close_hyprview_without_selection)
