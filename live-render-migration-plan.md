# Hyprview Live Rendering Migration Plan

## Decision

Migrate Hyprview from a snapshot-first overview renderer to a live compositor-surface renderer.

Snapshots should stop being the primary model. They can remain temporarily as a fallback while the migration lands, but the target architecture should render real Hyprland windows, layers, popups, decorations, and frame callbacks through an overview-aware render path.

The reference project proves that this model is viable. We should not copy it as one monolithic file. We should port the concepts into Hyprview's split architecture and improve the design where the split gives us cleaner ownership.

## Why this migration is needed

The current snapshot model creates a second visual truth:

- Real Hyprland windows and layer surfaces exist in the compositor.
- Hyprview stores cached framebuffer images for windows and workspaces.
- Hyprview then tries to keep those images in sync with damage, focus, selection, fullscreen state, pointer state, and workspace movement.

That duplication is the source of multiple bugs:

- Layer surfaces can be visible only when unrelated damage wakes the compositor.
- Launchers and notification panels need real surface frame callbacks, not just monitor damage.
- HDR and color behavior can diverge from Hyprland's real render path.
- Window drag, drop, and resize require fake image previews and reconciliation.
- Fullscreen behavior needs repeated special cases because the snapshot is not the compositor truth.
- Dirty image tracking can miss state that is not a normal window damage rectangle.

Live rendering moves Hyprview back to a single truth:

- Hyprland surfaces stay real.
- Hyprview computes overview geometry.
- Hyprview asks Hyprland's renderer to draw those surfaces into overview geometry.
- Hyprview owns only overview transforms, frame policy, and interaction state.

## Target architecture

The final architecture should split responsibilities like this:

| Area | Target owner | Purpose |
| --- | --- | --- |
| Hook integration | `src/plugin/PluginHooks.cpp` | Hook Hyprland render, surface damage, frame scheduling, and frame callback paths. |
| Overview interface | `src/overview/IOverview.hpp` | Expose source-aware surface/frame methods without leaking scroll-specific details. |
| Render orchestration | `src/overview/scroll/ScrollOverviewLifecycle.cpp` | Add the overview pass and render top-level layers at the right phase. |
| Live window rendering | New `src/overview/scroll/ScrollOverviewLiveRender.cpp` | Render visible windows, floating windows, dragged windows, decorations, shadows, and blur using live surfaces. |
| Surface/frame policy | New `src/overview/scroll/ScrollOverviewSurface.cpp` | Decide which surface damage and frame callbacks belong to overview rendering. |
| Layer handling | New `src/overview/scroll/ScrollOverviewLayers.cpp` | Render background, bottom, top, overlay, and layer popups consistently. |
| Snapshot compatibility | Existing cache files, then reduced | Keep temporary fallback and remove once live rendering is feature complete. |
| Geometry model | Existing geometry/layout files | Continue computing overview boxes, hitboxes, workspace tape position, and selection. |
| Interaction model | Existing interaction files | Keep keyboard, mouse, drag, drop, pan, and resize behavior, but operate on live windows instead of image proxies. |

## Core design principles

- Do not add launcher-specific behavior.
- Do not add delayed redraws or forced damage loops as fixes.
- Treat `CWLSurfaceResource` ownership as the source of truth for layer/window/popup behavior.
- Keep frame callbacks overview-aware so clients keep producing frames while transformed in the overview.
- Render only visible overview content where possible.
- Keep Hyprview's modular structure instead of importing the reference as one large file.
- Prefer explicit owner classification over broad heuristics.
- Keep snapshot code only as an interim compatibility path.
- Avoid behavior that depends on accidental compositor wakeups from unrelated damage.

## Migration phases

## Phase 0: Stabilize the current branch before migration

Goal: Make sure the migration starts from a known state and does not mix unrelated fixes.

Work:

- Keep the current P5 layer visibility work separate from the live-render migration.
- Revert or demote the broad `onDamageReported(const CRegion&)` change if it is no longer needed after surface-aware hooks land.
- Preserve the fullscreen visibility fix that resets `m_solitaryClient`, unless live rendering makes a cleaner equivalent obvious.
- Do not change README feature claims until live rendering is actually working.

Exit criteria:

- Current code builds.
- Current behavior baseline is documented.
- Live-render migration starts in a dedicated branch.

## Phase 1: Introduce source-aware hook plumbing

Goal: Let Hyprview see real surface damage and frame lifecycle events before they collapse into anonymous monitor damage.

Files:

- `src/plugin/PluginHooks.cpp`
- `src/overview/IOverview.hpp`
- `src/overview/scroll/ScrollOverview.hpp`
- New `src/overview/scroll/ScrollOverviewSurface.cpp`

Hooks to add:

| Hook | Why |
| --- | --- |
| `IHyprRenderer::damageSurface` | Needed to classify whether damage belongs to a layer, window, popup, or irrelevant monitor. |
| `IHyprRenderer::sendFrameEventsToWorkspace` | Needed to suppress normal workspace frame callbacks while overview renders transformed content. |
| `CWLSurfaceResource::frame` | Needed to decide whether a surface should receive a frame callback now or be scheduled for overview rendering. |
| `CCompositor::scheduleFrameForMonitor` | Needed to throttle or allow frames for realtime preview without fighting Hyprland's scheduler. |
| Existing `renderWorkspace` | Keep as the overview render injection point initially. |
| Existing `CMonitor::addDamage` | Keep temporarily, but it should no longer be the primary surface update mechanism. |

Interface methods to add:

```cpp
virtual bool shouldHandleSurfaceDamage(SP<CWLSurfaceResource> surface);
virtual bool shouldAllowSurfaceFrame(SP<CWLSurfaceResource> surface, const Time::steady_tp& now);
virtual bool shouldAllowRealtimePreviewSchedule();
virtual bool shouldSuppressRenderDamage() const;
```

Hyprview-specific improvement over the reference:

- Keep hook target discovery through the existing checked demangled matching helper instead of raw first-match lookup.
- Add narrow failure messages per hook so API drift is diagnosable.
- Keep hook functions small and delegate policy into overview methods.

Exit criteria:

- Layer surface damage can be classified as layer damage.
- Window surface damage can be classified as visible overview window damage or ignored offscreen damage.
- Normal workspace frame callbacks are suppressible while overview is active.
- The code builds before changing the render model.

## Phase 2: Add surface ownership classification

Goal: Convert `CWLSurfaceResource` into an explicit owner classification.

New concept:

```cpp
enum class eOverviewSurfaceOwner {
    UNKNOWN,
    WINDOW,
    LAYER,
    WINDOW_POPUP,
    LAYER_POPUP,
};

struct SOverviewSurfaceOwner {
    eOverviewSurfaceOwner type;
    PHLWINDOW window;
    PHLLS layer;
    PHLMONITOR monitor;
};
```

Classification rules:

- Convert `CWLSurfaceResource` to `Desktop::View::CWLSurface`.
- From the view, detect `Desktop::View::CWindow`.
- From the view, detect `Desktop::View::CLayerSurface`.
- If neither exists, detect `Desktop::View::CPopup` and resolve its top-level owner.
- Treat layer popups as layer-owned.
- Treat window popups as window-owned.
- Reject surfaces on monitors not owned by the active overview.

Hyprview-specific improvement over the reference:

- Put this in one helper instead of duplicating owner resolution in both damage and frame paths.
- Return a typed result instead of repeating loosely coupled checks.
- Make tests/manual debugging easier by logging owner type in trace mode if needed.

Exit criteria:

- `shouldHandleSurfaceDamage()` and `shouldAllowSurfaceFrame()` share the same classifier.
- Top/overlay layer popups are treated as blocking layer input and rendered above overview.
- Window popups are tied to their parent window's overview visibility.

## Phase 3: Port overview-aware frame callback delivery

Goal: Replace normal workspace frame callback delivery with overview-aware delivery while Hyprview is active.

Files:

- `src/overview/scroll/ScrollOverviewSurface.cpp`
- `src/overview/scroll/ScrollOverviewLifecycle.cpp`
- `src/overview/scroll/ScrollOverview.hpp`

New methods:

```cpp
void sendOverviewFrameCallbacks(const Time::steady_tp& now);
bool surfaceTreeHasFrameCallbacks(SP<CWLSurfaceResource> surface) const;
void surfaceTreePresent(SP<CWLSurfaceResource> surface, PHLMONITOR monitor, const Time::steady_tp& now);
bool shouldAllowRealtimePreviewFrame() const;
void scheduleRealtimePreviewFrame();
```

Frame callback policy:

- Always allow layer surfaces that are mapped and belong to the overview monitor.
- Allow visible overview windows.
- Allow pinned floating windows if they are visible in overview geometry.
- Deny invisible windows and schedule an overview frame if they need one later.
- Deny non-active fullscreen-covered tiled windows when the fullscreen window is the only visible overview content for that workspace.
- Keep realtime preview throttling to prevent animated clients from forcing unlimited frames.

Hyprview-specific improvement over the reference:

- Start with conservative frame delivery for layers and selected/visible windows.
- Add throttling as an explicit policy object or helper constants instead of scattered static state.
- Keep the first pass correctness-biased, then optimize after user testing.

Exit criteria:

- Reopening `hyprlauncher` inside Hyprview works repeatedly without `swaync` or workspace switching.
- Layer clients continue animating or updating while visible.
- Invisible overview windows do not receive unlimited frame callbacks.

## Phase 4: Create the live render pipeline

Goal: Render live windows and layers into overview geometry instead of drawing cached snapshots.

Files:

- New `src/overview/scroll/ScrollOverviewLiveRender.cpp`
- New `src/overview/scroll/ScrollOverviewLayers.cpp`
- `src/overview/scroll/ScrollOverviewRender.cpp`
- `src/overview/scroll/ScrollOverviewLifecycle.cpp`
- `src/overview/OverviewPassElement.cpp`

Render order:

| Step | Content |
| --- | --- |
| 1 | Implemented: clear to `backdrop_col`, render live `BACKGROUND` layers, and optionally blur through Hyprland `preBlurForCurrentMonitor` plus monitor `m_blurFB`. |
| 2 | Implemented: render background layer surfaces inside workspace cards when `scrolling.show_workspace_layers = true`. |
| 3 | Implemented: workspace shadows, cards, indicators, and annotations render from live overview geometry. |
| 4 | Implemented: render bottom layer surfaces inside workspace cards when `scrolling.show_workspace_layers = true`. |
| 5 | Implemented: render live tiled and floating windows into overview boxes. |
| 6 | Implemented: render overview decorations, borders, active indicators, labels, and drop targets. |
| 7 | Implemented: render dragged window above normal windows. |
| 8 | Implemented: render pinned floating windows above workspace cards if applicable. |
| 9 | Implemented: render top and overlay layer surfaces above the overview. |
| 10 | Implemented: send overview frame callbacks. |

Live rendering primitives:

```cpp
void renderOverviewLive(PHLMONITOR monitor, const Time::steady_tp& now);
void renderWorkspaceLive(PHLMONITOR monitor, const SWorkspaceImage& workspace, const CBox& workspaceBox, float scale, const Time::steady_tp& now);
void renderWindowLive(PHLMONITOR monitor, PHLWINDOW window, const CBox& overviewBox, float scale, const Time::steady_tp& now);
void renderLayerLevel(PHLMONITOR monitor, uint32_t layer, const CBox* workspaceBox, float scale, const Time::steady_tp& now);
```

Important renderer behavior:

- Use Hyprland renderer hints or equivalent transform state to translate and scale real surfaces.
- Block surface feedback while rendering transformed overview content.
- Restore renderer state with scope guards.
- Never leave render modifications active after one window or layer.
- Use mapped checks before rendering layer surfaces.
- Keep the render pass element as the injection mechanism initially.

Hyprview-specific improvement over the reference:

- Keep per-area render helpers in separate files.
- Avoid combining wallpaper, blur, live windows, frame callbacks, and interaction in one large function.
- Keep render order explicit and documented in code.

Exit criteria:

- Overview can display live windows and configured workspace-card layers without snapshot textures in the live path.
- Live backdrop uses background layers and Hyprland's monitor blur resource instead of the old captured backdrop texture.
- Top/overlay layers render above overview.
- Basic selection, hover, and close animations still work.

Remaining deferred cleanup:

- Keep the old snapshot/background framebuffer path until the full live render migration is complete, then remove it in one scoped cleanup.

## Phase 5: Migrate window geometry from image boxes to live boxes

Goal: Reuse the existing geometry model, but stop treating cached images as the render source.

Current concepts to preserve:

- Workspace ordering.
- Workspace tape offset.
- Horizontal content pan.
- Window hitboxes.
- Drop target geometry.
- Selection memory.
- Active indicator geometry.
- Keyboard navigation geometry.

Concepts to replace:

- `SWindowImage` should become a geometry/render record, not a cached texture owner.
- `SWorkspaceImage` should become `SWorkspaceOverview` or equivalent after the migration.
- `imageForWindow()` can become `overviewWindowForWindow()` or `windowEntryForWindow()`.
- `redrawWindowImage()` should disappear from the live path.

Suggested transition:

- First keep the current type names to reduce churn.
- Add fields for live geometry and visibility.
- Stop using framebuffer texture fields in live render mode.
- Rename the types only after behavior is stable.

Hyprview-specific improvement over the reference:

- Keep a strong separation between layout data and render implementation.
- The geometry cache should not know whether the renderer is snapshot or live.
- Hit testing should use the same live overview boxes that rendering uses.

Exit criteria:

- Keyboard navigation selects the same window that live rendering draws.
- Mouse hit testing matches live-rendered window positions.
- Drag and drop targets use live geometry.

## Phase 6: Port live drag, drop, and resize behavior

Goal: Make overview manipulation operate on real windows instead of fake thumbnails.

Behavior to preserve:

- Right-click drag viewport pan.
- Mouse drag window between workspaces.
- Drop target feedback.
- Keyboard selection movement.
- Keyboard and mouse takeover behavior.
- Workspace switching through overview.

Behavior to improve:

- Drag preview should render the real window at the dragged overview box.
- Resize preview should render the real window at the resized overview box.
- Drop should only commit once the interaction completes.
- Pointer hitboxes should come from live overview geometry.

Implementation approach:

- Keep current interaction state machine.
- Replace drag preview snapshot drawing with live `renderDraggedWindow()`.
- Store temporary overview box for dragged or resized window.
- Commit real Hyprland move/resize only on release.
- If Hyprland needs live resize during preview, gate that separately after the first stable pass.

Exit criteria:

- Dragging a window does not show stale thumbnail content.
- Resizing a window preview does not require recapturing texture images.
- Dropping between workspaces still updates workspace contents correctly.

## Phase 7: Fullscreen and workspace lifecycle

Goal: Ensure live rendering handles fullscreen windows without compositor state desync.

Current known state:

- Hyprview currently resets `m_solitaryClient` so overview remains visible above fullscreen.
- Scrolling workspaces own their own fullscreen workspace state.
- The plugin should not fake scrolling-layout fullscreen internals.

Live model policy:

- Keep overview visible over fullscreen by preventing solitary fullscreen optimization while overview is active.
- Render fullscreen windows as overview windows where appropriate.
- If a workspace has a fullscreen tiled window, do not render hidden tiled windows unless floating visibility rules require it.
- Top/overlay layers should remain visible above fullscreen overview rendering.
- On close, restore normal Hyprland rendering without leaving hidden or forced-visible state behind.

Hyprview-specific improvement over the reference:

- Keep fullscreen visibility policy in a dedicated helper instead of mixing it into all render paths.
- Do not force scrolling workspace internals unless Hyprland itself exposes the required state.

Exit criteria:

- Opening overview over fullscreen works.
- Entering fullscreen while overview is open does not hide the overview.
- Closing overview restores normal fullscreen behavior.

## Phase 8: Blur, shadows, decorations, and HDR-sensitive rendering

Goal: Preserve Hyprview's visual features while relying on Hyprland's real render path.

Visual features to preserve:

- Window borders.
- Active window indicator.
- Selection indicator.
- Drop target feedback.
- Workspace card backgrounds.
- Labels.
- Drag preview styling.

Visual features to reconsider:

- Cached blur framebuffers.
- Snapshot-based workspace shadows.
- Precomputed blur over cached windows.

Live model policy:

- Prefer Hyprland's own decoration and surface render behavior.
- Keep custom overview indicators as overlay pass elements.
- Only use precomputed blur if it is tied to overview background, not stale window screenshots.
- Avoid converting HDR surfaces into SDR snapshot textures.

Hyprview-specific improvement over the reference:

- Make visual effects optional and local to overview composition.
- Keep correctness first, then tune blur/shadow performance.

Exit criteria:

- Normal windows, HDR windows, and layer surfaces render through the same live path.
- Overview-specific indicators remain visible and distinguishable.
- No stale afterimage appears when clients update.

## Phase 9: Demote and remove snapshot cache code

Goal: Delete the old duplicate-rendering model once live rendering reaches parity.

Candidate code to remove or isolate:

- Window image framebuffer capture.
- Workspace image framebuffer capture.
- Dirty thumbnail recapture.
- Damage-region mapping to cached image boxes.
- Snapshot-specific render pass flushing.
- Fake drag preview texture drawing.

Safe migration sequence:

- Keep snapshot fallback behind an internal compile-time or config-disabled path for one short phase.
- Do not expose snapshot mode as a long-term public feature unless there is a clear low-power use case.
- Remove dead cache fields after live path passes manual testing.
- Rename remaining data types from image terminology to overview-entry terminology.

Exit criteria:

- No core interaction depends on snapshot textures.
- Dirty image state is gone from the live path.
- The source tree names match the live architecture.

## Phase 10: Config and README update

Goal: Update public behavior only after implementation is real.

Config policy:

- Do not add a user-facing `render_mode` until there is a maintained second mode.
- If snapshot fallback is temporary, keep it internal.
- Keep existing Lua config semantics stable where possible.
- Add config only for real user-facing behavior, not migration toggles.

README updates after completion:

- Hyprview renders a live Niri-like scrolling overview.
- Layer-shell launchers and overlays are supported.
- Normal Hyprland keybinds continue to work unless keyboard grab overrides them.
- Drag, drop, resize, selection, and focus behavior use live window state.
- Snapshot limitations should not be documented as features once removed.

Exit criteria:

- README matches code.
- Example Lua config remains accurate.
- No docs claim behavior before it exists.

## Validation plan

Manual validation should happen at the end of each phase, not only at the end of the migration.

Always run before asking for testing:

```sh
make format-fix && make all && hyprctl plugin unload /home/imperishablesecret/projs/hyprview/hyprview.so && hyprctl plugin load /home/imperishablesecret/projs/hyprview/hyprview.so
```

Layer validation:

- Open Hyprview.
- Launch `hyprlauncher`.
- Press Escape.
- Launch `hyprlauncher` again without opening any other layer.
- Repeat several times.
- Open and close `swaync`.
- Verify both layer clients remain visible and interactive.

Frame callback validation:

- Use an animated or updating launcher surface if available.
- Verify animation/update continues while visible in overview.
- Move the pointer over the layer and verify pointer input reaches the layer when expected.

Window validation:

- Open multiple tiled windows.
- Open floating windows.
- Open windows with popups or menus.
- Verify overview render matches real window state.
- Verify keyboard selection and mouse selection match visible live geometry.

Interaction validation:

- Drag a window between workspaces.
- Resize a window in overview if supported.
- Drop a window and verify workspace contents update.
- Use keyboard movement while mouse is idle.
- Use mouse movement after keyboard takeover.

Fullscreen validation:

- Open overview over fullscreen.
- Enter fullscreen while overview is open.
- Exit overview from fullscreen.
- Verify normal Hyprland fullscreen behavior returns.

HDR/color validation:

- Open HDR or color-sensitive content.
- Compare normal workspace rendering and overview rendering.
- Verify no obvious SDR snapshot flattening or stale color frame appears.

Exit animation validation:

- Select a non-active window.
- Exit without activating selection.
- Verify zoom returns to the active workspace/window state smoothly.
- Exit with activating selection.
- Verify target window focus and animation align.

Regression validation:

- Existing Lua dispatcher binds still work.
- Keyboard grab still works.
- Non-overridden Hyprland keybinds still work.
- Gesture open and close still work.
- Plugin unload does not leave windows hidden or layers invisible.

## Risk assessment

| Risk | Impact | Mitigation |
| --- | --- | --- |
| Hyprland internal hook drift | Plugin fails to load after Hyprland updates. | Use checked demangled matching and clear failure notifications. |
| Live rendering too expensive with many animated windows | Overview can stutter. | Add visible-window filtering and realtime frame throttling. |
| Surface feedback mishandled | Clients may over-render or under-render. | Block feedback during transformed render and send explicit overview frame callbacks. |
| Layer popups misclassified | Launcher menus or popups can disappear. | Centralize owner classification and resolve popup top-level owners. |
| Fullscreen state desync | Overview can disappear or leave windows hidden. | Keep fullscreen policy dedicated and restore state on close/destructor. |
| Drag/drop behavior regressions | Window movement can commit incorrectly. | Keep current interaction state machine and swap render source first. |
| Large refactor scope | Hard to debug if all changes land together. | Implement phase by phase with user testing after each major phase. |

## Suggested implementation order

1. Branch: `feature/live-render-overview`.
2. Add hook plumbing and overview interface methods.
3. Add centralized surface owner classification.
4. Port overview-aware frame callback delivery.
5. Fix launcher relaunch bug through the proper surface/frame path.
6. Add live rendering for top/overlay layers with mapped checks and feedback blocking.
7. Add live window rendering while keeping snapshot fallback.
8. Switch default rendering to live windows.
9. Port drag/drop/resize preview to live windows.
10. Migrate fullscreen and visual effects into the live path.
11. Remove snapshot cache code.
12. Update README and example config.
13. Run scoped graph update for changed code paths.
14. Commit in reviewable chunks, signed, with behavior-specific messages.

## Commit slicing

Recommended commits:

| Commit | Scope |
| --- | --- |
| `hooks` | Add surface/frame hook plumbing and interface methods. |
| `surface-policy` | Add owner classification and surface damage policy. |
| `frame-callbacks` | Add overview-aware frame callback delivery. |
| `layer-live-render` | Render layers through live path and fix launcher lifecycle. |
| `window-live-render` | Render visible windows live with snapshot fallback. |
| `interaction-live-render` | Move drag/drop/resize previews to live windows. |
| `fullscreen-live-render` | Consolidate fullscreen handling for live overview. |
| `remove-snapshots` | Delete or isolate snapshot cache code after parity. |
| `docs` | Update README and example Lua config after behavior is complete. |

## Final target

Hyprview should become a live transformed view of Hyprland's compositor state.

The overview should own:

- Workspace tape geometry.
- Overview transforms.
- Selection and focus policy.
- Input overrides.
- Surface/frame scheduling while overview is active.
- Overview-only decorations and indicators.

Hyprland should remain the source of truth for:

- Window contents.
- Layer contents.
- Popups and subsurfaces.
- Color and HDR rendering.
- Surface damage.
- Frame callback semantics.

That division is cleaner than snapshots and better matches the feature direction of this project.
