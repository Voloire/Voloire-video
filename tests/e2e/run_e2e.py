"""End-to-end test cycle of Voloire-video.

Each scenario drives the real binaries, then judges the produced files and the telemetry the app
wrote. A JSON report lands in --out. Exit code is the number of failed checks (capped at 100).

  python tests/e2e/run_e2e.py [--out DIR] [--only name,name] [--skip name,name]
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import traceback
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
from vvlib import *  # noqa: E402,F401,F403


# ------------------------------------------------------------------ shared checks

def check_telemetry(rep, result, expect_audio=("sys", "mic")):
    tdir = Path(result["telemetryDir"])
    ev, mt, rj = tdir / "events.jsonl", tdir / "metrics.jsonl", tdir / "result.json"
    rep.check("telemetry: events.jsonl written", ev.exists() and ev.stat().st_size > 0)
    rep.check("telemetry: metrics.jsonl sampled at 1 Hz", mt.exists() and len(read_jsonl(mt)) >= 1)
    rep.check("telemetry: result.json written", rj.exists())
    if not ev.exists():
        return {}
    events = read_jsonl(ev)
    states = [e["msg"] for e in events if e["component"] == "recorder" and e["msg"].startswith("state ")]
    rep.check("telemetry: state machine starting -> recording -> stopping -> idle",
              [s.split()[1] for s in states][:1] == ["starting"] and states[-1] == "state idle" and "state recording" in states,
              " > ".join(s.split()[1] for s in states))
    rep.check("telemetry: no error events", not [e for e in events if e["level"] == "error"],
              "; ".join(e["msg"] for e in events if e["level"] == "error")[:200])
    metrics = rj.exists() and json.loads(rj.read_text(encoding="utf-8")).get("metrics", {}) or {}
    rep.data["metrics"] = metrics
    for k in expect_audio:
        rep.check(f"telemetry: audio.{k} meters published", f"audio.{k}.rms_db" in metrics)
    return metrics


def check_container(rep, path, codec="h264", height=None, fps=None, audio=2, min_s=None, max_s=None):
    info = probe(path)
    if not rep.check("media: ffprobe reads the file", info is not None):
        return None
    v, a = streams(info, "video"), streams(info, "audio")
    rep.check("media: one video stream", len(v) == 1)
    rep.check(f"media: video codec {codec}", v and v[0]["codec_name"] == codec, v[0]["codec_name"] if v else "")
    if height:
        rep.check(f"media: height {height}", v and v[0]["height"] == height, v[0]["height"] if v else "")
    if fps:
        rep.check(f"media: {fps} fps", v and abs(fps_of(v[0]) - fps) < 0.01, v[0]["r_frame_rate"] if v else "")
    rep.check(f"media: {audio} audio streams", len(a) == audio, len(a))
    titles = [s.get("tags", {}).get("title") for s in a]
    if audio == 2:
        rep.check("media: tracks titled SYS, MIC", titles == ["SYS", "MIC"], titles)
    dur = float(info["format"]["duration"])
    if min_s is not None:
        rep.check(f"media: duration within {min_s}..{max_s} s", min_s <= dur <= max_s, f"{dur:.2f}")
    ok, why = decodes_clean(path)
    rep.check("media: decodes end to end with no error", ok, why)
    rep.data["duration"] = dur
    return info


def check_sync_and_tones(rep, path, sys_hz=440, mic_hz=1000, expect_flashes=None, tol_ms=120, check_mic_tone=True,
                         isolation=True):
    luma, fps, vstart = decode_luma(path)
    flashes, _ = flash_times(luma, fps, vstart)
    sysx = decode_audio(path, 0)
    onsets, _ = beep_onsets(sysx)
    if expect_flashes:
        lo, hi = expect_flashes
        rep.check(f"video: {lo}..{hi} flashes seen", lo <= len(flashes) <= hi, len(flashes))
        rep.check(f"SYS: {lo}..{hi} beeps heard", lo <= len(onsets) <= hi, len(onsets))
    off, pairs = av_offset(flashes, onsets)
    rep.data["av_offset_ms"] = off
    rep.check(f"A/V sync: offset within +/-{tol_ms} ms", off is not None and abs(off) <= tol_ms and pairs >= 2,
              f"{off if off is None else round(off, 1)} ms over {pairs} pairs")
    if len(onsets):
        # analyse the loud part of the first beeps only
        seg = np.concatenate([sysx[int(t * RATE) + 480: int(t * RATE) + 4320] for t in onsets[:6]])
        f = dominant_freq(seg)
        rep.check(f"SYS: tone is {sys_hz} Hz", abs(f - sys_hz) < 20, f"{f:.0f} Hz")
    if isolation:
        p440, p880 = band_power(sysx, sys_hz), band_power(sysx, 2 * sys_hz)
        rep.data["sys_p_f0"], rep.data["sys_p_2f0"] = p440, p880
        rep.check("SYS: nothing from the other process (880 Hz) leaks in", p440 > 0 and p880 / p440 < 0.02,
                  f"ratio {p880 / max(p440, 1e-12):.4f}")
    if check_mic_tone:
        micx = decode_audio(path, 1)
        f = dominant_freq(micx[RATE:])
        rep.check(f"MIC: tone is {mic_hz} Hz", abs(f - mic_hz) < 20, f"{f:.0f} Hz")
    return flashes, onsets


def vtimes_ok(rep, path):
    """Frame counter based continuity check for the synthetic source."""
    c = decode_counters(path)
    if not rep.check("video: counter decodable", len(c) > 10, len(c)):
        return
    back = int((np.diff(c.astype(np.int64)) < 0).sum())
    uniq = len(np.unique(c)) / len(c)
    rep.check("video: counter never goes backwards", back == 0, f"{back} regressions")
    rep.check("video: >= 90% unique frames (source not stalling)", uniq >= 0.90, f"{uniq:.2%}")


# ------------------------------------------------------------------ scenarios

def sc_synthetic_basic(out):
    rep = Report("synthetic_basic")
    d = out / "synthetic_basic"
    rc, res, err = run_cli(["record", "--synthetic", "--duration", 6, "--fps", 30, "--out", d, "--name", "basic"])
    rep.check("cli exit code 0", rc == 0, f"rc={rc} {err[-200:]}")
    if not res:
        return rep
    rep.check("no dropped ticks", res["droppedTicks"] == 0, res["droppedTicks"])
    rep.check("frame count ~ 6 s * 30 fps", 170 <= res["videoFrames"] <= 190, res["videoFrames"])
    check_container(rep, res["path"], "h264", 720, 30, 2, 5.7, 6.4)
    m = check_telemetry(rep, res)
    rep.check("telemetry: video.fps_actual close to 30", 27 <= m.get("video.fps_actual", 0) <= 33, m.get("video.fps_actual"))
    rep.check("telemetry: SYS level not silent", m.get("audio.sys.max_peak_db", -120) > -20, m.get("audio.sys.max_peak_db"))
    rep.check("telemetry: MIC level not silent", m.get("audio.mic.max_peak_db", -120) > -30, m.get("audio.mic.max_peak_db"))
    check_sync_and_tones(rep, res["path"], expect_flashes=(5, 7), isolation=False)
    vtimes_ok(rep, res["path"])
    return rep


def sc_pause_resume(out):
    rep = Report("pause_resume")
    d = out / "pause_resume"
    p = popen_cli(["record", "--synthetic", "--duration", 0, "--fps", 30, "--out", d, "--name", "pause",
                          "--pipe", "vv-e2e-pause"], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    try:
        c = Pipe("vv-e2e-pause")
        rep.check("pipe: ping answers", c.call(cmd="ping")["ok"])
        time.sleep(2.0)
        s1 = c.call(cmd="status")
        rep.check("status: recording", s1["state"] == "recording", s1["state"])
        r = c.call(cmd="pause")
        rep.check("pause accepted", r["ok"])
        e0 = c.call(cmd="status")["elapsedUs"]
        time.sleep(2.0)
        s2 = c.call(cmd="status")
        rep.check("status: paused", s2["state"] == "paused", s2["state"])
        rep.check("clock frozen while paused", abs(s2["elapsedUs"] - e0) < 60000, f"{(s2['elapsedUs'] - e0) / 1000:.0f} ms drift")
        rep.check("second pause rejected", not c.call(cmd="pause")["ok"])
        rep.check("resume accepted", c.call(cmd="resume")["ok"])
        time.sleep(2.0)
        m = c.call(cmd="metrics")["metrics"]
        rep.check("metrics over pipe: frames counted", m.get("video.frames", 0) > 50, m.get("video.frames"))
        logs = c.call(cmd="logs", n=50)["logs"]
        rep.check("logs over pipe: state transitions present", any(l["msg"] == "state paused" for l in logs))
        res = c.call(cmd="stop")
        rep.check("stop returned the result", res.get("ok") and res.get("bytes", 0) > 0, res.get("error"))
        c.close()  # the CLI leaves on its own once the recording stopped
    finally:
        try:
            p.wait(20)
        except subprocess.TimeoutExpired:
            p.kill()
    if "path" not in res:
        return rep
    rep.check("cli exits cleanly after shutdown", p.returncode in (0, 4), p.returncode)
    info = check_container(rep, res["path"], "h264", 720, 30, 2, 3.6, 4.5)
    rep.check("file is 4 s of media, not 6 s of wall time (pause removed)", info and 3.6 <= float(info["format"]["duration"]) <= 4.5)
    m = check_telemetry(rep, res)
    luma, fps, vs = decode_luma(res["path"])
    fl, _ = flash_times(luma, fps, vs)
    rep.check("video: 3..5 flashes in 4 s of media", 3 <= len(fl) <= 5, len(fl))
    sysx = decode_audio(res["path"], 0)
    rep.check("audio track length matches video within 100 ms",
              abs(len(sysx) / RATE - rep.data["duration"]) < 0.1, f"{len(sysx) / RATE:.2f} s vs {rep.data['duration']:.2f} s")
    vtimes_ok(rep, res["path"])
    return rep


def sc_formats(out):
    rep = Report("formats")
    combos = [("mkv", "h264", 720, 30, "auto"), ("mp4", "h264", 1080, 60, "auto"), ("mkv", "hevc", 720, 30, "auto"),
              ("mp4", "hevc", 1080, 30, "auto"), ("mkv", "av1", 720, 30, "auto"), ("mkv", "h264", 480, 24, "mf"),
              ("mp4", "h264", 720, 30, "nvenc")]
    for cont, codec, h, fps, enc in combos:
        tag = f"{cont}/{codec}/{h}p{fps}/{enc}"
        d = out / "formats" / tag.replace("/", "_")
        rc, res, err = run_cli(["record", "--synthetic", "--duration", 3, "--container", cont, "--codec", codec, "--height", h,
                                "--fps", fps, "--encoder", enc, "--bitrate", 4000, "--out", d, "--name", "fmt"])
        if not rep.check(f"{tag}: recorded", rc == 0 and res, f"rc={rc} {err[-160:]}"):
            continue
        info = probe(res["path"])
        v = streams(info, "video")[0] if info else {}
        ok, why = decodes_clean(res["path"])
        rep.check(f"{tag}: codec/size/rate/decode", v.get("codec_name") == codec and v.get("height") == h and
                  abs(fps_of(v) - fps) < 0.01 and ok, f"{v.get('codec_name')} {v.get('width')}x{v.get('height')} {v.get('r_frame_rate')} via {res['encoder']} {why}")
        rep.check(f"{tag}: two audio tracks", info and len(streams(info, "audio")) == 2)
    return rep


def sc_window_app_audio(out):
    rep = Report("window_app_audio_isolation")
    d = out / "window_app"
    subj = Subject(d, title="VV TestSubject A", hz=440)
    noise = Subject(d, role="noise", hz=880)
    try:
        rc, res, err = run_cli(["record", "--window-title", "VV TestSubject A", "--sys", "app", "--mic", "none",
                                "--duration", 8, "--fps", 30, "--out", d / "app", "--name", "win"])
        rep.check("cli exit code 0", rc == 0, f"rc={rc} {err[-300:]}")
        if res:
            info = check_container(rep, res["path"], "h264", None, 30, 1, 7.6, 8.5)
            check_telemetry(rep, res, expect_audio=("sys",))
            v = streams(info, "video")[0] if info else {}
            rep.check("video: window sized capture (not the desktop)", 800 <= v.get("width", 0) <= 2000 and v.get("height", 0) < 1100, f"{v.get('width')}x{v.get('height')}")
            check_sync_and_tones(rep, res["path"], expect_flashes=(6, 9), tol_ms=150, check_mic_tone=False)
        # Contrast run: the whole system output must contain the noise process too.
        rc2, res2, err2 = run_cli(["record", "--window-title", "VV TestSubject A", "--sys", "system", "--mic", "none",
                                   "--duration", 5, "--fps", 30, "--out", d / "system", "--name", "sys"])
        rep.check("system loopback run ok", rc2 == 0, f"rc={rc2} {err2[-200:]}")
        if res2:
            x = decode_audio(res2["path"], 0)
            p440, p880 = band_power(x, 440), band_power(x, 880)
            rep.check("control: system loopback DOES carry the 880 Hz noise (so isolation above is real)",
                      p440 > 0 and p880 / p440 > 0.5, f"ratio {p880 / max(p440, 1e-12):.2f}")
    finally:
        subj.stop()
        noise.stop()
    return rep


def sc_borderless(out):
    rep = Report("borderless_window")
    d = out / "borderless"
    subj = Subject(d, title="VV TestSubject Borderless", hz=440, borderless=True)
    noise = Subject(d, role="noise", hz=880)
    try:
        rc, res, err = run_cli(["record", "--window-title", "VV TestSubject Borderless", "--sys", "app", "--mic", "none",
                                "--duration", 6, "--fps", 30, "--height", 720, "--out", d / "rec", "--name", "bl"])
        rep.check("cli exit code 0", rc == 0, f"rc={rc} {err[-300:]}")
        if res:
            check_container(rep, res["path"], "h264", 720, 30, 1, 5.6, 6.5)
            check_sync_and_tones(rep, res["path"], expect_flashes=(4, 7), tol_ms=150, check_mic_tone=False)
    finally:
        subj.stop()
        noise.stop()
    return rep


def sc_window_resolution_scaling(out):
    rep = Report("window_scaled_output")
    d = out / "window_scaled"
    subj = Subject(d, title="VV TestSubject Scale", hz=440)
    try:
        rc, res, err = run_cli(["record", "--window-title", "VV TestSubject Scale", "--sys", "app", "--mic", "none",
                                "--duration", 3, "--fps", 60, "--height", 1080, "--codec", "hevc", "--container", "mp4",
                                "--out", d, "--name", "scale"])
        rep.check("cli exit code 0", rc == 0, f"rc={rc} {err[-200:]}")
        if res:
            info = check_container(rep, res["path"], "hevc", 1080, 60, 1, 2.7, 3.5)
            rep.check("frame count ~ 3 s * 60 fps", 160 <= res["videoFrames"] <= 190, res["videoFrames"])
    finally:
        subj.stop()
    return rep


def sc_monitor_capture(out):
    rep = Report("monitor_capture")
    d = out / "monitor"
    for mode, args in (("WGC monitor", ["--monitor", 0]), ("DXGI duplication", ["--dxgi", 0])):
        rc, res, err = run_cli(["record", *args, "--sys", "system", "--mic", "none", "--duration", 3, "--fps", 30, "--out", d / mode.split()[0],
                                "--name", "mon"])
        if not rep.check(f"{mode}: recorded", rc == 0 and res, f"rc={rc} {err[-200:]}"):
            continue
        info = check_container(rep, res["path"], "h264", None, 30, 1, 2.7, 3.6)
        mons = json.loads(subprocess.run([str(CLI), "list", "monitors"], capture_output=True, text=True).stdout)["monitors"]
        v = streams(info, "video")[0]
        rep.check(f"{mode}: size equals monitor 0", (v["width"], v["height"]) == (mons[0]["w"] & ~1, mons[0]["h"] & ~1),
                  f"{v['width']}x{v['height']} vs {mons[0]['w']}x{mons[0]['h']}")
        rep.check(f"{mode}: frames flowed", res["videoFrames"] >= 80, res["videoFrames"])
        shutil.rmtree(d / mode.split()[0], ignore_errors=True)  # desktop footage: never keep it
    return rep


def sc_mic_real(out):
    rep = Report("microphone_devices")
    d = out / "mic"
    listing = json.loads(subprocess.run([str(CLI), "list", "devices"], capture_output=True, text=True).stdout)["devices"]
    caps = [x for x in listing if x["capture"]]
    rep.data["capture_devices"] = [x["name"] for x in caps]
    rep.check("at least one capture device is present", len(caps) >= 1, len(caps))
    rc, res, err = run_cli(["record", "--synthetic", "--mic", "default", "--duration", 3, "--fps", 30, "--out", d / "default", "--name", "m"])
    if rep.check("default microphone: recorded", rc == 0 and res, f"rc={rc} {err[-200:]}"):
        m = check_telemetry(rep, res, expect_audio=("mic",))
        check_container(rep, res["path"], "h264", 720, 30, 2, 2.7, 3.6)
        x = decode_audio(res["path"], 1)
        rep.check("MIC track has the full length", abs(len(x) / RATE - 3.0) < 0.35, f"{len(x) / RATE:.2f} s")
        rep.data["mic_default_rms_db"] = m.get("audio.mic.rms_db")
    started, failed = 0, []
    for dev in caps:
        rc, res, err = run_cli(["record", "--synthetic", "--mic", "device:" + dev["id"], "--duration", 2, "--fps", 30,
                                "--out", d / "dev", "--name", "m"])
        if rc == 0 and res:
            started += 1
            x = decode_audio(res["path"], 1)
            rep.check(f"device '{dev['name']}': 2 s of audio", abs(len(x) / RATE - 2.0) < 0.35, f"{len(x) / RATE:.2f} s")
        else:
            failed.append((dev["name"], err.strip()[-120:]))
    rep.check("every capture device can be selected", not failed, failed)
    renders = [x for x in listing if not x["capture"]]
    for dev in renders[:2]:
        rc, res, err = run_cli(["record", "--synthetic", "--sys", "system", "--mic", "none", "--duration", 2, "--out", d / "out", "--name", "o"])
        rep.check("system output loopback records", rc == 0 and res, err[-150:])
        break
    return rep


def sc_source_closed(out):
    rep = Report("source_closed_midway")
    d = out / "closed"
    subj = Subject(d, title="VV TestSubject Short", hz=440, exit_after=4)
    try:
        t0 = time.time()
        rc, res, err = run_cli(["record", "--window-title", "VV TestSubject Short", "--sys", "app", "--mic", "none",
                                "--duration", 15, "--fps", 30, "--out", d / "rec", "--name", "closed"], timeout=60)
        took = time.time() - t0
        rep.check("recording ended by itself well before the 15 s asked", took < 11, f"{took:.1f} s")
        rep.check("result carries the reason", res and "closed" in res.get("error", ""), res and res.get("error"))
        if res and res.get("bytes", 0) > 0:
            info = probe(res["path"])
            rep.check("file is still a valid, finalised recording", info is not None and decodes_clean(res["path"])[0])
            dur = float(info["format"]["duration"]) if info else 0
            rep.check("duration matches the window's lifetime", 1.5 <= dur <= 5.5, f"{dur:.2f} s")
        else:
            rep.check("file produced", False, res)
    finally:
        subj.stop()
    return rep


def sc_errors(out):
    rep = Report("error_handling")
    rc, res, err = run_cli(["record", "--window-title", "no such window anywhere", "--duration", 1, "--out", out / "err"])
    rep.check("unknown window: clean failure, nonzero exit", rc != 0 and "no window matches" in err, f"rc={rc} {err.strip()[-100:]}")
    rc, res, err = run_cli(["record", "--hwnd", 12345, "--pid", 1, "--duration", 1, "--out", out / "err"])
    rep.check("dead window handle: clean failure", rc != 0 and "no longer exists" in err, f"rc={rc} {err.strip()[-100:]}")
    rc, res, err = run_cli(["record", "--synthetic", "--codec", "vp9", "--duration", 1, "--out", out / "err"])
    rep.check("bad codec rejected by validation", rc != 0 and "codec" in err, err.strip()[-100:])
    rc, res, err = run_cli(["record", "--synthetic", "--mic", "device:{0.0.1.00000000}.{does-not-exist}", "--duration", 1, "--out", out / "err"])
    rep.check("unknown audio device: clean failure", rc != 0 and "mic" in err, err.strip()[-120:])
    # a second instance on the same pipe is refused, the first keeps working
    p = popen_cli(["record", "--synthetic", "--duration", 6, "--pipe", "vv-e2e-dup", "--out", out / "err"],
                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        c = Pipe("vv-e2e-dup")
        rc2, _, err2 = run_cli(["record", "--synthetic", "--duration", 1, "--pipe", "vv-e2e-dup", "--out", out / "err"])
        rep.check("second instance on a busy pipe is refused", rc2 == 2 and "already in use" in err2, f"rc={rc2} {err2.strip()[-80:]}")
        rep.check("first instance unaffected", c.call(cmd="ping")["ok"])
        c.call(cmd="stop")
        c.close()
    finally:
        try:
            p.wait(15)
        except subprocess.TimeoutExpired:
            p.kill()
    return rep


def sc_longer_soak(out):
    rep = Report("soak_60s_drift")
    d = out / "soak"
    rc, res, err = run_cli(["record", "--synthetic", "--duration", 60, "--fps", 60, "--synthetic", "--height", 720, "--bitrate", 8000,
                            "--out", d, "--name", "soak"], timeout=150)
    rep.check("cli exit code 0", rc == 0, f"rc={rc}")
    if not res:
        return rep
    check_container(rep, res["path"], "h264", 720, 60, 2, 59.5, 61)
    rep.check("no dropped ticks in 60 s", res["droppedTicks"] <= 2, res["droppedTicks"])
    m = check_telemetry(rep, res)
    rep.check("encoder keeps up: mean encode time < 8 ms", m.get("video.encode_ms", 99) < 8, m.get("video.encode_ms"))
    rep.check("audio drift trimmed/padded under 150 ms total",
              (m.get("audio.sys.padded_ms", 0) + m.get("audio.sys.trimmed_ms", 0)) < 150,
              f"padded {m.get('audio.sys.padded_ms')} trimmed {m.get('audio.sys.trimmed_ms')}")
    luma, fps, vs = decode_luma(res["path"])
    fl, _ = flash_times(luma, fps, vs)
    on, _ = beep_onsets(decode_audio(res["path"], 0))
    # sync at the start and at the end of the hour-glass must agree: no drift
    head = av_offset(fl[fl < 10], on[on < 10])[0]
    tail = av_offset(fl[fl > 50], on[on > 50])[0]
    rep.check("A/V offset stable from first to last 10 s", head is not None and tail is not None and abs(head - tail) < 60,
              f"{head} ms -> {tail} ms")
    return rep


def sc_ui(out):
    rep = Report("tray_ui")
    d = out / "ui"
    d.mkdir(parents=True, exist_ok=True)
    subj = Subject(d, title="VV TestSubject UI", hz=440)
    noise = Subject(d, role="noise", hz=880)
    pipe_name = "vv-e2e-ui"
    gui = subprocess.Popen([str(GUI), "--pipe", pipe_name, "--show"], env={**os.environ, "VV_SETTINGS_DIR": str(d / "settings")})
    try:
        c = Pipe(pipe_name, wait=20)
        rep.check("gui answers ping over the control pipe", c.call(cmd="ping")["ok"])
        c.call(cmd="set_config", config={"outDir": str(d / "rec"), "name": "ui"})
        c.call(cmd="ui_override", config={"outDir": str(d / "rec"), "name": "ui", "mic": "none", "fps": 30, "telemetryDir": str(d / "telemetry")})
        s = c.call(cmd="ui_state")
        rep.check("tray icon is visible", s["trayVisible"])
        rep.check("tray icon state idle", s["icon"] == "idle", s["icon"])
        rep.check("tooltip names the app and state", s["tooltip"].startswith("Voloire-video | IDLE"), s["tooltip"])
        top = [m.get("text") for m in s["menu"] if not m.get("separator")]
        rep.check("menu has every top-level entry",
                  all(x in " ".join(top) for x in ("Start recording", "Pause", "Source", "Format", "MIC", "SYS", "Open panel", "Quit")), top)
        rep.check("pause is disabled while idle", [m for m in s["menu"] if m.get("text") == "Pause"][0]["enabled"] is False)
        src = [m for m in s["menu"] if m.get("text") == "Source"][0]["menu"]
        titles = [m.get("text", "") for m in src]
        rep.check("window list contains the test subject", any("VV TestSubject UI" in t for t in titles), len(titles))
        mic = [m for m in s["menu"] if m.get("text") == "MIC"][0]["menu"]
        rep.check("MIC menu lists default, devices, off", len(mic) >= 3 and mic[0]["text"] == "Default microphone" and mic[-1]["text"] == "Off", [m["text"] for m in mic])
        sysm = [m for m in s["menu"] if m.get("text") == "SYS"][0]["menu"]
        rep.check("SYS menu offers app audio, outputs, off", sysm[0]["text"].startswith("App audio") and sysm[-1]["text"] == "Off", [m["text"] for m in sysm])

        shots = c.call(cmd="ui_screenshot", dir=str(d / "shots"))
        rep.check("screenshots written", shots["ok"] and all(f["ok"] for f in shots["files"]), len(shots["files"]))
        panel = png_rgb(d / "shots" / "panel.png")
        px = panel.reshape(-1, 3).astype(int)
        dark = ((px.max(axis=1)) < 40).mean()
        neon = ((px[:, 2] > 180) & (px[:, 1] > 100) & (px[:, 0] < 80)).mean()
        rep.check("theme: panel is predominantly black", dark > 0.7, f"{dark:.1%}")
        rep.check("theme: neon blue accents present", neon > 0.002, f"{neon:.2%}")
        red = ((px[:, 0] > 180) & (px[:, 1] < 90) & (px[:, 2] < 120)).mean()
        rep.check("theme: no stray warm colors in the idle panel", red < 0.0005, f"{red:.3%}")
        icons = {n: png_rgb(d / "shots" / f"tray_{n}.png") for n in ("idle", "recording", "paused", "error")}
        rep.check("tray icons differ per state", len({a.tobytes() for a in icons.values()}) == 4)

        # drive a recording through the tray menu only
        c.call(cmd="ui_trigger", path=["Source", [m["text"] for m in src if "VV TestSubject UI" in m.get("text", "")][0]])
        c.call(cmd="ui_override", config={"outDir": str(d / "rec"), "name": "ui", "mic": "none", "fps": 30, "sys": "app", "telemetryDir": str(d / "telemetry")})
        rep.check("menu: Start recording accepted", c.call(cmd="ui_trigger", path=["Start recording"])["ok"])
        end = time.time() + 15
        while time.time() < end and c.call(cmd="ui_state")["state"] != "recording":
            time.sleep(0.2)
        s = c.call(cmd="ui_state")
        rep.check("state reaches recording", s["state"] == "recording", s["state"])
        rep.check("tray icon state recording", s["icon"] == "recording", s["icon"])
        rep.check("tooltip shows RECORDING and a timer", "RECORDING" in s["tooltip"] and "00:00:" in s["tooltip"], s["tooltip"])
        fmt = [m for m in s["menu"] if m.get("text") == "Format"][0]
        rep.check("format menu locked while recording", fmt["enabled"] is False)
        time.sleep(2.5)
        t1 = c.call(cmd="ui_state")["timer"]
        rep.check("timer advances", t1 >= "00:00:02", t1)
        rep.check("menu: Pause accepted", c.call(cmd="ui_trigger", path=["Pause"])["ok"])
        time.sleep(0.4)
        s = c.call(cmd="ui_state")
        rep.check("tray icon state paused", s["icon"] == "paused" and s["state"] == "paused", s["icon"])
        tp = s["timer"]
        time.sleep(1.5)
        rep.check("timer frozen during pause", c.call(cmd="ui_state")["timer"] == tp, tp)
        rep.check("menu: Resume accepted", c.call(cmd="ui_trigger", path=["Resume"])["ok"])
        time.sleep(2.5)
        rep.check("menu: Stop recording accepted", c.call(cmd="ui_trigger", path=["Stop recording"])["ok"])
        end = time.time() + 20
        while time.time() < end and c.call(cmd="ui_state")["state"] != "idle":
            time.sleep(0.2)
        s = c.call(cmd="ui_state")
        rep.check("state returns to idle", s["state"] == "idle", s["state"])
        rep.check("tray icon back to idle", s["icon"] == "idle", s["icon"])
        files = sorted((d / "rec").glob("ui_*.mkv"))
        if rep.check("recording file produced by the tray", len(files) == 1, [f.name for f in files]):
            info = check_container(rep, files[0], "h264", None, 30, 1, 4.6, 7.0)
            tele = list((d / "telemetry").glob("result.json"))
            rep.check("telemetry written for the UI session", len(tele) == 1)
            rep.data["ui_file"] = str(files[0])
            check_sync_and_tones(rep, files[0], expect_flashes=(4, 8), tol_ms=150, check_mic_tone=False)
        # settings through the menu while idle (drop the test override so the menu is the only source)
        c.call(cmd="ui_override", config={})
        rep.check("menu: Format > Codec > HEVC", c.call(cmd="ui_trigger", path=["Format", "Codec", "HEVC (H.265)"])["ok"])
        rep.check("menu: Format > Resolution > 720p", c.call(cmd="ui_trigger", path=["Format", "Resolution", "720p"])["ok"])
        rep.check("menu: Format > Container > MP4", c.call(cmd="ui_trigger", path=["Format", "Container", "MP4"])["ok"])
        cfg = c.call(cmd="get_config")["config"]
        rep.check("settings reached the controller", (cfg["codec"], cfg["outHeight"], cfg["container"]) == ("hevc", 720, "mp4"), cfg)
        rep.check("menu: MIC > Off", c.call(cmd="ui_trigger", path=["MIC", "Off"])["ok"])
        rep.check("menu: SYS > Off", c.call(cmd="ui_trigger", path=["SYS", "Off"])["ok"])
        cfg = c.call(cmd="get_config")["config"]
        rep.check("audio channels switched off via the menu", (cfg["mic"], cfg["sys"]) == ("none", "none"), (cfg["mic"], cfg["sys"]))
        c.call(cmd="shutdown")
        c.close()
        try:
            gui.wait(15)
            rep.check("gui exits on shutdown", True)
        except subprocess.TimeoutExpired:
            rep.check("gui exits on shutdown", False)
    finally:
        if gui.poll() is None:
            gui.kill()
        subj.stop()
        noise.stop()
    return rep


def sc_installers(out):
    import sc_installers
    return sc_installers.run(out)


SCENARIOS = {
    "synthetic_basic": sc_synthetic_basic,
    "pause_resume": sc_pause_resume,
    "formats": sc_formats,
    "window_app_audio": sc_window_app_audio,
    "borderless": sc_borderless,
    "window_scaled": sc_window_resolution_scaling,
    "monitor": sc_monitor_capture,
    "mic": sc_mic_real,
    "source_closed": sc_source_closed,
    "errors": sc_errors,
    "ui": sc_ui,
    "soak": sc_longer_soak,
    "installers": sc_installers,
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(Path(tempfile.gettempdir()) / "vv-e2e"))
    ap.add_argument("--only", default="")
    ap.add_argument("--skip", default="")
    ap.add_argument("--fast", action="store_true", help="skip the 60 s soak")
    a = ap.parse_args()
    out = Path(a.out)
    if out.exists():
        shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    only = [s for s in a.only.split(",") if s]
    skip = [s for s in a.skip.split(",") if s] + (["soak"] if a.fast else [])
    if not only and "installers" not in skip and not (REPO / "dist").exists():
        skip.append("installers")  # only meaningful after scripts/package.ps1
    reports = []
    for name, fn in SCENARIOS.items():
        if (only and name not in only) or name in skip:
            continue
        t0 = time.time()
        try:
            rep = fn(out)
        except Exception as e:  # a scenario that blows up is a failed check, not a crashed run
            rep = Report(name)
            rep.check("scenario ran to completion", False, f"{type(e).__name__}: {e}")
            traceback.print_exc()
        kill_subjects()
        rep.data["seconds"] = round(time.time() - t0, 1)
        rep.print()
        reports.append(rep)
    total = sum(len(r.checks) for r in reports)
    failed = sum(len(r.failed) for r in reports)
    (out / "report.json").write_text(json.dumps(
        [{"scenario": r.name, "checks": r.checks, "data": r.data} for r in reports], indent=2, default=str), encoding="utf-8")
    print(f"\n{total - failed}/{total} checks passed; report: {out / 'report.json'}")
    sys.exit(min(failed, 100))


if __name__ == "__main__":
    main()
