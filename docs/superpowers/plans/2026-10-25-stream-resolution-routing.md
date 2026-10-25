# Stream resolution routing, version 1.4.2

The desktop menu selected `outHeight`, which only affects local recording. A Twitch stream
therefore retained the YouTube native/16000 kbps settings while the menu displayed 1080p.
The installed application's event logs confirmed 2560x1080 at 60 fps and 16000 kbps.

## Fix

- Route the shared resolution menu to `streamHeight` while output is streaming and to
  `outHeight` while recording. Read the same field for the checked menu item.
- Make named stream resolutions exact 16:9 canvases: 1080p is 1920x1080. Fit the entire
  source inside the canvas with black bars; never stretch or crop ultrawide content.
  Native size remains an explicit choice. Local recording scaling is unchanged.
- Show real streaming resolution, fps, CBR bitrate and H.264 in the app header and desktop
  HUD. Put mirrored stream controls directly in the Twitch tab.
- Use the standard 6000 kbps ceiling for this application's Twitch H.264 stream, replacing
  stale YouTube settings. Disable recording-only quality and codec choices while streaming.

## Authorized validation and delivery

Only functional checks: test menu routing, persistence, control synchronization, destination
changes and geometry arithmetic. Offscreen widget rendering cannot access devices or start
capture. Do not run audio/video tests, capture, encoding, playback or broadcasting.

Build and package 1.4.2. Commit and push an isolated branch, open a pull request, merge after
configured checks pass. Stop and uninstall the old installed version, install and start the
new MSI minimized, retaining Windows autostart and encrypted credentials. Set the reported
1080p Twitch choice to streamHeight=1080 and bitrate=6000 during local replacement, after
backing up settings. Verify installed hashes, safe account/key state and idle status through
the application control API without taking desktop focus.

## Status

Implementation is complete. Release build passed. Functional core checks passed (17 cases,
171 assertions), offscreen UI checks passed (3 cases, 201 assertions), and all 9 fake API
flows passed. Dark and light app/HUD proofs were inspected. No audio/video tests, capture,
encoding, playback or broadcasts were run. This validates routing and canvas arithmetic;
it does not establish the playback behavior of existing Twitch VODs or their thumbnails.

MSI replacement and remote checks are tracked in the pull request.
