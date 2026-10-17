# Voloire-video

Windows recorder for one window or application, with the application's own audio and a microphone on separate tracks, to a file or live to YouTube. Tray icon with live state and timer, a status bar pill that carries the same controls, cyberpunk panel in black with neon blue accents.

## What it does

- **Source**: any window, windowed or borderless fullscreen (Windows Graphics Capture), a whole monitor, or a monitor through DXGI Desktop Duplication for exclusive fullscreen.
- **Two audio channels** in one file. With both on, track 1 is `MIX` (SYS + MIC, the default, so any player plays both), then `SYS` and `MIC` separately for editing:
  - `SYS`: only the audio of the selected application and its child processes (WASAPI process loopback), or the full output of a chosen device, or off.
  - `MIC`: the default microphone, any chosen capture device, or off.
- **Format**: MKV (crash safe) or MP4, H.264, HEVC or AV1, NVENC, AMF, Quick Sync or Media Foundation (auto picks the first that opens), source size or 480p to 2160p, 24 to 120 fps, bitrate.
- **Pause and resume** without a hole in the file: the pause is cut out of the timeline.
- **Tray icon**: ring and dot that pulse while recording and show a pause glyph when paused; tooltip and menu header carry the state and timer. Start, stop, pause, window, format, MIC and SYS devices are all in its menu.
- **Status bar (HUD)**: an always-on-top pill with dot, state label and timer, dragged where you like (it remembers); right click gives the same shared menu as the tray. It is excluded from screen captures, and while streaming it shows `LIVE` with the current Mb/s, or `RECONN` with an amber dot while the ingest is down.
- **Quality presets**: High, Standard, Light and Minimal resolve the bitrate from the output height; a hand-typed bitrate switches to Custom.
- **Share copy**: a compact H.264 MP4 copy of a finished recording, capped at a chosen size (10/25/50/100 MB), written beside the source as `<name>_share_<MB>MB.mp4`. The tray can run it after every recording and put the result on the clipboard, so one Ctrl+V pastes the file.
- **Streaming (RTMP)**: go live to YouTube from the tray, the bar or the CLI. The stream key is stored encrypted with DPAPI and never appears in the config, the logs or the telemetry; a dropped ingest reconnects (1/2/4/8/16 s) keeping the timeline, a stalled one never hangs the recorder, and an unreachable one fails in seconds.

Quality presets, H.264, kbps for more than 30 fps (30 fps or less cuts the value by 35%; HEVC and AV1 by another 30%):

| Preset | 480p | 720p | 1080p | 1440p | 2160p |
|---|---|---|---|---|---|
| High | 8000 | 20000 | 40000 | 60000 | 100000 |
| Standard | 3000 | 7500 | 12000 | 20000 | 40000 |
| Light | 1500 | 4000 | 6000 | 10000 | 20000 |
| Minimal | 2000 kbps, never above 720p | | | | |
- **Constant frame rate**, A/V kept on one clock; silent loopback is padded with silence and slow device drift is trimmed, so audio never wanders from the picture.

Requires Windows 10 version 2004 (build 19041) or later, x64. App audio capture needs that version; exclusive-fullscreen titles with DRM-protected audio cannot be captured by any user-mode recorder.

## Install

Download one of the installers from the latest release (both install per user, no administrator rights needed):

- `voloire-video-setup-<version>.exe`
- `voloire-video-<version>.msi`

Silent: `voloire-video-setup-1.0.1.exe /VERYSILENT` or `msiexec /i voloire-video-1.0.1.msi /qn`.

## Use

Start Voloire-video, pick the window (or leave "active window at record time"), check the two audio channels, press REC. The panel hides to the tray with the close button; the tray icon menu does everything the panel does, and so does the status bar's. Autostart with `--minimized` keeps it tray only.

Headless: `vvcli record --window-title "My Game" --sys app --mic default --duration 60 --out D:\clips`. `vvcli list windows|monitors|devices` shows what can be selected.

To go live on YouTube: in YouTube Studio open *Go live* (the manual stream key, not a scheduled event), write the title there and copy the stream key. Then in the tray or on the bar: `YouTube > Set stream key...`, paste it (it is stored encrypted in the settings folder), pick `Output > Stream to YouTube`, and `Go live`. While live `Pause` is off and `Output` is locked; `End stream` stops. Headless it is `vvcli record --stream rtmp://a.rtmp.youtube.com/live2 --stream-bitrate 6000 --stream-key KEY` (a stream writes no file; without `--stream-key` the stored key is used).

Share copy: the `Share copy` menu sets the size cap (`Off`, 10, 25, 50, 100 MB) and whether the copy goes on the clipboard; `Make share copy of last recording` runs it again by hand. From the CLI: `vvcli share <file> --target-mb 8`.

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
| End to end | 21 scenarios, below | `python tests\e2e\run_e2e.py [--only a,b] [--fast]` |
| Installers | silent install, run, uninstall of both | `python tests\e2e\run_e2e.py --only installers` |

Scenarios: synthetic basic (tones, flashes, A/V offset, counter continuity), pause and resume over the pipe, quality presets (the resolved bitrate reaches the encoder, Minimal really caps at 720p), share copy (a size-capped MP4 beside the source, a video-only source, an impossible target that cleans up and fails), streaming to a local RTMP ingest, judged like a file (H.264/fps/one audio track/duration/sync/tones) with no key leak in logs or telemetry, the stored-key path (encrypted blob, cleared key, a clean refusal without one), reconnect after the ingest is killed, a stalled ingest that must not hang the recorder, an unreachable ingest that fails in seconds, format matrix (MKV/MP4 x H.264/HEVC/AV1 x sizes x rates x encoders), per-app audio isolation (a second process plays a different tone and must not leak into `SYS`, while a system-loopback control run must contain it), borderless window, scaled output, monitor and DXGI capture, microphone devices, window closed mid-recording, error handling, tray UI driven through its own menu with screenshots and colour checks, the status bar with its menu, capture exclusion and position persistence, going live through the bar against the local ingest, and a 60 s soak for drift.

Every component reports through one telemetry layer:

- `events.jsonl`: structured log of every state change, source start, encoder choice, FFmpeg warning and error, with microsecond timestamps.
- `metrics.jsonl`: all metrics once a second: `video.fps_actual`, `video.frames`, `video.repeated_frames`, `video.dropped_ticks`, `video.encode_ms`, `video.bitrate_kbps`, `video.queue_lag_ms`, `rec.size_bytes`, per track `audio.sys|mic.rms_db`, `peak_db`, `padded_ms`, `trimmed_ms`, `ring_ms`, `written_s`, and for a stream `stream.kbps`, `stream.queue_ms`, `stream.dropped_packets`, `stream.reconnects`, `stream.bytes` (plus a `share.progress` gauge while a share copy runs).
- `result.json`: the final result with the last metrics.

They are written next to each recording under `telemetry\<name>`. The same data is live over the control pipe `\\.\pipe\voloire-video` (`vvcli ctl voloire-video "{\"cmd\":\"status\"}"`; commands: `ping status metrics logs list_windows list_monitors list_devices get_config set_config start pause resume stop shutdown share share_status set_stream_key`, plus `ui_*` for the tray tests). `status` embeds the live `stream` block (state, kbps, queueMs, droppedPackets, reconnects) and `share`; `get_config` carries `streamKeySet` and never the key itself. A crash writes a symbolised stack to `%LOCALAPPDATA%\Voloire-video\crash.log`.

Synthetic sources (`--synthetic`) stand in for the screen and the audio devices: a frame counter, a once-per-second flash and a tone that beeps on the flash. `testsubject.exe` is a stand-in game: a window that flashes with its own audio beeping in step, optionally borderless, and a `noise` role that plays another tone from a different process.

## Licence

MIT, see `LICENSE`. Third-party components: `THIRD-PARTY.md`.
