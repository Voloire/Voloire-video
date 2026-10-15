# Voloire-video

Windows recorder for one window or application, with the application's own audio and a microphone on separate tracks. Tray icon with live state and timer, cyberpunk panel in black with neon blue accents.

## What it does

- **Source**: any window, windowed or borderless fullscreen (Windows Graphics Capture), a whole monitor, or a monitor through DXGI Desktop Duplication for exclusive fullscreen.
- **Two audio channels** in one file. With both on, track 1 is `MIX` (SYS + MIC, the default, so any player plays both), then `SYS` and `MIC` separately for editing:
  - `SYS`: only the audio of the selected application and its child processes (WASAPI process loopback), or the full output of a chosen device, or off.
  - `MIC`: the default microphone, any chosen capture device, or off.
- **Format**: MKV (crash safe) or MP4, H.264, HEVC or AV1, NVENC, AMF, Quick Sync or Media Foundation (auto picks the first that opens), source size or 480p to 2160p, 24 to 120 fps, bitrate.
- **Pause and resume** without a hole in the file: the pause is cut out of the timeline.
- **Tray icon**: ring and dot that pulse while recording and show a pause glyph when paused; tooltip and menu header carry the state and timer. Start, stop, pause, window, format, MIC and SYS devices are all in its menu.
- **Constant frame rate**, A/V kept on one clock; silent loopback is padded with silence and slow device drift is trimmed, so audio never wanders from the picture.

Requires Windows 10 version 2004 (build 19041) or later, x64. App audio capture needs that version; exclusive-fullscreen titles with DRM-protected audio cannot be captured by any user-mode recorder.

## Install

Download one of the installers from the latest release (both install per user, no administrator rights needed):

- `voloire-video-setup-<version>.exe`
- `voloire-video-<version>.msi`

Silent: `voloire-video-setup-1.0.0.exe /VERYSILENT` or `msiexec /i voloire-video-1.0.0.msi /qn`.

## Use

Start Voloire-video, pick the window (or leave "active window at record time"), check the two audio channels, press REC. The panel hides to the tray with the close button; the tray icon menu does everything the panel does. Autostart with `--minimized` keeps it tray only.

Headless: `vvcli record --window-title "My Game" --sys app --mic default --duration 60 --out D:\clips`. `vvcli list windows|monitors|devices` shows what can be selected.

## Build

Needs Visual Studio 2019 or 2022 build tools, CMake, Python 3 with numpy and (for installers) Inno Setup 6. Everything else is fetched:

```powershell
scripts\setup-deps.ps1     # FFmpeg (LGPL), Qt 6.5, C++/WinRT, WiX 3.14, into %USERPROFILE%\deps
scripts\build.ps1          # Release build into build\bin
scripts\package.ps1        # both installers into dist\
```

## Test and observability

The harness exercises the real binaries from the outside and judges the produced media; it never trusts the app's own word.

| Layer | What | Run |
|---|---|---|
| Unit | ring buffer, config, telemetry, control pipe, recorder with synthetic sources | `build\bin\vv_unit_tests.exe` |
| End to end | 12 scenarios, below | `python tests\e2e\run_e2e.py [--only a,b] [--fast]` |
| Installers | silent install, run, uninstall of both | `python tests\e2e\run_e2e.py --only installers` |

Scenarios: synthetic basic (tones, flashes, A/V offset, counter continuity), pause and resume over the pipe, format matrix (MKV/MP4 x H.264/HEVC/AV1 x sizes x rates x encoders), per-app audio isolation (a second process plays a different tone and must not leak into `SYS`, while a system-loopback control run must contain it), borderless window, scaled output, monitor and DXGI capture, microphone devices, window closed mid-recording, error handling, tray UI driven through its own menu with screenshots and colour checks, and a 60 s soak for drift.

Every component reports through one telemetry layer:

- `events.jsonl`: structured log of every state change, source start, encoder choice, FFmpeg warning and error, with microsecond timestamps.
- `metrics.jsonl`: all metrics once a second: `video.fps_actual`, `video.frames`, `video.repeated_frames`, `video.dropped_ticks`, `video.encode_ms`, `video.queue_lag_ms`, `rec.size_bytes`, and per track `audio.sys|mic.rms_db`, `peak_db`, `padded_ms`, `trimmed_ms`, `ring_ms`, `written_s`.
- `result.json`: the final result with the last metrics.

They are written next to each recording under `telemetry\<name>`. The same data is live over the control pipe `\\.\pipe\voloire-video` (`vvcli ctl voloire-video "{\"cmd\":\"status\"}"`; commands: `ping status metrics logs list_windows list_monitors list_devices get_config set_config start pause resume stop shutdown`, plus `ui_*` for the tray tests). A crash writes a symbolised stack to `%LOCALAPPDATA%\Voloire-video\crash.log`.

Synthetic sources (`--synthetic`) stand in for the screen and the audio devices: a frame counter, a once-per-second flash and a tone that beeps on the flash. `testsubject.exe` is a stand-in game: a window that flashes with its own audio beeping in step, optionally borderless, and a `noise` role that plays another tone from a different process.

## Licence

MIT, see `LICENSE`. Third-party components: `THIRD-PARTY.md`.
