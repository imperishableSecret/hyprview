# Hyprview Live Rendering Plan

## Decision

Hyprview is now a live-rendering overview. The snapshot model is retired as a design target.

The overview should render real Hyprland windows, layer surfaces, popups, decorations, and frame callbacks through Hyprland's compositor paths while Hyprview owns only overview geometry, interaction state, render ordering, and scheduling policy.

This plan replaces the older snapshot migration plan. Future work should optimize and clean up the current live path, not preserve the old cached-image architecture.

## Current status

The live rendering path is active.

| Area | Status |
| --- | --- |
| Window rendering | Live windows render through overview geometry. |
| Layer rendering | Wallpaper, background, bottom, top, overlay, and popups are rendered through live layer handling. |
| Workspace cards | Cards use live geometry records and configurable workspace gaps. |
| Blur | Background blur uses Hyprland monitor preblur instead of a custom blur pass. |
| Frame callbacks | Overview-aware callbacks keep visible live surfaces updating. |
| Hit testing | Pointer hit testing uses live visibility state. |
| Keyboard selection | Selection skips entries that are not live-renderable. |
| Drag and drop | Drag state stores the dragged Hyprland window plus live source geometry, and drag previews render live window content. |
| Documentation | README describes the live path, but old internal names still leak into code and plans. |

## Architecture model

Hyprview's live architecture has one source of visual truth: Hyprland surfaces.

| Responsibility | Owner | Notes |
| --- | --- | --- |
| Plugin hook integration | `src/plugin/PluginHooks.cpp` | Hooks render, damage, frame scheduling, and frame callback paths. |
| Overview interface | `src/overview/IOverview.hpp` | Exposes surface-aware and render-pass entry points. |
| Overview lifecycle | `src/overview/scroll/ScrollOverviewLifecycle.cpp` | Opens, closes, schedules frames, and injects the overview render pass. |
| Live render orchestration | `src/overview/scroll/ScrollOverviewRender.cpp` | Routes full render into the live render pipeline. |
| Live window rendering | `src/overview/scroll/ScrollOverviewLiveRender.cpp` | Renders live window surfaces into overview boxes. |
| Live layer rendering | `src/overview/scroll/ScrollOverviewLayers.cpp` | Renders workspace and monitor layer surfaces in the correct order. |
| Surface policy | `src/overview/scroll/ScrollOverviewSurface.cpp` | Classifies surface ownership and decides frame callback behavior. |
| Live geometry records | `src/overview/scroll/ScrollOverviewCache.cpp` and geometry files | Rebuilds window/workspace records from compositor state. |
| Interaction | `src/overview/scroll/ScrollOverviewInteraction.cpp` and related files | Uses live geometry for pointer, keyboard, pan, snap, selection, and drag/drop. |

## Design principles

- No cached window image is a source of truth.
- No fallback render mode should be added for retired snapshot behavior.
- Live geometry records should drive rendering, hit testing, selection, drag/drop, and workspace motion.
- Hyprland's renderer should draw Hyprland surfaces; Hyprview should not invent a compositor pipeline.
- Hyprland's existing preblur should be used for blur; Hyprview should not blur live content every frame.
- Frame callbacks should be delivered only to surfaces that are visible or policy-approved in the overview.
- Render only what intersects the visible overview viewport unless correctness requires otherwise.
- Fix root causes in lifecycle, ownership, geometry, or render order; do not add launcher-specific or timing-based hacks.
- Keep scrolling overview behavior local to `src/overview/scroll/` unless the plugin boundary requires otherwise.
- Keep docs updated with the current behavior after each feature slice.

## Completed work

| Phase | Result |
| --- | --- |
| Repository split | Hyprview is a scrolling-only plugin separated from the old mixed overview work. |
| Lua-only config | Non-Lua config and dispatcher paths were removed from the plugin behavior. |
| Hook hardening | Function hook registration moved to a safer pattern with clearer failure handling. |
| Keyboard control | Overview-specific keyboard navigation and keyboard takeover behavior are implemented. |
| Mouse pan and snap | Right-click pan and edge snap behavior are implemented for scrolling workspace cards. |
| Focus and selection sync | Focus changes can update overview selection, and selection memory is preserved across workspace movement. |
| Exit animation alignment | Closing the overview syncs to the active/tape state so zoom-out is visually aligned. |
| Fullscreen handling | Fullscreen state no longer hides the overview itself and scrolling workspaces handle their own fullscreen behavior. |
| Live windows | Windows render through live surface rendering instead of cached window captures. |
| Live layers | Workspace layers, monitor layers, popups, and wallpaper/backdrop handling are routed through live rendering. |
| Hyprland blur | Background blur uses Hyprland's monitor blur path. |
| Config additions | `scrolling.show_workspace_layers` and `scrolling.workspace_gap` are documented and implemented. |
| Snapshot removal start | Per-window framebuffer allocation and redraw paths were removed from the current live records path. |
| Live drag geometry | Drag/drop stores explicit live drag state, renders dragged previews from live window geometry, repositions floating windows from overview drops, and commits tiled drops through Hyprland layout targets. |

## Phase details

## Phase A: Rename image-era data structures to live entries

Status: Implemented.

Goal: Make the code describe the current model instead of preserving old mental models.

Work:

- Rename `SWindowEntry` to a live-entry name such as `SWindowEntry` or `SOverviewWindow`.
- Rename `SWorkspaceEntry` to a live workspace/card name such as `SWorkspaceEntry` or `SOverviewWorkspace`.
- Rename helpers like `windowEntryForWindow` and `workspaceEntryForWorkspace` to entry/card terminology.
- Renamed drag state away from durable entry pointers into explicit live drag state.
- Keep this as a behavior-preserving refactor.

Exit criteria:

- No active code path describes live-rendered windows as workspaceEntries.
- Render, input, and drag/drop still use the same live geometry records.
- README and plan terminology match the code names.

## Phase B: Make drag/drop fully live-geometry based

Status: Implemented.

Goal: Keep one drag pipeline and commit drop results through the correct Hyprland semantics: exact floating placement for floating targets, layout-aware reordering for tiled targets.

Work:

- Store drag source as `SWindowDragState` with the dragged Hyprland window, source overview box, and grab offset.
- Compute drag preview from current live entry geometry, falling back only to the stored source geometry from the same live drag state.
- Map floating drop boxes from overview coordinates back to target workspace global coordinates and clamp them to the workspace.
- Commit tiled drops through layout targets: scrolling layouts move columns/rows directly, while other tiled layouts use Hyprland target switching when an anchor window is selected.
- Keep drop target feedback based on live workspace/card boxes.
- Ensure dragged floating and tiled windows preserve scale, border, and clipping behavior.
- Keep drag render ordering explicit: normal cards, normal windows, drop feedback, dragged live window, top/overlay layers.

Exit criteria:

- Dragging a tiled window between scrolling workspaces uses live content during the drag and commits through layout-aware workspace/target movement.
- Dragging a floating window preserves visible geometry and lands at the dropped overview position when the drop target has workspace-card geometry.
- Drop target feedback does not depend on retired image fields.

## Phase C: Optimize live visibility and render culling

Goal: Keep live rendering efficient without sacrificing correctness.

Work:

- Render only workspace cards that intersect the visible monitor overview region plus a small animation margin.
- Render only windows whose overview boxes intersect their workspace card clip.
- Skip hidden, unmapped, or non-renderable windows before entering expensive render helpers.
- Skip workspace layer rendering when `scrolling.show_workspace_layers = false`.
- Skip Hyprland preblur when `scrolling.background_blur = false`.
- Avoid repeated owner classification work inside one frame by caching per-frame classification where safe.
- Keep labels, text, and static annotations cached independently from live surfaces.

Exit criteria:

- Large scrolling workspaces do not render offscreen live windows every frame.
- Layer rendering cost disappears when workspace layers are disabled.
- Animated visible clients update correctly while invisible clients do not force unlimited frames.

## Phase D: Harden surface ownership and frame policy

Goal: Make live surfaces reliable without accidental wakeups or client-specific behavior.

Work:

- Keep one shared classifier for windows, layers, window popups, layer popups, and unknown surfaces.
- Route damage and frame decisions through the same owner result.
- Send frame callbacks to visible live windows and visible layer surfaces.
- Deny or defer frame callbacks for invisible overview entries.
- Keep realtime preview throttling explicit and documented.
- Add trace telemetry only where it answers ownership, visibility, or frame-policy questions.

Exit criteria:

- Layers can appear, close, and reopen while Hyprview is active without requiring unrelated damage.
- Window popups follow their parent window's overview visibility.
- Layer popups render above the overview when their layer is active.
- No launcher-specific special cases exist in the frame policy.

## Phase E: Visual correctness and lifecycle polish

Goal: Make the live path match the intended overview interaction model.

Work:

- Verify zoom-in and zoom-out endpoints use the same tape/card/window geometry as the visible overview state.
- Keep active-window indicators visually distinct from overview selection indicators.
- Keep focus-change centering and selection-change centering separate unless config explicitly couples them.
- Keep workspace switch, fullscreen transition, close transition, and overview reopen state consistent.
- Confirm floating, pinned, tiled, fullscreen, and partially offscreen windows use the same live geometry rules.
- Confirm wallpaper/backdrop scaling follows card geometry, not monitor-sized texture assumptions.

Exit criteria:

- Closing from a selected-but-not-active window zooms toward the actual active live geometry.
- Focus keybinds update selection and centering only through the intended policy path.
- Floating windows render in the correct card position and do not jump on the next frame.

## Phase F: Remove obsolete snapshot-era code and docs

Goal: Finish the architectural cleanup once the live path is stable.

Work:

- Remove unused includes, comments, telemetry, and helper names that only existed for cached workspaceEntries.
- Remove old plan entries that describe temporary fallback behavior.
- Update README to describe only live rendering behavior and current quirks.
- Update examples if config names or defaults changed.
- Refresh graphify for the Hyprview code scope after code changes.

Exit criteria:

- `rg` finds no active snapshot-era implementation names in `src/overview/scroll/` except historical docs or explicitly retired notes.
- README, example config, and this plan describe the same behavior.
- The graph no longer reports removed redraw/cache functions as central live architecture nodes after a scoped refresh.

## Validation matrix

Use this matrix after each phase that touches code.

| Area | Test |
| --- | --- |
| Open/close | Open Hyprview and close it from tiled, floating, and scrolling workspaces. |
| Keyboard navigation | Move selection left, right, up, and down across visible and offscreen scrolling entries. |
| Focus sync | Move focus with normal Hyprland binds while Hyprview is open and confirm selection/centering policy. |
| Mouse hit testing | Hover and click live windows, empty card space, and workspace gaps. |
| Mouse pan | Right-click pan normally inside cards and edge snap only near screen edges. |
| Drag/drop | Drag tiled and floating windows between workspace cards. |
| Layers | Open and close notification center, launcher, waybar-like layers, and layer popups. |
| Backdrop | Toggle workspace layers and blur and confirm wallpaper/card scaling. |
| Fullscreen | Open overview from fullscreen and enter fullscreen while overview is visible. |
| Large workspaces | Use many windows on a scrolling workspace and confirm offscreen entries do not disappear or over-render. |
| Live updates | Play video or animated content while overview is open and confirm visible clients update. |
| Reload | Unload and reload the plugin after build and confirm the first launch is visible. |

## Performance targets

| Target | Reason |
| --- | --- |
| Visible-card culling | Prevent many-window scrolling workspaces from scaling linearly with hidden content. |
| Visible-window culling | Avoid rendering live surfaces that are clipped out of the card viewport. |
| Layer config gate | Avoid layer traversal when users disable workspace layer rendering. |
| Hyprland preblur only | Reuse compositor blur instead of adding per-frame custom blur cost. |
| Label texture cache | Keep static text cheap without caching live surfaces. |
| Frame callback throttling | Let visible clients update without letting one animated client force unlimited overview frames. |
| Scoped damage | Damage the overview monitor/card regions needed for live updates instead of waking unrelated monitors. |

## Suggested commit slicing

| Slice | Scope |
| --- | --- |
| `rename-live-entries` | Behavior-preserving rename from image terminology to live-entry terminology. |
| `live-drag-geometry` | Drag/drop state and preview based on live geometry records. |
| `live-render-culling` | Visible workspace/window/layer culling and frame policy cleanup. |
| `live-lifecycle-polish` | Focus, close animation, fullscreen, and workspace-switch lifecycle fixes. |
| `live-docs-cleanup` | README, example config, and plan cleanup after code names settle. |

## Anti-goals

- Do not reintroduce cached window framebuffer rendering.
- Do not add a user-facing config that switches back to the retired model.
- Do not create a parallel compositor pipeline inside Hyprview.
- Do not solve live layer or launcher behavior with client-specific hacks.
- Do not keep image-era names once the rename phase starts.
- Do not optimize for old thumbnail behavior when it conflicts with live surface correctness.
