# Visible streaming destination, version 1.4.1

The generic Go live action did not identify the platform, and opening a stream tab did not
select its destination. A channel label alone on the desktop HUD was insufficient to tell
YouTube and Twitch apart.

## Scope

- Share one destination presentation between the panel, desktop HUD, tray tooltip and menus.
- Keep an explicit destination selector above every tab. It changes the real output and
  profile together and identifies Twitch/YouTube with names and vector social icons.
- Name both the go-live and end-stream actions by service. Keep local recording explicit.
- Keep the real destination visible while idle, starting, live and reconnecting. Lock editing
  while starting or recording. Browsing tabs does not reroute output.
- Add MSI opt-in `AUTOSTART=1`, using the existing per-user tray-only startup convention.
- Preserve existing accounts, encrypted keys, recording preferences and channel metadata.

## Validation and delivery

Only functional checks are authorized. No audio/video tests, capture, encoders, broadcast
connections or live channel edits. Run the functional unit suite, fake-API suite and offscreen
widget checks. Inspect app/HUD rendering for Twitch, YouTube and local output in both themes.

Build and package 1.4.1. Commit and push this isolated branch, create a pull request, and merge
once configured checks pass. Stop the installed app cleanly, uninstall 1.4.0, install the new
MSI with `AUTOSTART=1`, launch it and verify the version, preserved sign-in/key state, destination
indicators, idle status and per-user startup command. Do not start any stream or recording.

## Progress

- Implemented the shared destination, always-visible selector, social icons and service-named
  actions in the app, HUD, tray tooltip and shared control menus.
- Added the MSI autostart component, opt-in with `AUTOSTART=1` and preserved on upgrades.
- Functional validation passed: 16 unit cases / 106 assertions, 9 fake-API flows, and 2
  offscreen widget cases / 171 assertions. The widget checks cover routing configuration,
  selector text, tab independence, app/HUD/menu agreement, both themes and 420/620 px widths.
- Reviewed the rendered app and HUD for Twitch, YouTube and local output. No media tests or
  broadcasts were run.
- Release packaging, pull request and local MSI replacement are the remaining delivery steps.
