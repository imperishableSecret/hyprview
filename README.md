# Hyprview

Hyprview is a Hyprland plugin that adds a niri-like scrolling workspace overview. It shows workspace thumbnails for the current monitor, lets you select a workspace or window, drag windows between workspaces, and pan wide scrolling-layout workspaces inside the overview.

## Core features

| feature | behavior |
| --- | --- |
| Niri-like overview | Shows workspaces as a vertical scrolling overview instead of a grid. |
| Per-monitor overview | Opens for the focused monitor and only includes workspaces from that monitor. |
| Workspace thumbnails | Captures each visible overview workspace into thumbnail-sized window images. |
| Live-ish thumbnail updates | Tracks window commits, damage, moves, and resizes, then redraws dirty window thumbnails before rendering the overview. |
| Window selection | Clicking a window closes the overview, switches to its workspace if needed, focuses the window, and warps the cursor to it. |
| Workspace selection | Clicking a workspace closes the overview and switches to that workspace. |
| Window dragging | Left-drag a non-pinned window inside the overview and drop it on another workspace. |
| Workspace insertion markers | Drop a dragged window on a numbered insertion marker to create or select that workspace ID and move the window there. |
| Horizontal content pan | Right-drag a workspace using Hyprland's `scrolling` tiled layout to pan tiled window thumbnails horizontally. |
| Wheel navigation | Wheel input moves between overview workspaces by default. It can be configured to zoom instead. |
| Keyboard selection | Optional Lua-controlled selection dispatchers move the selected thumbnail left/right/up/down and activate it. |
| Normal Hyprland keybinds | Workspace switching, focus movement, and moving windows between workspaces keep working while the overview is open. Hyprview refreshes and recenters from the resulting Hyprland events. |
| Trackpad gesture | Lua config can register a trackpad gesture that opens the overview when closed and drives overview scale while swiping. |
| Workspace badges | Optional workspace annotations can show ID, name, ID/name combinations, or window count. |
| Hover/focus styling | Hovered windows can use a translucent overview box or Hyprland's active border gradient. |
| Drag feedback | Valid and invalid drag targets use separate overlay colors and preview opacity. |
| Backdrop modes | Uses a captured background by default, or a manually captured blurred background when `background_blur = true`. |
| Lua API | Provides Lua functions for configure, overview control, close, moving the hovered window, and gesture registration. |
| Dispatcher | Registers `hyprview:overview` internally. Lua users normally call `hl.plugin.hyprview.overview(...)`, which wraps the same action path. |

## Compatibility

Hyprland plugins are ABI-sensitive. Hyprview checks the running Hyprland API hash at plugin load time and refuses to initialize if the plugin was built against different Hyprland headers.

Hyprview also requires Hyprland's Lua config. If Hyprland is not using Lua config, the plugin refuses to initialize with a notification.

Pinned Hyprland/plugin commit pairs are tracked in `hyprpm.toml`.

On plugin unload, Hyprview removes overview pass elements, marks gesture registration as unloading, and reloads Hyprland config so registered gestures are cleared.

## Build

Requirements are provided through `pkg-config`:

| dependency |
| --- |
| `hyprland` |
| `pixman-1` |
| `libdrm` |
| `pangocairo` |
| `libinput` |
| `libudev` |
| `wayland-server` |
| `xkbcommon` |

Build from the repository root:

```sh
make all
```

The build writes `hyprview.so` in the repository root.

Useful make targets:

| target | behavior |
| --- | --- |
| `make all` | Build `hyprview.so`. |
| `make format-fix` | Run `clang-format -i` over `src/**/*.cpp` and `src/**/*.hpp`. |
| `make clean` | Remove `hyprview.so`. |

CMake and Meson build files are also present, but `make all` is the primary local build path for this repo.

## Lua configuration

Hyprview is configured and controlled from Hyprland's Lua config through `hl.plugin.hyprview.*` functions.

Example:

```lua
hl.plugin.hyprview.configure {
    gesture_distance = 200,
    on_close = function()
        hl.dispatch("submap", "reset")
    end,
    keyboard = {
        enabled = true,
        grab = false,
        remember_selection = true,
        wrap = true,
        activation_closes_overview = true,
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
        insertion_marker_labels = true,
        insertion_max_markers = 8,
        append_marker_count = 1,
        insertion_marker_col = "rgba(a7c7ff44)",
        invalid_insertion_marker_col = "rgba(ff5c5c55)",
    },
}

hl.plugin.hyprview.gesture {
    fingers = 3,
    direction = "down",
    gesture = "overview",
}

hl.bind("SUPER+G", function()
    hl.plugin.hyprview.overview("toggle")
end)
```

Keyboard selection can be bound through overview-local key overrides. These
binds only run while Hyprview is open and `keyboard.grab = true`. Matching
overview binds are cancelled before normal Hyprland keybind handling; unmatched
keys continue through Hyprland normally.

```lua
hl.plugin.hyprview.configure {
    keyboard = {
        enabled = true,
        grab = true,
    },
}

hl.plugin.hyprview.bind("SUPER", "LEFT", hl.plugin.hyprview.selection_left)
hl.plugin.hyprview.bind("SUPER", "RIGHT", hl.plugin.hyprview.selection_right)
hl.plugin.hyprview.bind("SUPER", "UP", hl.plugin.hyprview.selection_up)
hl.plugin.hyprview.bind("SUPER", "DOWN", hl.plugin.hyprview.selection_down)
hl.plugin.hyprview.bind("", "RETURN", hl.plugin.hyprview.selection_activate)
hl.plugin.hyprview.bind("", "ESCAPE", function()
    hl.plugin.hyprview.close(false)
end)
```

## Options

Top-level options:

| property | type | default | description |
| --- | --- | --- | --- |
| `gesture_distance` | integer | `200` | Gesture travel distance used to interpolate the overview scale animation. Values below `1` are treated as `1`. |
| `on_close` | function, `false`, or `nil` | `nil` | Optional callback called once whenever the overview begins closing. Use it to reset Lua-owned submaps. `false` clears an existing callback. |

Keyboard options:

| property | type | default | description |
| --- | --- | --- | --- |
| `enabled` | boolean | `true` | Enables the overview-native selection dispatchers. If disabled, `selection_left`, `selection_right`, `selection_up`, `selection_down`, and `selection_activate` are no-ops. |
| `grab` | boolean | `false` | Enables overview-local key overrides registered through `hl.plugin.hyprview.bind`. Only matching overview binds are cancelled while the overview is open; unmatched keys keep normal Hyprland behavior. |
| `remember_selection` | boolean | `true` | Remembers the selected window per workspace and restores it when the viewport returns to that workspace. |
| `wrap` | boolean | `true` | Allows left/right selection to wrap within a workspace and up/down selection to wrap between the first and last overview workspace. |
| `activation_closes_overview` | boolean | `true` | `selection_activate` closes the overview after focusing the selected window. If disabled, it focuses the selected window and keeps the overview open. |

Mouse options:

| property | type | default | description |
| --- | --- | --- | --- |
| `select_follows_hover` | boolean | `true` | Pointer hover updates the selected thumbnail. Disable this if keyboard selection should stay fixed until keyboard navigation changes it. |
| `edge_navigation` | boolean | `true` | Pointer motion near the top/bottom overview edge moves the workspace viewport. Disable this to rely on wheel input or right-click drag panning instead. |
| `edge_navigation_snap` | boolean | `true` | Move one workspace when the pointer enters the top/bottom edge zone, then wait until the pointer leaves and re-enters before moving again. Disable to restore continuous edge motion. |
| `edge_navigation_speed` | float | `1.0` | Pointer edge-navigation speed multiplier used when `edge_navigation_snap = false`. Higher values move through overview workspaces faster. |
| `hitbox_expansion` | integer | `16` | Extra logical pixels around each window thumbnail for pointer hit testing. Set to `0` to restore exact thumbnail-only hits. |
| `nearest_hitbox` | boolean | `true` | If expanded hitboxes overlap, choose the nearest real thumbnail. If disabled, the first expanded hit in reverse render order wins. |
| `snap_pan` | boolean | `true` | Enables pointer-driven horizontal snap panning for scrolling-layout workspace thumbnails. Right-click drag panning still works when this is disabled. |
| `snap_pan_zone` | integer | `96` | Left/right screen edge size, in logical pixels, used for horizontal snap panning. |

Scrolling options:

| property | type | default | description |
| --- | --- | --- | --- |
| `scroll_moves_up_down` | boolean | `true` | If enabled, vertical wheel/scroll input moves between overview workspaces. If disabled, wheel/scroll input changes zoom. |
| `default_zoom` | float | `0.5` | Default overview zoom, clamped to `0.1..0.9` when opening or completing a gesture. |
| `window_gap` | integer | `0` | Visual gap between window thumbnails. Values below `0` render as `0`. |
| `background_blur` | boolean | `false` | Capture and blur the workspace background manually instead of using the default captured background. |
| `backdrop_col` | color | `rgba(000000ff)` | Color drawn behind overview thumbnails and any captured backdrop texture. |
| `workspace_shadow_col` | color | `rgba(00000000)` | Optional shadow color behind workspace thumbnails. Alpha `0` disables visible shadows. |
| `workspace_shadow_size` | integer | `0` | Shadow expansion around workspace thumbnails. Values below `0` render as `0`. |
| `focus_indicator` | string | `overview_box` | `overview_box` draws the hover color over the selected thumbnail. `active_border` draws Hyprland's active border gradient. Other values fall back to `overview_box`. |
| `active_border_size` | integer | `4` | Border thickness used by `active_border`, clamped to `1..64`. |
| `hover_col` | color | `rgba(a7c7ff33)` | Hover overlay color for `overview_box` and valid drag previews. |
| `drop_target_col` | color | `rgba(a7c7ff22)` | Workspace-body drop highlight color while dragging a window. |
| `drag_alpha` | float | `1.0` | Dragged-window preview opacity for valid drops, clamped to `0..1`. |
| `invalid_drag_alpha` | float | `0.55` | Dragged-window preview opacity for invalid drops, clamped to `0..1`. |
| `edge_scroll_zone` | integer | `64` | Top/bottom edge size in pixels used for autoscroll while dragging a window. Values below `0` render as `0`. |
| `edge_scroll_speed` | float | `1.0` | Edge autoscroll speed multiplier. `1.0` is roughly one workspace per second at full edge pressure. Values below `0` render as `0`. |
| `hover_activate_ms` | integer | `600` | Delay before a hovered workspace body is centered while dragging a window. Values below `0` render as `0`. |
| `workspace_annotation` | string | `id` | Workspace badge text mode: `none`, `id`, `name`, `id_name`, `name_id`, or `id_windows`. Unknown values fall back to `id`. |
| `workspace_annotation_position` | string | `top_left` | Badge position: `top_left`, `top_right`, `bottom_left`, or `bottom_right`. Unknown values behave like `top_left`. |
| `workspace_annotation_color` | color | `rgba(ffffffff)` | Workspace badge and insertion label text color. |
| `workspace_annotation_bg_col` | color | `rgba(00000099)` | Workspace badge background color. |
| `workspace_annotation_font_size` | integer | `14` | Workspace badge and insertion label font size. Values below `1` render as `1`. |
| `insertion_marker_labels` | boolean | `true` | Show target workspace IDs inside insertion markers. |
| `insertion_max_markers` | integer | `8` | Maximum insertion markers shown for one numeric workspace gap. Values below `0` render as `0`. |
| `append_marker_count` | integer | `1` | Number of append workspace markers after the last numeric workspace, clamped to `0..5`. |
| `insertion_marker_col` | color | `rgba(a7c7ff44)` | Insertion marker color. |
| `invalid_insertion_marker_col` | color | `rgba(ff5c5c55)` | Invalid drag/drop feedback color. |

Color options accept Hyprland-style color strings or integer values.

Configuration is applied atomically: Hyprview parses into a copy of the current config and only replaces the active config after all fields validate. Integer fields require Lua integers. Float fields accept Lua numbers. Boolean fields accept booleans and integer values, where non-zero integers are treated as true. String fields require strings. Invalid fields report a Lua config error and leave the previous active config in place.

Calling `hl.plugin.hyprview.configure` while the overview is open updates the config and damages the overview monitor so the new settings can render. Config resets to defaults before Hyprland config reloads, so Lua config should call `hl.plugin.hyprview.configure` on each reload.

## Workspace behavior

Hyprview builds the overview from non-special workspaces on the focused monitor. Workspaces are sorted by workspace ID. A workspace appears when it has at least one displayable window. The active workspace also appears even when it is empty.

Empty workspaces, including persistent empty workspaces, are hidden from the overview but can still appear as insertion targets by workspace ID.

Pinned windows are shown only on the active workspace for the current monitor and cannot be dragged across workspaces.

Floating windows are rendered after tiled windows and are raised when selected or dragged. Horizontal right-drag panning affects tiled windows only; floating windows keep their normal position.

When windows open, close, move between workspaces, or change active focus while the overview is open, Hyprview queues a refresh and recenters the overview. Keybind-driven focus changes also briefly warp the cursor to the newly focused window thumbnail.

## Insertion markers

Insertion markers use real Hyprland workspace IDs. If workspaces `1` and `3` exist, dragging to the marker between them targets workspace `2`. If workspaces `1` and `6` exist, markers `2`, `3`, `4`, and `5` are shown, capped by `insertion_max_markers`.

No marker is shown before the first numeric workspace. Append markers are shown after the last numeric workspace according to `append_marker_count`. Append markers can target new workspace IDs after the last numeric workspace.

Insertion targets respect monitor-bound workspace rules. Existing target workspaces must belong to the overview monitor and have no displayable windows. New insertion workspaces are created on the overview monitor.

## Controls

| input | behavior |
| --- | --- |
| Left click | Select the hovered window or workspace and close the overview. |
| Left drag a window | Drag that window inside the overview. Pinned windows cannot be dragged across workspaces. |
| Release dragged window over a workspace | Move the window to that workspace, activate that workspace without stealing focus from the dragged window, focus the dragged window, and refresh the overview. |
| Release dragged window over a numbered insertion marker | Create or select that workspace ID, move the window there, focus it, and refresh the overview. |
| Right drag | Pan tiled thumbnails inside the hovered scrolling-layout workspace horizontally. |
| Mouse wheel with `scroll_moves_up_down = true` | Move the overview up/down by workspace. Wheel steps move one workspace; smooth vertical scrolling accumulates until the threshold is reached. Horizontal scroll axes are ignored. |
| Mouse wheel with `scroll_moves_up_down = false` | Zoom the overview in/out. Zoom is clamped to `0.05..0.95` while wheel-zooming. |
| Touch press | Select the hovered workspace/window and close the overview. |
| `hl.plugin.hyprview.selection_left` | Move the selected thumbnail left within the current workspace. |
| `hl.plugin.hyprview.selection_right` | Move the selected thumbnail right within the current workspace. |
| `hl.plugin.hyprview.selection_up` | Move to the previous overview workspace and restore its remembered selection. |
| `hl.plugin.hyprview.selection_down` | Move to the next overview workspace and restore its remembered selection. |
| `hl.plugin.hyprview.selection_activate` | Focus the selected window. By default this also closes the overview. |
| Normal Hyprland keybinds | Continue to focus or move windows. The overview queues refreshes and recenters from Hyprland workspace/window events. |

While dragging a window, hovering a workspace body for `hover_activate_ms` centers that workspace. Moving the pointer into the top or bottom `edge_scroll_zone` autoscrolls through workspaces at `edge_scroll_speed`.

Outside a drag, pointer workspace navigation is snap-based by default: entering the top or bottom edge zone moves one workspace and waits for the pointer to leave the zone before moving again. Scrolling-layout thumbnails also support horizontal snap panning from the left or right screen edge. A horizontal snap centers the next tiled thumbnail in that direction and waits until the pointer leaves the edge before snapping again. Right-click drag remains the manual continuous pan path.

## Gesture behavior

`hl.plugin.hyprview.gesture` registers or removes a trackpad gesture through Hyprland's trackpad gesture manager.

| field | behavior |
| --- | --- |
| `fingers` | Required integer from `2` through `9`. |
| `direction` | Required direction string parsed by Hyprland. Invalid directions are rejected. |
| `gesture` | `overview` registers the gesture. `unset` removes the matching gesture. Defaults to `overview`. |
| `mod` | Optional modifier string converted through Hyprland's keybind manager. |
| `scale` | Optional gesture scale, clamped to `0.1..10.0`. |
| `disable_inhibit` | Optional boolean passed to Hyprland's gesture registration. |

Starting the gesture opens the overview if it is closed. Starting the gesture while the overview is already open selects the hovered item and uses close-style scaling while the gesture is active. Gesture updates interpolate the overview scale using `gesture_distance`, and very small gesture deltas are clamped above zero to avoid invalid swipe-end state.

## Lua API

| name | arguments | behavior |
| --- | --- | --- |
| `hl.plugin.hyprview.configure` | table | Applies Hyprview configuration. The config resets to defaults before Hyprland config reloads. |
| `hl.plugin.hyprview.overview` | action string or `{ action = "toggle" }` | Controls the overview. |
| `hl.plugin.hyprview.close` | optional boolean or `{ select = true }` | Closes the overview. Pass `false` or `{ select = false }` to close without switching to the hovered selection. |
| `hl.plugin.hyprview.move_hovered_window` | none | Moves the hovered overview window to the active workspace, focuses it, and warps the cursor to it. |
| `hl.plugin.hyprview.bind` | `mods`, `key`, function or `false` | Registers an overview-local key override. The override only applies while Hyprview is open and `keyboard.grab = true`. Passing `false` removes that override. |
| `hl.plugin.hyprview.selection_left` | none | Moves overview keyboard selection left within the current workspace. |
| `hl.plugin.hyprview.selection_right` | none | Moves overview keyboard selection right within the current workspace. |
| `hl.plugin.hyprview.selection_up` | none | Moves overview keyboard selection to the previous workspace. |
| `hl.plugin.hyprview.selection_down` | none | Moves overview keyboard selection to the next workspace. |
| `hl.plugin.hyprview.selection_activate` | none | Activates the selected overview window. |
| `hl.plugin.hyprview.gesture` | `{ fingers, direction, gesture, mod?, scale?, disable_inhibit? }` | Registers or removes a trackpad gesture. |

Keyboard dispatchers are plain Lua API calls. `hl.plugin.hyprview.bind` is the
recommended way to reuse keys like `SUPER+LEFT` for overview selection without
shadowing your normal Hyprland keybinds when the overview is closed.

Hyprview still does not own Hyprland submap state. If you prefer a fully modal
overview layer, define a Hyprland submap in Lua and bind the selection functions
inside that submap.

`on_close` is the intended bridge for modal key layers. It lets Lua reset a
submap even when the overview closes through mouse selection, touch, gesture, or
another plugin path.

Overview actions:

| action | behavior |
| --- | --- |
| `toggle` | Open the overview if closed, close it if open. |
| `select` | Select the hovered workspace/window and close the overview. |
| `bring` | Bring the topmost mapped, non-hidden window from the selected workspace to the active workspace, then close without switching workspace. |
| `off` | Close the overview. |
| `close` | Close the overview. |
| `disable` | Close the overview. |
| `on` | Open the overview if closed. |
| `enable` | Open the overview if closed. |
| unknown action | If the overview is closed, unknown actions open it. If the overview is already open, unknown actions are a no-op. |

## Rendering and damage quirks

Hyprview hooks Hyprland workspace rendering and monitor damage reporting while the overview is open. Normal workspace rendering is replaced by the overview render pass for the overview monitor only.

Window thumbnails are rendered into offscreen framebuffers using Hyprland's fake render path. X11 windows are considered renderable when mapped. Wayland windows require a current surface texture.

Damage is translated into monitor logical coordinates so only affected thumbnails are marked dirty when possible. Full damage reporting marks all renderable thumbnails dirty.

The overview pass reports a full-monitor bounding box and opaque region. It does not request live blur or precomputed blur from the pass element; the optional blurred background is captured manually into the overview background framebuffer. Background capture happens during overview image refresh, not as a continuously live background stream.

The background framebuffer and window framebuffers use `ABGR8888`. The overview clears window thumbnails transparent before rendering each window image.

## Known quirks

| quirk | detail |
| --- | --- |
| Lua-only plugin | The plugin refuses to initialize unless Hyprland is using Lua config. |
| ABI-sensitive | Build against the same Hyprland commit/API hash that will load the plugin. |
| Current monitor only | Workspaces from other monitors are excluded. |
| Special workspaces excluded | Special workspaces are not shown and are not valid insertion targets. |
| Empty workspace visibility | Empty workspaces are hidden unless they are the active workspace. |
| Pinned windows | Pinned windows are visible only on the active workspace and cannot be dragged across workspaces. |
| Horizontal pan scope | Right-drag panning only works for workspaces whose tiled algorithm name is `scrolling`; it pans tiled windows only. |
| Workspace insertion scope | Insertion markers only use positive numeric workspace IDs and obey monitor-bound workspace rules. |
| Unknown config strings | Unknown `focus_indicator`, `workspace_annotation`, or annotation position values fall back to the default behavior rather than failing config. |
| Background blur cost | `background_blur = true` captures and blurs the workspace background manually, which can be heavier than the default captured background. |
| Screen shader blocking | Fake renders set `blockScreenShader` to avoid applying screen shaders to thumbnail/background captures. |
| Cursor sync | Keybind-driven focus changes can warp the cursor to the focused thumbnail for a short sync window. |
| Background freshness | The background is captured when overview images refresh; individual window thumbnails are the parts that update from damage/commit tracking. |
| Bring action target | `bring` does not require hovering a specific window. It chooses the topmost mapped, non-hidden window from the selected workspace. |
| Unknown overview actions | Unknown actions open the overview when closed and otherwise do nothing. |
