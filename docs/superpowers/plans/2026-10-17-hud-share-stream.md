# Status HUD, Quality and Share Presets, YouTube Streaming: Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give Voloire-video an always-visible status pill at the top of the screen with a right-click settings menu, one-click quality presets plus an automatic size-capped share copy, and OBS-style YouTube streaming with a stream key, all proven by an extended autonomous test harness.

**Architecture:** Everything extends the existing pipeline (`Recorder` -> `Muxer`) and the existing outside-in harness (control pipe + telemetry + ffprobe/numpy judgement). Streaming is a second *output kind* of the same pipeline (file **or** stream, never both at once), written through FFmpeg's FLV/RTMP muxer with an async packet writer so the network can never block capture. No YouTube API, no Google sign-in: the app pushes RTMP to the ingest URL with the stream key copied from YouTube Studio; title and category are written by hand in YouTube Studio. The harness drives a local RTMP ingest.

**Tech Stack:** C++20 / MSVC, Qt 6.5.3 (UI), FFmpeg 8.1 LGPL shared (BtbN), DPAPI (`CryptProtectData`), doctest (unit), Python 3 + numpy + ffmpeg/ffprobe (e2e). No new third-party dependency.

**Spec:** this document's *Context* and *Decisions* sections (Franco's request of 2026-10-17 and his answers).

## Context

Franco wants, in this order of importance:
1. A minimal monitor that tells him **at a glance** whether a recording is running (state + timer), as a small icon/pill at the **top** of the screen, from which everything can be configured with a **right click**.
2. A **quick compression choice** so a clip is already shareable without him re-encoding it.
3. **YouTube live streaming** like OBS.
4. **First step:** a complete test and observability harness that lets the implementing agent simulate recording and streaming and run end-to-end tests fully autonomously.

What already exists (do not rebuild): tray icon with state/timer/pulse and a full right-click menu (`src/ui/tray.cpp`), config panel (`src/ui/panel.cpp`), control pipe `\\.\pipe\voloire-video` with `ui_*` test commands (`src/ui/main.cpp`, `src/core/app_controller.cpp`), telemetry `events.jsonl`/`metrics.jsonl`/`result.json` (`src/core/telemetry.*`), synthetic sources, `testsubject.exe`, 12 e2e scenarios (`tests/e2e/run_e2e.py`, `tests/e2e/vvlib.py`), doctest unit tests (`tests/unit_tests.cpp`), CI (`.github/workflows/ci.yml`).

## Decisions (Franco's answers, binding)

- **HUD:** a floating always-on-top pill at the top edge of the screen, in addition to the tray. Excluded from screen capture.
- **Compression:** both a recording **quality preset** and an automatic **share copy** at a target size made at stop.
- **YouTube:** stream key only. No Google Cloud project, no OAuth, no Data API. Franco writes the title by hand in YouTube Studio; the app does not compute, suggest or copy a title, and does not detect the game name.
- **Record and stream are mutually exclusive:** one output per session.
- **Palette:** the HUD and the shared context menu follow the reference image `docs/design/palette.png` (dark variant, "Menu dark"): sage/forest green on near-black, red recording dot. The config panel keeps its current black/neon theme (out of scope; the existing `ui` theme checks stay valid). Sampled values, to add to `vv::ui::col` as a new `hud` namespace:

  | token | hex | use |
  |---|---|---|
  | `hud::bg` | `#14201E` | pill and menu background |
  | `hud::surface` | `#2F3633` | menu item hover, pill border |
  | `hud::accent` | `#154239` | selected/checked item, LIVE label background |
  | `hud::tint` | `#D6E4D6` | primary text on dark, idle dot |
  | `hud::muted` | `#8A9590` | secondary text, disabled items |
  | `hud::rec` | `#EF2746` | recording dot, LIVE dot, error |
  | `hud::paused` | `#FFB000` | paused dot, RECONN |

  These replace the `col::*` colours named in Task 3 and Task 7 for the HUD and the menu.

## Global Constraints

- Work on branch `feature/hud-share-stream` in its own `git worktree`; the main checkout stays on `main`. Merge only through a pull request with green CI.
- Commit messages and PR text in English; no AI/assistant attribution anywhere (no `Co-Authored-By`, no "Generated with").
- Windows 10 2004 (build 19041)+ x64 only; Windows PowerShell 5.1 is the shell on the dev machine (no `&&`; `rg` is not installed).
- No new third-party libraries. System libs allowed: `crypt32`, `shell32`.
- Every component reports through `Telemetry` (`VV_LOG`, `inc`, `set`); nothing prints on its own.
- The stream key never appears in: `events.jsonl`, `metrics.jsonl`, `result.json`, `crash.log`, pipe responses (`status`, `get_config`, `logs`, `ui_state`), tooltips, menu text.
- UI copy in English, font Ubuntu / Ubuntu Mono (already loaded by `loadFonts`), colours from `vv::ui::col` and the new `vv::ui::hud` tokens (`src/ui/theme.h`).
- Tests never stream to real YouTube: every stream scenario targets the local `RtmpIngest`.
- Python harness: every child process (ffmpeg listener, fakes) starts with `creationflags=subprocess.CREATE_NO_WINDOW`.
- Settings and the stored stream key stay isolated by the existing `VV_SETTINGS_DIR`.

## Review Focus

1. **Stream key leakage:** the key appears in any telemetry file or pipe response. Test: `scan_for_secrets()` over the whole telemetry dir + every pipe response collected in the scenario (Task 6, Task 7).
2. **Network stall blocks capture:** an ingest that stops reading must not freeze the video loop. Expected: `video.dropped_ticks <= 2` during the stall, stream goes `reconnecting`, then recovers or ends with a clear error within 15 s. Test `stream_stall` (Task 6).
3. **Share copy cannot fit:** clip too long for the target size -> clear error naming the maximum duration, no half-written file left. Also non-ASCII paths (`Città`) and sources without audio. Tests in Task 4.
4. **HUD saved off-screen** (monitor unplugged / resolution changed) -> on start it is clamped onto the nearest available screen. Test in Task 3.
5. **Go live with no stream key stored** (or an empty one) -> start refused before any connection with `"no stream key: set it in YouTube > Set stream key..."`; menu `Go live` disabled while no key is set. Tests in Task 6 and Task 7.

---

## File Structure

| File | Responsibility |
|---|---|
| `tests/e2e/fakes/rtmp_ingest.py` (new) | Local RTMP ingest = `ffmpeg -listen 1`, start/stop/kill/freeze, received file |
| `tests/e2e/fakes/blackhole.py` (new) | TCP server that accepts and never reads |
| `tests/e2e/fakes/selftest.py` (new) | Self-test of the fakes, scenario `harness_selftest` |
| `tests/e2e/vvlib.py` (modify) | helpers: `Gui`, `scan_for_secrets`, `capabilities` |
| `tests/e2e/run_e2e.py` (modify) | new scenarios, registered in `SCENARIOS` |
| `src/core/quality.{h,cpp}` (new) | preset -> bitrate/height table |
| `src/core/share.{h,cpp}` (new) | size-capped share copy (in-process transcode reusing `Muxer`) |
| `src/core/encoder.{h,cpp}` (modify) | `EncoderParams` extensions, async network writer |
| `src/core/recorder.{h,cpp}` (modify) | output kind file/stream, single-mix audio for stream, reconnect |
| `src/core/redact.{h,cpp}` (new) | stream-key masking for json and URLs |
| `src/core/secrets.{h,cpp}` (new) | DPAPI-protected per-user secret store |
| `src/core/config.{h,cpp}` (modify) | new fields, `UiPrefs` in `ui.json` |
| `src/core/app_controller.cpp` (modify) | pipe commands `share`, `share_status`, `set_stream_key` |
| `src/cli/main.cpp` (modify) | `--quality`, `--stream`, `--stream-key`, `share` subcommand |
| `src/ui/control_menu.{h,cpp}` (new) | the one context menu, shared by tray and HUD |
| `src/ui/hud.{h,cpp}` (new) | the top status pill |
| `src/ui/tray.{h,cpp}`, `panel.{h,cpp}`, `main.cpp`, `theme.{h,cpp}` (modify) | wiring, panel sections, `ui_*` commands |

---

### Task 1: Harness and observability extensions (first, before any product code)

**Files:**
- Create: `tests/e2e/fakes/__init__.py`, `rtmp_ingest.py`, `blackhole.py`, `selftest.py`
- Modify: `tests/e2e/vvlib.py`, `tests/e2e/run_e2e.py`, `.github/workflows/ci.yml`

**Interfaces (Produces):**
- `RtmpIngest(out_dir, port=0, app="live2", key="testkey")`: `.url` (`rtmp://127.0.0.1:<port>/live2`), `.key`, `.start()`, `.stop()` (graceful, `q` on stdin), `.kill()`, `.freeze()` / `.thaw()` (NtSuspendProcess/NtResumeProcess via ctypes), `.received` (Path of the `.mkv` it remuxes into with `-c copy`), `.wait_connected(timeout)`. Port 0 = pick a free port first.
- `Blackhole(port=0)`: `.url`, `.stop()`.
- `vvlib.Gui(out_dir, pipe, env_extra={})`: launches `voloire-video.exe --pipe <pipe> --show` with `VV_SETTINGS_DIR=<out>/settings`; `.c` (a `Pipe`), `.close()` (shutdown + wait 15 s + kill).
- `vvlib.scan_for_secrets(paths_and_texts, secrets) -> list[str]` (each hit as `"<where>: <secret prefix 4 chars>..."`).
- `vvlib.capabilities() -> dict`: `{"rtmp": bool, "rtmps": bool, "encoders": [...]}` from `ffmpeg -protocols` / `-encoders`; written into `report.json` data.

- [ ] **Step 1: Write `tests/e2e/fakes/selftest.py`** with these checks (registered as scenario `harness_selftest`):
  - `ffmpeg -re -f lavfi -i testsrc2=s=640x360:r=30 -f lavfi -i sine=440 -t 4 -c:v h264_mf -c:a aac -f flv <ingest.url>/<key>` -> `ingest.received` probes as 1 video + 1 audio stream, duration 3.5..4.5 s.
  - `ingest.kill()` mid-push -> the pushing ffmpeg exits non-zero within 10 s.
  - Connecting to `Blackhole` succeeds, a 1 s read gets no data.
  - `scan_for_secrets` finds a planted key in a temp file and in a string, and nothing in a clean one.
- [ ] **Step 2: Run** `python tests\e2e\run_e2e.py --only harness_selftest` -> FAIL (fakes missing).
- [ ] **Step 3: Implement the fakes and vvlib helpers** (listener command: `ffmpeg -hide_banner -loglevel warning -listen 1 -timeout 30 -f flv -i rtmp://127.0.0.1:<port>/<app>/<key> -c copy -f matroska <out>`).
- [ ] **Step 4: Run** the same command -> PASS. Add `harness_selftest` to the CI e2e `--only` list.
- [ ] **Step 5: Commit** `test: add RTMP ingest and blackhole fakes to the e2e harness`.

### Task 2: Recording quality presets

**Files:** Create `src/core/quality.{h,cpp}`; Modify `config.{h,cpp}`, `recorder.cpp` (`start`), `src/cli/main.cpp`, `tests/unit_tests.cpp`, `tests/e2e/run_e2e.py`.

**Interfaces (Produces):**
- `RecordConfig::quality` : `"high" | "standard" | "light" | "minimal" | "custom"`, default `"standard"`; JSON key `"quality"`; validated.
- `int presetBitrateKbps(const std::string& preset, int outHeight, int fps, const std::string& codec);`
- `int presetHeightCap(const std::string& preset);` -> 720 for `minimal`, 0 otherwise.
- `Recorder::start` resolves the effective bitrate after the output size is known: `custom` -> `cfg.bitrateKbps`; otherwise `presetBitrateKbps(cfg.quality, ep.height, cfg.fps, cfg.codec)`; `minimal` caps `ep.height` at 720 when the source or `outHeight` is taller. The effective value is logged in the `muxer open` event as `bitrateKbps` and published as gauge `video.bitrate_kbps`.
- CLI `--quality high|standard|light|minimal` (and `--bitrate` implies `custom`).

Table (kbps for H.264 at fps > 30; fps <= 30 multiply by 0.65; HEVC/AV1 multiply by 0.7; round to the nearest 100; clamp 300..200000):

| output height | high | standard | light | minimal |
|---|---|---|---|---|
| <= 480 | 8000 | 3000 | 1500 | 800 |
| <= 720 | 20000 | 7500 | 4000 | 2000 |
| <= 1080 | 40000 | 12000 | 6000 | 2000 |
| <= 1440 | 60000 | 20000 | 10000 | 2000 |
| > 1440 | 100000 | 40000 | 20000 | 2000 |

- [ ] **Step 1: Failing unit tests** `TEST_CASE("quality presets map to bitrates")`: `presetBitrateKbps("standard",1080,60,"h264")==12000`; `("light",720,30,"h264")==2600`; `("high",2160,60,"hevc")==70000`; `("minimal",1080,60,"h264")==2000`; `presetHeightCap("minimal")==720`; `RecordConfig` with `quality="ultra"` fails `validate()` with a message containing `quality`.
- [ ] **Step 2: Run** -> FAIL. **Step 3: Implement.** **Step 4: Run** -> PASS.
- [ ] **Step 5: e2e scenario `quality_presets`** (synthetic source is 1280x720; 4 s, 30 fps, `--height 720`): for each of `high, standard, light, minimal` record; `result.json` metric `video.bitrate_kbps` equals the table value (720 row, x0.65); file sizes non-increasing in that order with `high` > `minimal`; `--quality minimal --height 1080` produces height 720. Add to CI `--only` list.
- [ ] **Step 6: Commit** `feat: quality presets for recordings`.

### Task 3: Shared control menu and the top status HUD

**Files:** Create `src/ui/control_menu.{h,cpp}`, `src/ui/hud.{h,cpp}`; Modify `src/ui/tray.{h,cpp}`, `src/ui/main.cpp`, `src/ui/theme.{h,cpp}`, `src/ui/CMakeLists.txt`, `src/core/config.{h,cpp}`, `tests/e2e/run_e2e.py`.

**Interfaces (Produces):**
- `struct UiPrefs { bool hud = true; int hudX = -1, hudY = -1; int shareTargetMB = 0; bool shareToClipboard = true; json toJson() const; static UiPrefs fromJson(const json&); };` with `UiPrefs loadUiPrefs(); void saveUiPrefs(const UiPrefs&);` stored in `ui.json` next to `config.json` (`settingsPath()` parent).
- `void buildControlMenu(QMenu& menu, MainPanel& panel, ControlMenuHooks hooks);` - moves the body of `TrayController::rebuildMenu()` here unchanged, plus a checkable `Show status bar` entry before `Open panel`. `ControlMenuHooks { std::function<void()> showPanel; std::function<void(bool)> setHudVisible; std::function<bool()> hudVisible; }`. Later tasks add their submenus here only.
- `class HudBar : public QWidget` - `explicit HudBar(MainPanel* panel, UiPrefs* prefs)`; `json state() const` -> `{visible, x, y, w, h (physical px from GetWindowRect), label, timer, dot ("idle"|"recording"|"paused"|"live"|"error"), excludedFromCapture}`; `QMenu* menu()`; `void setTestPattern(bool)` (fills the pill with pure `#FF00FF`); `void setExcludeFromCapture(bool)`.
- Pipe additions in `main.cpp`: `ui_state` gains `"hud": hud.state()`; `ui_trigger` accepts `"surface": "tray"|"hud"` (default tray); new `ui_hud` `{visible?, x?, y?, testPattern?, exclude?}`.

Behaviour: frameless, `Qt::Tool | Qt::WindowStaysOnTopHint`, translucent, ~160x28 logical px, rounded pill in `hud::bg` with a `hud::surface` border; content `[dot] LABEL  HH:MM:SS` in Ubuntu Mono, text `hud::tint`. Dot: idle `hud::tint` at 50% opacity, recording `hud::rec` pulsing with the tray's 500 ms tick, paused `hud::paused` static, live `hud::rec` + label `LIVE` on `hud::accent`, error `hud::rec` static + label `ERROR` (tooltip carries the error text). The shared menu gets a stylesheet in the same tokens (`hud::bg` background, `hud::accent` selection, `hud::muted` disabled text). Default position: top-centre of the primary screen, 8 px from the top. Drag with the left button moves it and saves `hudX/hudY` on release; a plain left click (no drag) toggles the panel; right click opens the shared menu. On every `show`, `SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)`; the BOOL result is logged (`component "hud"`) and returned in `excludedFromCapture`. On start, if the saved rect does not intersect any `QGuiApplication::screens()` geometry, move it to the default position.

- [ ] **Step 1: e2e scenario `hud`** (fails now), using `Gui`:
  - `ui_state.hud.visible` true, `dot=="idle"`, `excludedFromCapture` true, geometry inside the primary monitor and within 40 px of its top.
  - `ui_trigger surface=hud path=["Format","Codec","HEVC (H.265)"]` -> `get_config.codec=="hevc"` (proves one shared menu); the dumped HUD menu equals the tray menu except the first (header) line.
  - Start through the HUD menu (`Start recording`) with a subject window -> `dot=="recording"`, timer advances >= 2 s; pause -> `paused`; stop -> `idle`.
  - Capture exclusion: `ui_hud testPattern=true`, `ui_override videoSource=monitor monitor=0`, record 2 s through the menu, decode the middle frame cropped to the HUD rect -> magenta pixel share < 0.5%; control run with `ui_hud exclude=false` -> share > 20% (the check is real). Local only (needs a screen).
  - Off-screen restore: `ui_hud x=-5000 y=-5000`, shutdown, relaunch `Gui` with the same settings dir -> HUD rect intersects a monitor.
  - `Show status bar` unchecked -> `hud.visible` false and persisted after relaunch.
  - Screenshots `hud_idle.png`, `hud_recording.png` via `ui_screenshot`.
- [ ] **Step 2: Run** `--only hud,ui` -> `hud` FAIL, `ui` PASS (baseline for the refactor).
- [ ] **Step 3: Extract `buildControlMenu`** and switch `TrayController` to it. **Run** `--only ui` -> still PASS.
- [ ] **Step 4: Implement `HudBar`, `UiPrefs`, pipe commands.**
- [ ] **Step 5: Run** `--only hud,ui` -> PASS. Commit `feat: always-on-top status bar with the shared control menu`.

### Task 4: Share copy engine (core)

**Files:** Create `src/core/share.{h,cpp}`; Modify `src/core/encoder.{h,cpp}`, `src/core/app_controller.{h,cpp}`, `src/cli/main.cpp`, `tests/unit_tests.cpp`, `tests/e2e/run_e2e.py`.

**Interfaces:**
- Consumes: the existing `Muxer` and `EncoderParams` (`src/core/encoder.h`).
- `EncoderParams` gains: `std::string format;` (forced muxer name, empty = guess), `std::string rateControl = "vbr";` (`"vbr"|"cbr"`; CBR sets `rc` to `cbr` for nvenc/amf, `bit_rate=maxrate=bitrate`, `rc_buffer_size=bitrate` for all), `int gopSeconds = 2;`, `int audioKbps = 160;`, `bool faststart = false;` (MP4: `movflags=+faststart` instead of the fragmented flags).
- `struct SharePlan { int width, height, fps, videoKbps, audioKbps; std::string error; };`
  `SharePlan planShare(double durationSec, int srcW, int srcH, double srcFps, int targetMB, bool hasAudio);` pure:
  `totalKbps = targetMB * 8388.608 * 0.92 / durationSec` (1 MB = 1 MiB); `audioKbps = hasAudio ? 128 : 0`; `videoKbps = floor(totalKbps - audioKbps)`. If `videoKbps < 150`: `error = "too long for <N> MB: at most <S> s"` with `S = floor(targetMB * 8388.608 * 0.92 / (150 + audioKbps))`. Otherwise, while `bpp = videoKbps*1000/(w*h*fps) < 0.04`: first drop fps to 30 if > 30, then height through 1080 -> 720 -> 480 (keep aspect, even sizes); stop at 480p30 even if bpp stays low.
- `struct ShareResult { std::string path, error; uint64_t bytes = 0; int attempts = 0; json toJson() const; };`
  `ShareResult makeShareCopy(const std::string& src, int targetMB, std::function<void(double)> progress, std::atomic<bool>* cancel);`
  Demux + decode the video stream and the default audio stream (track titled `MIX`, else the first), convert video to BGRA `VideoFrame`, resample audio to 48 kHz stereo float, feed a `Muxer` (`container mp4`, `codec h264`, `encoder auto`, `rateControl cbr`, `faststart true`, one track `"MIX"` or none). Drop a decoded frame when its target pts equals the previous written one (fps reduction). Output `<src stem>_share_<targetMB>MB.mp4` beside the source, written to `<name>.part` then renamed. If the result exceeds the target: retry with `videoKbps *= 0.85 * target/actual`, at most 2 retries; still too big -> delete and error. Any error deletes the `.part`.
- `AppController`: commands `{"cmd":"share","path":P,"targetMB":N}` (async; one job at a time) and `{"cmd":"share_status"}` -> `{state: "idle"|"running"|"done"|"error", progress, path, bytes, error}`; `status` also embeds `share`. Telemetry events `component "share"`, gauge `share.progress`.
- CLI `vvcli share <file> --target-mb N` -> prints `ShareResult` json, exit 0 on success, 5 on error.

- [ ] **Step 1: Failing unit tests** `TEST_CASE("planShare fits the target")`: `planShare(30,1920,1080,60,25,true)` -> `videoKbps` in 6200..6400 and 1920x1080 at 60 fps kept; `planShare(120,1920,1080,60,10,true)` -> height 480, fps 30, `videoKbps` in 500..530; `planShare(3600,1920,1080,60,10,true).error` contains `"at most 277 s"`; `planShare(10,1280,720,30,25,false).audioKbps==0`. Plus `TEST_CASE("makeShareCopy produces a playable file under the target")`: record 6 s synthetic (`encoder mf`), share at 1 MB -> `bytes <= 1048576`, file exists, no `.part` left.
- [ ] **Step 2: Run** -> FAIL. **Step 3: Implement.** **Step 4: Run** -> PASS.
- [ ] **Step 5: e2e scenario `share_copy`:** synthetic 20 s 1080p60 `--quality high`; `vvcli share --target-mb 8` -> size <= 8 MiB, `decodes_clean`, exactly 1 audio stream, A/V offset within ±150 ms (`check_sync_and_tones` with `check_mic_tone=False`, `isolation=False` on track 0), `moov` atom before `mdat` (read the first 64 KiB); output dir named `Città` (non-ASCII) works; a recording made with `--sys none --mic none` shares with 0 audio streams; a 35 s `--quality minimal` synthetic recording shared at `--target-mb 1` -> exit 5, message contains `at most 27 s`, no `_share_` or `.part` file left. Add to CI.
- [ ] **Step 6: Commit** `feat: size-capped share copy of a recording`.

### Task 5: Quality and share copy in the UI

**Files:** Modify `src/ui/control_menu.cpp`, `src/ui/panel.{h,cpp}`, `src/ui/tray.cpp`, `src/ui/main.cpp`, `tests/e2e/run_e2e.py`.

**Interfaces:** Consumes `UiPrefs::shareTargetMB/shareToClipboard`, pipe `share`/`share_status`.
- Menu (shared): `Quality` submenu (radio `High`, `Standard`, `Light`, `Minimal`, disabled radio `Custom (<n> kbps)` checked when quality is custom) placed after `Format`; `Share copy` submenu: radio `Off`, `10 MB`, `25 MB`, `50 MB`, `100 MB`, separator, checkable `Copy to clipboard when ready`, `Make share copy of last recording` (enabled when a last recording exists and no job runs). Both locked while recording, like `Format`.
- Panel: `Quality` combo and `Share copy` combo next to the existing format controls; editing the bitrate spinbox switches quality to `custom`.
- After a successful file recording stops and `shareTargetMB > 0`, start the job automatically. While running: tray tooltip and HUD label show `SHARE 42%`. On done: tray balloon `Share copy ready: <name> (<size> MB)`; when `shareToClipboard`, put the file on the clipboard as a file URL (`QMimeData::setUrls`) so Ctrl+V pastes it into Discord/WhatsApp/Explorer. On error: balloon with the error text.
- `ui_state` gains `clipboardFiles` (local paths of the clipboard's URLs) and `share` (same shape as `share_status`).

- [ ] **Step 1: Extend scenario `ui`** (fails now): set `Quality > Light` and `Share copy > 10 MB` through the menu; `get_config.quality=="light"`; record ~5 s via the menu; wait for `ui_state.share.state=="done"` (timeout 60 s); file exists, size <= 10 MiB; `clipboardFiles == [share path]`; `Share copy > Make share copy of last recording` re-runs it.
- [ ] **Step 2: Run** `--only ui` -> FAIL. **Step 3: Implement.** **Step 4: Run** -> PASS.
- [ ] **Step 5: Commit** `feat: quality and share copy from the tray and the status bar`.

### Task 6: Streaming output (RTMP with stream key) with reconnect

**Files:** Modify `src/core/encoder.{h,cpp}`, `src/core/recorder.{h,cpp}`, `src/core/config.{h,cpp}`, `src/core/app_controller.cpp`, `src/cli/main.cpp`, `src/core/CMakeLists.txt` (`crypt32`); Create `src/core/redact.{h,cpp}`, `src/core/secrets.{h,cpp}`; Modify `src/core/telemetry.cpp` (apply redaction in `log`), `tests/unit_tests.cpp`, `tests/e2e/run_e2e.py`.

**Interfaces (Produces):**
- `RecordConfig` gains: `output = "file"` (`"file"|"stream"`), `streamUrl = "rtmp://a.rtmp.youtube.com/live2"`, `streamBitrateKbps = 6000` (validated 1000..51000), `streamHeight = 1080` (0 = source). The key is **not** a config field.
- `secrets.h`: `bool saveSecret(const std::string& name, const std::string& value); std::string loadSecret(const std::string& name); void deleteSecret(const std::string& name);` DPAPI `CryptProtectData` (current user) into `<settings dir>/secrets/<name>.bin`. One name used: `stream_key`.
- `redact.h`: `json redactJson(const json&);` masks values of keys (case-insensitive) `key`, `streamKey` with `"***"`; `std::string redactUrl(const std::string&);` turns `rtmp[s]://host[:port]/app/<anything>` into `.../app/***`. `Telemetry::log` runs `redactJson` on `extra` and `redactUrl` on `msg` and on every string value of `extra`.
- `Muxer`: `EncoderParams::asyncWrite` (bool) and `maxQueueMs = 3000`. With `asyncWrite`, encoded packets are cloned into a queue drained by a writer thread; when the queued duration exceeds `maxQueueMs`, video packets are dropped up to the next keyframe (audio kept), counted by `droppedPackets()`. For network URLs open with `avio_open2` and options `rw_timeout=5000000`, `tcp_nodelay=1`; `avformat_network_init()` once. New: `bool failed() const; double queuedMs() const; uint64_t droppedPackets() const;`.
- `Recorder` with `cfg.output=="stream"`: `EncoderParams{format "flv", codec "h264", rateControl "cbr", gopSeconds 2, bitrateKbps cfg.streamBitrateKbps, audioKbps 128, asyncWrite true}`, height capped at `streamHeight`, exactly **one** audio track `"MIX"` carrying the sum of the enabled channels (silence when none, YouTube needs audio). Target = `cfg.streamUrl + "/" + key`, key from the CLI `--stream-key` when given, else `loadSecret("stream_key")`; empty -> start fails with `"no stream key: set it in YouTube > Set stream key..."` before any connection. `pause` returns false in stream mode. No file is written; `RecResult.path` empty, `bytes` = bytes sent.
- Reconnect: when `mux_->failed()`, the video and audio loops keep the timeline but discard samples; a reconnect thread opens a fresh `Muxer` with the same params after 1, 2, 4, 8, 16 s (5 attempts); the new muxer's video pts base = elapsed at reconnect. `mux_` is a `std::shared_ptr` swapped under a mutex held only to copy the pointer. All attempts failed -> stop with `error = "stream lost: <reason>"`.
- `status` gains `output` and `stream: {state: "connecting"|"live"|"reconnecting"|"ended", kbps, queueMs, droppedPackets, reconnects}`. Gauges: `stream.kbps`, `stream.queue_ms`, `stream.dropped_packets`, `stream.reconnects`, `stream.bytes`.
- Pipe `set_stream_key {"key": K}` (stores via `saveSecret`, empty key deletes it; response `{"ok":true,"streamKeySet":bool}`); `get_config` adds `streamKeySet`. CLI: `--stream URL`, `--stream-key K`, `--stream-bitrate KBPS`.

- [ ] **Step 1: Failing unit tests:** `redactJson({{"streamKey","abc"},{"x",{{"key","r"}}}})` masks both; `redactUrl("rtmp://a.rtmp.youtube.com/live2/abcd-efgh")=="rtmp://a.rtmp.youtube.com/live2/***"`; secrets round trip and `loadSecret("missing").empty()`; the secret file does not contain the plaintext bytes; `RecordConfig` with `output="stream"`, `streamBitrateKbps=60000` fails validation.
- [ ] **Step 2: Failing e2e scenarios:**
  - `stream_rtmp`: `RtmpIngest`, `vvcli record --synthetic --stream <url> --stream-key <key> --duration 10 --fps 30 --pipe vv-e2e-stream` -> exit 0; `ingest.received` has 1 H.264 video at 30 fps, exactly 1 audio stream, duration 9..11 s, `decodes_clean`, A/V offset within ±150 ms, flashes 8..11; metric `stream.kbps` between 300 and 7800 (encoders undershoot on the static synthetic picture; the bound that matters is the cap); `scan_for_secrets([key])` over telemetry + `status`/`logs` responses finds nothing.
  - `stream_stored_key`: `set_stream_key` over the pipe of a `vvcli record --pipe` idle instance is not possible, so use `Gui`: `set_stream_key`, `get_config.streamKeySet` true and the key absent from the response; the secret file exists and does not contain the key bytes. Then `set_stream_key ""` -> `streamKeySet` false; `vvcli record --synthetic --stream <url> --duration 3` (no key) with the same `VV_SETTINGS_DIR` -> exit 3, error starts with `no stream key`, the ingest never sees a connection.
  - `stream_reconnect`: kill the ingest at 4 s, start a new one on the same port 1.5 s later; recording continues; `stream.reconnects == 1`; the second ingest file decodes clean and lasts >= 3 s; events contain `reconnecting` then `live`.
  - `stream_stall`: `ingest.freeze()` at 3 s for 8 s -> `video.dropped_ticks <= 2`; `status.stream.state` becomes `reconnecting` within 8 s; after `thaw()` and a fresh ingest the stream ends with exit 0 or 4 and a non-empty error, never a hang (wall time < duration + 20 s).
  - `stream_unreachable`: URL of `Blackhole` -> start fails within 10 s with an error containing `stream`, exit 3.
- [ ] **Step 3: Run** -> FAIL. **Step 4: Implement** (redaction, secrets, muxer async/network, recorder stream mode, reconnect, CLI, pipe). **Step 5: Run** unit + `--only stream_rtmp,stream_stored_key,stream_reconnect,stream_stall,stream_unreachable` -> PASS. Add `stream_rtmp,stream_unreachable` to CI.
- [ ] **Step 6: Commit** `feat: RTMP streaming output with reconnect and stream key protection`.

### Task 7: Streaming in the UI (menu, panel, HUD)

**Files:** Modify `src/ui/control_menu.cpp`, `src/ui/panel.{h,cpp}`, `src/ui/hud.cpp`, `src/ui/tray.cpp`, `src/ui/main.cpp`, `tests/e2e/run_e2e.py`.

- Menu (shared): first action text depends on output: `Start recording`/`Stop recording` (file) or `Go live`/`End stream` (stream); `Go live` disabled while no key is stored; `Pause` disabled in stream mode. New submenus after `Share copy`: `Output` (radio `Record to file`, `Stream to YouTube`), locked while live; `YouTube` with: disabled info line `Stream key: set` | `Stream key: not set`, `Set stream key...` (masked `QInputDialog`, `QLineEdit::Password`), `Clear stream key`, `Open YouTube Studio` (opens `https://studio.youtube.com/` with `QDesktopServices::openUrl`, where Franco writes the title and copies the key).
- Panel: an `Output` row (file / stream) and a `Stream` group shown in stream mode: masked key field with a `Save` button, ingest URL (default YouTube, editable), stream bitrate.
- HUD and tray in stream mode: dot `live`, label `LIVE`, timer, plus `stream.kbps` as `6.0M` on the HUD; state `reconnecting` shows label `RECONN` with amber dot. Tooltip: `Voloire-video | LIVE 00:01:02 | 6.0 Mbps`. Never the key.

- [ ] **Step 1: Failing e2e scenario `ui_stream`** (`Gui` + `RtmpIngest` + `Subject`), through the HUD menu only: `Output > Stream to YouTube`; with no key `Go live` is disabled; `set_stream_key` over the pipe (dialogs are not drivable), `ui_override streamUrl=<ingest.url>`; `YouTube` info line reads `Stream key: set`; `Go live` -> `hud.dot=="live"`, `hud.label=="LIVE"`, `Pause` disabled, `Output` disabled; after 5 s `End stream` -> idle; `ingest.received` decodes clean; `scan_for_secrets([key])` over every `ui_state` dump (menu, tooltip, hud) and the telemetry dir finds nothing.
- [ ] **Step 2: Run** -> FAIL. **Step 3: Implement.** **Step 4: Run** `--only ui,hud,ui_stream` -> PASS.
- [ ] **Step 5: Commit** `feat: go live from the tray and the status bar`.

### Task 8: Documentation and full verification

**Files:** Modify `README.md` (features, the new scenarios table, streaming setup), `THIRD-PARTY.md` only if something changed (it should not).

- [ ] **Step 1:** README: HUD, quality presets table, share copy, streaming with a stream key (where to find it in YouTube Studio, title written there by hand), new pipe commands and metrics, new scenarios.
- [ ] **Step 2: Full run:** `scripts\build.ps1`, `build\bin\vv_unit_tests.exe` (all pass), `python tests\e2e\run_e2e.py` (all scenarios, soak included) -> exit 0; `scripts\package.ps1` then `--only installers` -> pass. Paste the final `N/N checks passed` line into the PR.
- [ ] **Step 3:** Commit `docs: status bar, share copy and streaming`; push; open the PR; wait for green CI; merge only if green and no review asks for changes.

---

## Verification (end to end)

- Autonomous: `python tests\e2e\run_e2e.py` covers recording (existing 12 scenarios), presets, share copy, HUD, RTMP streaming, stored key, reconnect, stall, unreachable ingest, and the UI streaming path, all against local fakes. CI runs the subset without screen/audio/GPU: `harness_selftest, synthetic_basic, pause_resume, errors, quality_presets, share_copy, stream_rtmp, stream_unreachable` (all synthetic).
- Manual, by Franco only, once (the agent never streams to real YouTube):
  1. YouTube Studio: live streaming enabled on the channel (first activation takes up to 24 h); copy the stream key.
  2. In the app: `YouTube > Set stream key...`, `Output > Stream to YouTube`, start a game, `Go live`; in YouTube Studio write the title and check the preview; `End stream` after 1 minute.
  3. Record 30 s with `Share copy > 25 MB`, paste with Ctrl+V into a chat: the file attaches and is under 25 MB.

## Known limits (stated, not solved)

- Title, description and category of the live are written by hand in YouTube Studio (no API by decision).
- The HUD cannot draw over exclusive-fullscreen games (no injection); the tray icon remains the indicator there.
- Streaming is H.264 only (widest ingest compatibility); recording codecs are unchanged.
