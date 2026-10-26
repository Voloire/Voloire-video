# Panel screen bounds and scroll reachability, version 1.4.3

The panel only constrained its preferred size at construction, using the primary monitor.
Subsequent resizing and reopening could leave its bottom outside the current work area.
Scrolling inside such a window cannot make controls below the screen reachable. Tab
content layouts also need explicit minimum/maximum size propagation as wrapped labels
and controls change their height.

## Implementation

- Bound the actual panel to its current screen's logical work area, accounting for Windows
  scaling and the taskbar. Apply the bounds at construction, show and resize, and when the
  screen or its available geometry changes. Keep normal resizing within those limits.
- Propagate all three tab body layout constraints to their scroll areas so their scroll
  ranges include the full content, including the final Share controls. Allow a horizontal
  scrollbar only when the window is too narrow for a card, instead of clipping its buttons.
- Preserve the destination header above the scrolling tabs.

## Validation and delivery

Use only functional checks: offscreen widgets with capture and device access disabled.
The regression requests compact and oversized windows, changes all tabs and both themes,
scrolls using wheel events, checks the final buttons are inside the viewport, and reopens
an offscreen panel. Run it at 100%, 125%, 150% and 200% scaling. Do not run audio/video
tests, capture, playback, encoding or broadcasting.

Build/package 1.4.3, commit and push an isolated branch, open a pull request and merge
after configured checks pass. Replace the installed version through MSI using the existing
authorization, retaining all current settings, encrypted credentials and Windows autostart.
Restart minimized and verify idle/account state and installed file hashes. Do not change
the user's current stream settings. Delivery evidence is tracked in the pull request.

## Results

The original regression failed three work-area containment checks. Release build now
passes, and the full offscreen functional UI suite passes 4 cases / 330 assertions.
Additional scaled runs pass 129 assertions at 125%, 129 at 150% and 141 at 200%, including
wheel scrolling and access to rightmost buttons when a horizontal scrollbar is needed.
Dark/light scroll-bottom proofs were inspected. No audio/video tests were run.
