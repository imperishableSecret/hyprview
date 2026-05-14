# Hyprview Workspace Sync Regression Fix Plan

## Current Root Cause

The new regressions come from the final workspace-navigation cleanup, plus one close-path change from the previous phase.

1. Selection navigation stopped activating Hyprland workspaces.
   - `moveViewportWorkspace()` now calls `setViewportWorkspace(..., activate=false)`.
   - Wrapped vertical keyboard selection does the same in `moveSelection()`.
   - This makes the overview selection move to another workspace while Hyprland remains on the old active workspace, so users cannot reliably organize windows with normal Hyprland keybinds from the selected workspace.

2. External Hyprland workspace switches are being warped in Hyprview.
   - `onWorkspaceActive()` queues refreshes with `warpViewport=true`.
   - `refreshWorkspaceEntries()` treats active-workspace refreshes as `ANCHOR_TO_ACTIVE` and immediately rewrites `startedOn`, `viewportCurrentWorkspace`, and `viewOffset`.
   - That removes the expected Hyprview workspace animation and makes Hyprland keybind workspace switches feel instant while the overview is open.

3. The close animation can be anchored to the wrong geometry.
   - `close()` changes `startedOn` to the final workspace without converting the existing `viewOffset` into the new anchor space.
   - The previous pan reset was removed, so `workspaceContentPan` can still offset live tiles even when scale reaches `1.0`.
   - Result: the zoom-out frame can render a fake overview tape position, then the final native frame reveals the real scrolling tape.

## Fix Phases

### Phase 1: Restore Workspace Activation From Selection

Files:

- `src/overview/scroll/ScrollOverviewLayout.cpp`
- `src/overview/scroll/ScrollOverviewSelection.cpp`

Changes:

- Restore `activate=true` for vertical viewport/selection movement.
- Keep activation through `activateWorkspace()`, which uses Hyprland's normal non-internal workspace switch path.
- Keep horizontal selection inside the current workspace unchanged.

Expected result:

- Moving the overview selection up/down switches the real Hyprland workspace again.
- The selected workspace is usable for normal keybind-based organization while Hyprview stays open.

### Phase 2: Animate Active Workspace Sync Instead Of Warping

Files:

- `src/overview/scroll/ScrollOverviewLifecycle.cpp`
- `src/overview/scroll/ScrollOverviewState.cpp`

Changes:

- Make `onWorkspaceActive()` queue an animated refresh, not an instant warp.
- Remove the `ANCHOR_TO_ACTIVE` immediate re-anchor branch.
- Keep anchor normalization after the view animation finishes, so empty active workspaces settle with `viewOffset = 0` after the visible transition.
- Restore queued refresh merge behavior so any non-warp request keeps the batch animated.

Expected result:

- Hyprland workspace keybind changes animate inside Hyprview.
- Empty workspace switches no longer require an instant overview warp to settle correctly.

### Phase 3: Re-anchor Close Geometry And Reset Fake Tape Pan

Files:

- `src/overview/scroll/ScrollOverview.hpp`
- `src/overview/scroll/ScrollOverviewLayout.cpp`
- `src/overview/scroll/ScrollOverviewLifecycle.cpp`

Changes:

- Add a small helper to re-anchor `startedOn` while preserving the current visual `viewOffset`.
- Use that helper in `close()` before animating `viewOffset` back to zero.
- Switch the target workspace through the normal activation helper during close-to-selection.
- Reset the final workspace overview content pan to `0` during close, so the final live overview frame matches the real scrolling tape.

Expected result:

- Closing to a selected window or workspace zooms toward the same position that will be visible after Hyprview exits.
- No last-frame reveal of a different real scrolling tape state.

## Validation

Run after each code phase:

- `make format-fix`
- `make all`
- `graphify update .`
- Commit only the phase files.

After the final phase:

- Reload the plugin with `hyprctl plugin unload /home/imperishablesecret/projs/hyprview/hyprview.so`
- Reload the plugin with `hyprctl plugin load /home/imperishablesecret/projs/hyprview/hyprview.so`

Manual checks:

- Move selection up/down inside Hyprview and confirm the real Hyprland workspace switches.
- Use normal Hyprland workspace keybinds while Hyprview is open and confirm the overview animates.
- Switch to an empty workspace while Hyprview is open and confirm it settles centered.
- Click a window on a non-active workspace and confirm Hyprview exits to that workspace.
- Close to a selected scrolling-layout window and confirm no last-frame tape-position reveal.
