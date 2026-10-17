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
    if audio == 3:
        rep.check("media: tracks titled MIX, SYS, MIC", titles == ["MIX", "SYS", "MIC"], titles)
        rep.check("media: MIX is the default track", a[0].get("disposition", {}).get("default") == 1 and
                  all(x.get("disposition", {}).get("default") == 0 for x in a[1:]))
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
    sysx = decode_audio(path, track_index(path, "SYS") or 0)
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
        micx = decode_audio(path, track_index(path, "MIC"))
        f = dominant_freq(micx[RATE:])
        mixx = decode_audio(path, track_index(path, "MIX"))
        rep.check("MIX: carries both the SYS tone and the MIC tone",
                  band_power(mixx, sys_hz) > 0 and band_power(mixx, mic_hz) / max(band_power(micx, mic_hz), 1e-12) > 0.5 and
                  band_power(mixx, sys_hz) / max(band_power(sysx, sys_hz), 1e-12) > 0.5,
                  f"sys {band_power(mixx, sys_hz):.2e} mic {band_power(mixx, mic_hz):.2e}")
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
    check_container(rep, res["path"], "h264", 720, 30, 3, 5.7, 6.4)
    m = check_telemetry(rep, res)
    rep.check("telemetry: video.fps_actual close to 30", 27 <= m.get("video.fps_actual", 0) <= 33, m.get("video.fps_actual"))
    rep.check("telemetry: SYS level not silent", m.get("audio.sys.max_peak_db", -120) > -20, m.get("audio.sys.max_peak_db"))
    rep.check("telemetry: MIC level not silent", m.get("audio.mic.max_peak_db", -120) > -30, m.get("audio.mic.max_peak_db"))
    check_sync_and_tones(rep, res["path"], expect_flashes=(5, 7), isolation=False)
    vtimes_ok(rep, res["path"])
    return rep


def sc_quality_presets(out):
    rep = Report("quality_presets")
    d = out / "quality"
    rows = {"high": 13000, "standard": 4900, "light": 2600, "minimal": 1300}
    sizes = {}
    for preset, kbps in rows.items():
        rc, res, err = run_cli(["record", "--synthetic", "--duration", 4, "--fps", 30, "--height", 720,
                                "--encoder", "mf",  # matches CI (auto -> mf); stable bitrate relation on a static picture
                                "--quality", preset, "--out", d / preset, "--name", "q"], timeout=90)
        if not rep.check(f"{preset}: recorded", rc == 0 and res, f"rc={rc} {err[-160:]}"):
            continue
        rj = Path(res["telemetryDir"]) / "result.json"
        m = json.loads(rj.read_text(encoding="utf-8")).get("metrics", {})
        rep.check(f"{preset}: video.bitrate_kbps is {kbps}", m.get("video.bitrate_kbps") == kbps, m.get("video.bitrate_kbps"))
        sizes[preset] = Path(res["path"]).stat().st_size
        check_container(rep, res["path"], "h264", 720, 30, 3, 3.6, 4.5)
    # On a near-static synthetic picture the encoder spends content-driven bits (~670 kbps on
    # mf) far below every preset target, so preset-vs-preset sizes differ only by encoder noise
    # (measured +/-2%): the size responds only when the rate cap binds. High preset against a
    # 300 kbps custom run pairs the two ends of the same knob.
    rc, res, err = run_cli(["record", "--synthetic", "--duration", 4, "--fps", 30, "--height", 720,
                            "--encoder", "mf", "--bitrate", "300", "--out", d / "rate300", "--name", "q"], timeout=90)
    if rep.check("custom 300 kbps: recorded", rc == 0 and res, f"rc={rc} {err[-160:]}"):
        rj = Path(res["telemetryDir"]) / "result.json"
        m = json.loads(rj.read_text(encoding="utf-8")).get("metrics", {})
        rep.check("custom 300 kbps: video.bitrate_kbps is 300", m.get("video.bitrate_kbps") == 300,
                  m.get("video.bitrate_kbps"))
        sizes["custom300"] = Path(res["path"]).stat().st_size
        check_container(rep, res["path"], "h264", 720, 30, 3, 3.6, 4.5)
    if "high" in sizes and "custom300" in sizes:
        rep.check("a binding rate cap shrinks the file (high preset > 300 kbps custom)",
                  sizes["high"] > sizes["custom300"], sizes)
    rc, res, err = run_cli(["record", "--synthetic", "--duration", 3, "--fps", 30, "--height", 1080, "--encoder", "mf",
                            "--quality", "minimal", "--out", d / "cap", "--name", "cap"], timeout=90)
    if rep.check("minimal at 1080: recorded", rc == 0 and res, f"rc={rc} {err[-160:]}"):
        info = probe(res["path"])
        v = streams(info, "video")[0] if info else {}
        rep.check("minimal caps the height at 720", v.get("height") == 720, v.get("height"))
    return rep


def sc_share_copy(out):
    rep = Report("share_copy")
    d = out / "share_copy"
    # 20 s of 1080p60 high quality in a non-ASCII directory: the share copy lands beside it.
    # Without the continuous MIC tone the SYS beeps stay recognisable in the single shared
    # audio track, which is what the A/V sync check on track 0 measures.
    src_dir = d / "Città"
    rc, res, err = run_cli(["record", "--synthetic", "--mic", "none", "--duration", 20, "--fps", 60, "--height", 1080,
                            "--encoder", "mf",  # matches CI (auto -> mf); the source encoder must not change the verdicts
                            "--quality", "high", "--out", src_dir, "--name", "big"], timeout=240)
    if not rep.check("source: 20 s 1080p60 high recorded", rc == 0 and res, f"rc={rc} {err[-200:]}"):
        return rep
    rc, share, err = run_cli(["share", res["path"], "--target-mb", 8], timeout=300)
    if not rep.check("share: exit code 0", rc == 0 and share, f"rc={rc} {err[-200:]}"):
        return rep
    sp = Path(str(share.get("path", "")))
    rep.check("share: named after the source and the target", sp.name == Path(res["path"]).stem + "_share_8MB.mp4",
              sp.name)
    rep.check("share: beside the source in the non-ASCII dir", sp.exists() and sp.parent == Path(res["path"]).parent,
              str(sp))
    size = sp.stat().st_size if sp.exists() else 0
    rep.check("share: size <= 8 MiB", 0 < size <= 8 * 1024 * 1024, size)
    rep.check("share: reported bytes match the file", share.get("bytes") == size, f"{share.get('bytes')} vs {size}")
    info = probe(sp)
    if info:
        rep.check("share: one video stream", len(streams(info, "video")) == 1)
        a = streams(info, "audio")
        rep.check("share: exactly 1 audio stream", len(a) == 1, len(a))
    ok, why = decodes_clean(sp)
    rep.check("share: decodes end to end with no error", ok, why)
    check_sync_and_tones(rep, sp, tol_ms=150, check_mic_tone=False, isolation=False)
    head = sp.read_bytes()[:65536]
    i, j = head.find(b"moov"), head.find(b"mdat")
    rep.check("share: moov atom before mdat", i >= 0 and j >= 0 and i < j, f"moov@{i} mdat@{j}")

    # A recording with no audio at all shares into a video-only file.
    rc, res, err = run_cli(["record", "--synthetic", "--duration", 3, "--fps", 30, "--sys", "none", "--mic", "none",
                            "--out", d / "mute", "--name", "mute"], timeout=90)
    if not rep.check("mute: 3 s recorded without audio", rc == 0 and res, f"rc={rc} {err[-200:]}"):
        return rep
    rc, share, err = run_cli(["share", res["path"], "--target-mb", 8], timeout=120)
    if rep.check("mute share: exit code 0", rc == 0 and share, f"rc={rc} {err[-200:]}"):
        info = probe(share["path"])
        a = streams(info, "audio") if info else None
        rep.check("mute share: 0 audio streams", a is not None and len(a) == 0, len(a) if a is not None else "no probe")

    # 35 s cannot fit in 1 MB with any sane bitrate: the CLI must refuse and leave nothing behind.
    rc, res, err = run_cli(["record", "--synthetic", "--duration", 35, "--quality", "minimal",
                            "--out", d / "long", "--name", "long"], timeout=240)
    if not rep.check("long: 35 s minimal recorded", rc == 0 and res, f"rc={rc} {err[-200:]}"):
        return rep
    rc, share, err = run_cli(["share", res["path"], "--target-mb", 1], timeout=120)
    rep.check("long share: exit code 5", rc == 5, f"rc={rc}")
    msg = str((share or {}).get("error", ""))
    rep.check('long share: error says "at most 27 s"', "at most 27 s" in msg, msg)
    leftovers = sorted(p.name for p in Path(res["path"]).parent.iterdir() if "_share_" in p.name or p.suffix == ".part")
    rep.check("long share: no share or .part file left", not leftovers, leftovers)
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
    info = check_container(rep, res["path"], "h264", 720, 30, 3, 3.6, 4.5)
    rep.check("file is 4 s of media, not 6 s of wall time (pause removed)", info and 3.6 <= float(info["format"]["duration"]) <= 4.5)
    m = check_telemetry(rep, res)
    luma, fps, vs = decode_luma(res["path"])
    fl, _ = flash_times(luma, fps, vs)
    rep.check("video: 3..5 flashes in 4 s of media", 3 <= len(fl) <= 5, len(fl))
    sysx = decode_audio(res["path"], track_index(res["path"], "SYS"))
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
        rep.check(f"{tag}: MIX, SYS, MIC tracks", info and len(streams(info, "audio")) == 3)
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
        check_container(rep, res["path"], "h264", 720, 30, 3, 2.7, 3.6)
        x = decode_audio(res["path"], track_index(res["path"], "MIC"))
        rep.check("MIC track has the full length", abs(len(x) / RATE - 3.0) < 0.35, f"{len(x) / RATE:.2f} s")
        rep.data["mic_default_rms_db"] = m.get("audio.mic.rms_db")
    started, failed = 0, []
    for dev in caps:
        rc, res, err = run_cli(["record", "--synthetic", "--mic", "device:" + dev["id"], "--duration", 2, "--fps", 30,
                                "--out", d / "dev", "--name", "m"])
        if rc == 0 and res:
            started += 1
            x = decode_audio(res["path"], track_index(res["path"], "MIC"))
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
    check_container(rep, res["path"], "h264", 720, 60, 3, 59.5, 61)
    rep.check("no dropped ticks in 60 s", res["droppedTicks"] <= 2, res["droppedTicks"])
    m = check_telemetry(rep, res)
    rep.check("encoder keeps up: mean encode time < 8 ms", m.get("video.encode_ms", 99) < 8, m.get("video.encode_ms"))
    rep.check("audio drift trimmed/padded under 150 ms total",
              (m.get("audio.sys.padded_ms", 0) + m.get("audio.sys.trimmed_ms", 0)) < 150,
              f"padded {m.get('audio.sys.padded_ms')} trimmed {m.get('audio.sys.trimmed_ms')}")
    luma, fps, vs = decode_luma(res["path"])
    fl, _ = flash_times(luma, fps, vs)
    on, _ = beep_onsets(decode_audio(res["path"], track_index(res["path"], "SYS")))
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
        rep.check("menu: Quality > Light", c.call(cmd="ui_trigger", path=["Quality", "Light"])["ok"])
        rep.check("menu: Share copy > 10 MB", c.call(cmd="ui_trigger", path=["Share copy", "10 MB"])["ok"])
        cfg = c.call(cmd="get_config")["config"]
        rep.check("quality reached the controller", cfg["quality"] == "light", cfg.get("quality"))
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
        # Share copy > 10 MB: the copy of the file just recorded ran by itself.
        end = time.time() + 60
        while time.time() < end and c.call(cmd="ui_state")["share"]["state"] != "done":
            time.sleep(0.2)
        sh = c.call(cmd="ui_state")["share"]
        if rep.check("share: auto-started and done", sh["state"] == "done", sh.get("error", "")):
            sp = Path(sh["path"])
            rep.check("share: file exists", sp.exists(), str(sp))
            rep.check("share: size <= 10 MiB", sp.exists() and 0 < sp.stat().st_size <= 10 * 1024 * 1024,
                      sp.stat().st_size if sp.exists() else 0)
            rep.check("share: on the clipboard", c.call(cmd="ui_state")["clipboardFiles"] == [sh["path"]],
                      c.call(cmd="ui_state")["clipboardFiles"])
            sp.unlink(missing_ok=True)
            rep.check("menu: Share copy > Make share copy of last recording",
                      c.call(cmd="ui_trigger", path=["Share copy", "Make share copy of last recording"])["ok"])
            end = time.time() + 60
            while time.time() < end:
                st = c.call(cmd="ui_state")
                if st["share"]["state"] == "done" and Path(st["share"]["path"]).exists():
                    break
                time.sleep(0.2)
            st = c.call(cmd="ui_state")
            rep.check("share re-run: file is back", st["share"]["state"] == "done" and Path(st["share"]["path"]).exists(),
                      st["share"])
            rep.check("share re-run: on the clipboard again", st["clipboardFiles"] == [st["share"]["path"]],
                      st["clipboardFiles"])
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


def _frame_rgb(path, t):
    """One decoded frame at t seconds as an (h, w, 3) uint8 array."""
    info = probe(path)
    s = streams(info, "video")[0]
    p = subprocess.run([FFMPEG, "-v", "error", "-ss", str(t), "-i", str(path), "-frames:v", "1",
                        "-f", "rawvideo", "-pix_fmt", "rgb24", "-"], capture_output=True)
    return np.frombuffer(p.stdout, dtype=np.uint8).reshape(s["height"], s["width"], 3)


def _magenta_share(frame):
    if frame.size == 0:
        return None
    px = frame.astype(int)
    return float(((px[..., 0] > 180) & (px[..., 1] < 80) & (px[..., 2] > 180)).mean())


def sc_hud(out):
    rep = Report("hud")
    d = out / "hud"
    d.mkdir(parents=True, exist_ok=True)
    import ctypes
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)  # physical pixels from GetSystemMetrics
    except Exception:
        pass
    width = ctypes.windll.user32.GetSystemMetrics(0)
    height = ctypes.windll.user32.GetSystemMetrics(1)

    def wait_state(c, want, timeout=20):
        end = time.time() + timeout
        while time.time() < end:
            if c.call(cmd="ui_state")["state"] == want:
                return True
            time.sleep(0.2)
        return False

    subj = Subject(d, title="VV TestSubject HUD", hz=440)
    gui = Gui(d, "vv-e2e-hud")
    try:
        c = gui.c

        def hud_state():
            return c.call(cmd="ui_state").get("hud") or {}

        rep.check("gui answers ping over the control pipe", c.call(cmd="ping")["ok"])
        c.call(cmd="ui_override",
               config={"outDir": str(d / "rec"), "name": "hud", "mic": "none", "sys": "none", "fps": 30,
                       "telemetryDir": str(d / "telemetry")})
        hud = hud_state()
        rep.check("hud: ui_state carries the bar", bool(hud), sorted(hud.keys()))
        rep.check("hud: visible and idle", hud.get("visible") is True and hud.get("dot") == "idle", hud.get("dot"))
        rep.check("hud: excluded from capture", hud.get("excludedFromCapture") is True)
        x, y, w, h = (hud.get(k, -1) for k in ("x", "y", "w", "h"))
        rep.check("hud: inside the primary monitor",
                  0 <= x and 0 <= y and w > 0 and h > 0 and x + w <= width and y + h <= height,
                  (x, y, w, h, width, height))
        rep.check("hud: within 40 px of the screen top", 0 <= y <= 40, y)

        # One shared menu: the bar's copy drives the same settings as the tray's.
        rep.check("hud menu: Format > Codec > HEVC",
                  c.call(cmd="ui_trigger", path=["Format", "Codec", "HEVC (H.265)"], surface="hud")["ok"])
        rep.check("hud menu drove the config", c.call(cmd="get_config")["config"]["codec"] == "hevc")
        s = c.call(cmd="ui_state")
        tray_menu, hud_menu = s.get("menu", []), (s.get("hud") or {}).get("menu", [])
        rep.check("hud and tray share one menu below the header line",
                  bool(hud_menu) and hud_menu == tray_menu[2:], f"hud {len(hud_menu)} vs tray {len(tray_menu)} entries")
        c.call(cmd="ui_trigger", path=["Format", "Codec", "H.264"], surface="hud")

        shots = c.call(cmd="ui_screenshot", dir=str(d / "shots"))
        rep.check("screenshots written", shots.get("ok") and all(f.get("ok") for f in shots.get("files", [])))
        idle_png = d / "shots" / "hud_idle.png"
        rep.check("hud_idle.png written", idle_png.exists() and idle_png.stat().st_size > 0)
        if idle_png.exists():
            rep.check("hud_idle.png has pill pixels", int(png_rgb(idle_png).max()) > 100)

        # Record a window from the bar's menu.
        s = c.call(cmd="ui_state")
        src = [m for m in s["menu"] if m.get("text") == "Source"][0]["menu"]
        item = [m["text"] for m in src if "VV TestSubject HUD" in m.get("text", "")][0]
        rep.check("menu: Source > test window", c.call(cmd="ui_trigger", path=["Source", item])["ok"])
        c.call(cmd="ui_hide")
        rep.check("hud menu: Start recording", c.call(cmd="ui_trigger", path=["Start recording"], surface="hud")["ok"])
        rep.check("hud: reaches recording", wait_state(c, "recording", 15), hud_state().get("dot"))
        t1 = hud_state().get("timer")
        rep.check("hud: recording dot", hud_state().get("dot") == "recording", hud_state().get("dot"))
        time.sleep(2.2)
        t2 = hud_state().get("timer")
        rep.check("hud: timer advances >= 2 s", (t2 or "") >= "00:00:02" and t1 != t2, f"{t1} -> {t2}")
        c.call(cmd="ui_screenshot", dir=str(d / "shots"))
        rec_png = d / "shots" / "hud_recording.png"
        rep.check("hud_recording.png written", rec_png.exists() and rec_png.stat().st_size > 0)
        c.call(cmd="ui_hide")
        rep.check("hud menu: Pause", c.call(cmd="ui_trigger", path=["Pause"], surface="hud")["ok"])
        time.sleep(0.4)
        rep.check("hud: paused dot", hud_state().get("dot") == "paused")
        rep.check("hud menu: Stop recording", c.call(cmd="ui_trigger", path=["Stop recording"], surface="hud")["ok"])
        rep.check("hud: back to idle", wait_state(c, "idle", 20))
        rep.check("hud: idle dot after the stop", hud_state().get("dot") == "idle")
        files = sorted((d / "rec").glob("hud_*.mkv"))
        if rep.check("recording through the bar produced a file", len(files) == 1, [f.name for f in files]):
            rep.check("recording decodes clean", decodes_clean(files[0]))

        # Capture exclusion, both ways, over a monitor recording.
        def monitor_shot(name):
            c.call(cmd="ui_override", config={"outDir": str(d / "rec"), "name": name, "mic": "none", "sys": "none", "fps": 30,
                                              "videoSource": "monitor", "monitor": 0, "telemetryDir": str(d / "telemetry")})
            c.call(cmd="ui_trigger", path=["Start recording"])
            wait_state(c, "recording", 15)
            time.sleep(2.2)
            c.call(cmd="ui_trigger", path=["Stop recording"])
            wait_state(c, "idle", 20)
            fs = sorted((d / "rec").glob(name + "_*.mkv"))
            return fs[-1] if fs else None

        def crop_magenta(path):
            info = probe(path)
            v = streams(info, "video")[0]
            hud = hud_state()
            if not all(k in hud for k in ("x", "y", "w", "h")):
                return None, (0, 0)
            sx, sy = v["width"] / width, v["height"] / height  # physical screen -> frame pixels
            fr = _frame_rgb(path, min(1.0, float(info["format"]["duration"]) / 2))
            x0, y0 = max(0, int(hud["x"] * sx)), max(0, int(hud["y"] * sy))
            x1, y1 = int((hud["x"] + hud["w"]) * sx), int((hud["y"] + hud["h"]) * sy)
            return _magenta_share(fr[y0:y1, x0:x1]), (x1 - x0, y1 - y0)

        c.call(cmd="ui_hud", testPattern=True)
        shot = monitor_shot("excl")
        if rep.check("exclusion: monitor recording produced", shot is not None):
            share, dims = crop_magenta(shot)
            rep.check("exclusion: the test pattern stays out of the capture",
                      share is not None and share < 0.005, f"{share} of {dims}")
        c.call(cmd="ui_hud", exclude=False)
        shot = monitor_shot("exclctl")
        if rep.check("exclusion control: monitor recording produced", shot is not None):
            share, dims = crop_magenta(shot)
            rep.check("exclusion control: the test pattern is visible in the capture",
                      share is not None and share > 0.2, f"{share} of {dims}")
        c.call(cmd="ui_hud", exclude=True, testPattern=False)

        # A position off every screen snaps back to the default at the next start.
        c.call(cmd="ui_hud", x=-5000, y=-5000)
    finally:
        gui.close()

    gui = Gui(d, "vv-e2e-hud-r1")
    try:
        c = gui.c
        hud = c.call(cmd="ui_state").get("hud") or {}
        x, y, w, h = (hud.get(k, -1) for k in ("x", "y", "w", "h"))
        rep.check("hud: off-screen position snapped back to a monitor",
                  w > 0 and h > 0 and x + w > 0 and y + h > 0 and x < width and y < height, (x, y, w, h))
        rep.check("hud: visible after the relaunch", hud.get("visible") is True)
        rep.check("menu: Show status bar unchecked", c.call(cmd="ui_trigger", path=["Show status bar"])["ok"])
        time.sleep(0.3)
        rep.check("hud: hidden after unchecking",
                  ((c.call(cmd="ui_state").get("hud") or {}).get("visible")) is False)
    finally:
        gui.close()
    gui = Gui(d, "vv-e2e-hud-r2")
    try:
        rep.check("hud: still hidden after the next relaunch",
                  ((gui.c.call(cmd="ui_state").get("hud") or {}).get("visible")) is False)
    finally:
        gui.close()
    subj.stop()
    return rep


def sc_harness_selftest(out):
    from fakes import selftest
    return selftest.run(out)


# ------------------------------------------------------------------ streaming

def _last_json_line(text):
    for line in reversed((text or "").strip().splitlines()):
        try:
            return json.loads(line)
        except ValueError:
            continue
    return None


def _wait_stream_state(c, want, timeout):
    end = time.time() + timeout
    while time.time() < end:
        try:
            if (c.call(cmd="status").get("stream") or {}).get("state") == want:
                return True
        except (EOFError, OSError):
            return False
        time.sleep(0.3)
    return False


def _wait_reconnects(c, n, timeout):
    end = time.time() + timeout
    while time.time() < end:
        try:
            if int((c.call(cmd="status").get("stream") or {}).get("reconnects", 0)) >= n:
                return True
        except (EOFError, OSError):
            return False
        time.sleep(0.3)
    return False


def sc_stream_rtmp(out):
    rep = Report("stream_rtmp")
    d = out / "stream_rtmp"
    d.mkdir(parents=True, exist_ok=True)
    from fakes.rtmp_ingest import RtmpIngest
    if not rep.check("ffmpeg build has the rtmp protocol", capabilities()["rtmp"]):
        return rep
    key = "sk-live-rtmp-abcd1234"
    ing = RtmpIngest(d, key=key)
    ing.start()
    p = None
    try:
        p = popen_cli(["record", "--synthetic", "--mic", "none", "--stream", ing.url, "--stream-key", key,
                       "--duration", 10, "--fps", 30, "--out", d / "out", "--pipe", "vv-e2e-stream"],
                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        c = Pipe("vv-e2e-stream")
        rep.check("ingest: connection received", ing.wait_connected(20))
        time.sleep(3)
        s = c.call(cmd="status")
        st = s.get("stream") or {}
        rep.check("status: output is stream", s.get("output") == "stream", s.get("output"))
        rep.check("status: stream state live", st.get("state") == "live", st.get("state"))
        rep.check("status: kbps reported while live", float(st.get("kbps", 0)) > 0, st.get("kbps"))
        status_dump = json.dumps(s)
        logs_dump = json.dumps(c.call(cmd="logs", n=500))
        stdout, stderr = p.communicate(timeout=60)
        rep.check("exit code 0", p.returncode == 0, f"rc={p.returncode} {stderr[-200:]}")
        res = _last_json_line(stdout) or {}
        rep.check("no file is written", res.get("path", "") == "", res.get("path"))
        rep.check("bytes sent are counted", res.get("bytes", 0) > 0, res.get("bytes"))
        rj = Path(str(res.get("telemetryDir", ""))) / "result.json"
        metrics = json.loads(rj.read_text(encoding="utf-8"))["metrics"] if rj.exists() else {}
        kbps = metrics.get("stream.kbps", 0)
        rep.check("metric stream.kbps within 300..7800", 300 <= kbps <= 7800, kbps)
        ing.stop()
        if rep.check("ingest: received file written", ing.received.exists()):
            info = probe(ing.received)
            if rep.check("ingest: probes", info is not None):
                v, a = streams(info, "video"), streams(info, "audio")
                rep.check("1 H.264 video at 30 fps",
                          len(v) == 1 and v[0]["codec_name"] == "h264" and abs(fps_of(v[0]) - 30) < 0.5,
                          (v[0]["codec_name"], v[0]["r_frame_rate"]) if v else "no video")
                rep.check("exactly 1 audio stream", len(a) == 1, len(a))
                dur = float(info["format"]["duration"])
                rep.check("duration within 9..11 s", 9 <= dur <= 11, f"{dur:.2f}")
            ok, why = decodes_clean(ing.received)
            rep.check("ingest: decodes clean", ok, why)
            check_sync_and_tones(rep, ing.received, expect_flashes=(8, 11), tol_ms=150, check_mic_tone=False,
                                 isolation=False)
        hits = scan_for_secrets([Path(str(res.get("telemetryDir", ""))), status_dump, logs_dump], [key])
        rep.check("no key leak in telemetry or the pipe replies", not hits, hits)
    finally:
        ing.stop()
        if p is not None and p.poll() is None:
            p.kill()
    return rep


def sc_stream_stored_key(out):
    rep = Report("stream_stored_key")
    d = out / "stream_stored_key"
    d.mkdir(parents=True, exist_ok=True)
    from fakes.rtmp_ingest import RtmpIngest
    key = "sk-live-stored-wxyz9876"
    settings = d / "settings"
    gui = Gui(d, "vv-e2e-keygui")
    try:
        c = gui.c
        r = c.call(cmd="set_stream_key", key=key)
        rep.check("set_stream_key: stored", r.get("ok") is True and r.get("streamKeySet") is True, r)
        rep.check("set_stream_key: the key is not echoed back", key not in json.dumps(r))
        g = c.call(cmd="get_config")
        rep.check("get_config: streamKeySet true", g["config"].get("streamKeySet") is True,
                  g["config"].get("streamKeySet"))
        rep.check("get_config: the key is not echoed back", key not in json.dumps(g))
        blob = settings / "secrets" / "stream_key.bin"
        rep.check("secret file exists", blob.exists())
        rep.check("secret file holds no plaintext", blob.exists() and key.encode() not in blob.read_bytes())
        rep.check("settings dir holds no plaintext anywhere", not scan_for_secrets([settings], [key]))
        r = c.call(cmd="set_stream_key", key="")
        rep.check("clearing the key flips the flag", r.get("streamKeySet") is False, r)
        rep.check("cleared secret file is gone", not blob.exists())
    finally:
        gui.close()
    ing = RtmpIngest(d, key=key)
    ing.start()
    try:
        env = {**os.environ, "VV_SETTINGS_DIR": str(settings)}
        rc, res, err = run_cli(["record", "--synthetic", "--stream", ing.url, "--duration", 3], env=env, timeout=60)
        rep.check("no stored key: exit 3", rc == 3, f"rc={rc} {err.strip()[-150:]}")
        rep.check('no stored key: error starts with "no stream key"', "no stream key" in err, err.strip()[-150:])
        rep.check("no stored key: no connection is ever made", not ing.wait_connected(6))
    finally:
        ing.stop()
    return rep


def sc_stream_reconnect(out):
    rep = Report("stream_reconnect")
    d = out / "stream_reconnect"
    d.mkdir(parents=True, exist_ok=True)
    from fakes.rtmp_ingest import RtmpIngest
    key = "sk-live-recon-mnop4321"
    ing = RtmpIngest(d, key=key)
    ing.start()
    p = None
    try:
        p = popen_cli(["record", "--synthetic", "--mic", "none", "--stream", ing.url, "--stream-key", key,
                       "--duration", 14, "--fps", 30, "--out", d / "out", "--pipe", "vv-e2e-reconnect"],
                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        c = Pipe("vv-e2e-reconnect")
        rep.check("ingest: first connection received", ing.wait_connected(20))
        rep.check("stream: live", _wait_stream_state(c, "live", 15))
        time.sleep(4)
        ing.kill()
        time.sleep(1.5)
        ing2 = RtmpIngest(d, port=ing.port, key=key)
        ing2.start()
        rep.check("stream.reconnects reaches 1", _wait_reconnects(c, 1, 45))
        stdout, stderr = p.communicate(timeout=60)
        rep.check("exit code 0", p.returncode == 0, f"rc={p.returncode} {stderr[-200:]}")
        res = _last_json_line(stdout) or {}
        tdir = Path(str(res.get("telemetryDir", "")))
        ing2.stop()
        if rep.check("ingest 2: received file written", ing2.received.exists()):
            info = probe(ing2.received)
            if rep.check("ingest 2: probes", info is not None):
                dur = float(info["format"]["duration"])
                rep.check("ingest 2: duration >= 3 s", dur >= 3, f"{dur:.2f}")
            ok, why = decodes_clean(ing2.received)
            rep.check("ingest 2: decodes clean", ok, why)
        ev = tdir / "events.jsonl"
        text = ev.read_text(encoding="utf-8", errors="replace") if ev.exists() else ""
        i = text.rfind("stream: reconnecting")
        j = text.find("stream: live", i + 1) if i >= 0 else -1
        rep.check("events: reconnecting then live", i >= 0 and j > i)
    finally:
        ing.kill()
        if p is not None and p.poll() is None:
            p.kill()
    return rep


def sc_stream_stall(out):
    rep = Report("stream_stall")
    d = out / "stream_stall"
    d.mkdir(parents=True, exist_ok=True)
    from fakes.rtmp_ingest import RtmpIngest
    key = "sk-live-stall-qrst5678"
    ing = RtmpIngest(d, key=key)
    ing.start()
    p = None
    duration = 24
    t0 = time.time()
    try:
        p = popen_cli(["record", "--synthetic", "--mic", "none", "--stream", ing.url, "--stream-key", key,
                       "--duration", duration, "--fps", 30, "--out", d / "out", "--pipe", "vv-e2e-stall"],
                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        c = Pipe("vv-e2e-stall")
        rep.check("ingest: connection received", ing.wait_connected(20))
        rep.check("stream: live", _wait_stream_state(c, "live", 15))
        time.sleep(3)
        ing.freeze()  # the listener stops reading: TCP stays up, nothing flows
        frozen = time.time()
        became = False
        while time.time() < frozen + 8:
            st = c.call(cmd="status").get("stream") or {}
            if st.get("state") == "reconnecting":
                became = True
                break
            time.sleep(0.4)
        rep.check("stall: state is reconnecting within 8 s", became)
        dropped = c.call(cmd="status")["metrics"].get("video.dropped_ticks", 0)
        rep.check("stall: video.dropped_ticks <= 2", dropped <= 2, dropped)
        time.sleep(1.5)
        ing.thaw()
        ing.kill()
        ing2 = RtmpIngest(d, port=ing.port, key=key)
        ing2.start()
        stdout, stderr = p.communicate(timeout=duration + 20)
        wall = time.time() - t0
        rep.check("stall: never hangs (wall < duration + 20 s)", wall < duration + 20, f"{wall:.1f} s")
        rep.check("stall: ends with exit 0 or 4", p.returncode in (0, 4), f"rc={p.returncode} {stderr[-200:]}")
        res = _last_json_line(stdout) or {}
        if p.returncode == 4:
            rep.check("stall: a lost stream carries a non-empty error", bool(res.get("error")), res.get("error"))
        ing2.stop()
    finally:
        ing.kill()
        if p is not None and p.poll() is None:
            p.kill()
    return rep


def sc_stream_unreachable(out):
    rep = Report("stream_unreachable")
    d = out / "stream_unreachable"
    d.mkdir(parents=True, exist_ok=True)
    from fakes.blackhole import Blackhole
    bh = Blackhole()
    key = "sk-live-blackhole-1234"
    try:
        t0 = time.time()
        rc, res, err = run_cli(["record", "--synthetic", "--stream", bh.url, "--stream-key", key,
                                "--duration", 5, "--out", d], timeout=60)
        took = time.time() - t0
        rep.check("fails in under 10 s", took < 10, f"{took:.1f} s")
        rep.check("exit code 3", rc == 3, f"rc={rc} {err.strip()[-150:]}")
        rep.check('error mentions "stream"', "stream" in err, err.strip()[-150:])
    finally:
        bh.stop()
    return rep


def sc_ui_stream(out):
    rep = Report("ui_stream")
    d = out / "ui_stream"
    d.mkdir(parents=True, exist_ok=True)
    from fakes.rtmp_ingest import RtmpIngest
    if not rep.check("ffmpeg build has the rtmp protocol", capabilities()["rtmp"]):
        return rep
    key = "sk-live-uistream-abcd1234"
    ing = RtmpIngest(d, key=key)
    ing.start()
    subj = Subject(d, title="VV TestSubject Stream", hz=440)
    gui = Gui(d, "vv-e2e-ui-stream")
    dumps = []  # every ui_state dumped, scanned for the key at the end
    try:
        c = gui.c

        def ui_state():
            s = c.call(cmd="ui_state")
            dumps.append(json.dumps(s))
            return s

        def hud_state():
            return ui_state().get("hud") or {}

        def hud_action(text):
            return next((m for m in hud_state().get("menu", []) if m.get("text") == text), None)

        def hud_submenu(text):
            a = hud_action(text)
            return a.get("menu", []) if a else []

        rep.check("gui answers ping over the control pipe", c.call(cmd="ping")["ok"])
        c.call(cmd="ui_override", config={"outDir": str(d / "rec"), "name": "uistream", "mic": "none", "sys": "none",
                                          "fps": 30, "streamUrl": ing.url, "telemetryDir": str(d / "telemetry")})

        # Source and output, both from the status bar's menu.
        src = [m for m in ui_state()["menu"] if m.get("text") == "Source"][0]["menu"]
        item = [m["text"] for m in src if "VV TestSubject Stream" in m.get("text", "")][0]
        rep.check("hud menu: Source > test window",
                  c.call(cmd="ui_trigger", path=["Source", item], surface="hud")["ok"])
        rep.check("hud menu: Output > Stream to YouTube",
                  c.call(cmd="ui_trigger", path=["Output", "Stream to YouTube"], surface="hud")["ok"])
        live = hud_action("Go live")
        rep.check("with no key: Go live is disabled", live is not None and live["enabled"] is False, live)
        yt = hud_submenu("YouTube")
        rep.check("YouTube info line: Stream key: not set",
                  bool(yt) and yt[0]["text"] == "Stream key: not set", yt[0]["text"] if yt else yt)

        # The key comes from YouTube Studio; here it arrives over the pipe (dialogs are not drivable).
        r = c.call(cmd="set_stream_key", key=key)
        rep.check("set_stream_key over the pipe", r.get("ok") is True and r.get("streamKeySet") is True, r)
        live = hud_action("Go live")
        rep.check("with a key: Go live is enabled", live is not None and live["enabled"] is True, live)
        yt = hud_submenu("YouTube")
        rep.check("YouTube info line: Stream key: set",
                  bool(yt) and yt[0]["text"] == "Stream key: set", yt[0]["text"] if yt else yt)

        # Go live from the bar, run for 5 s, end it from the bar.
        rep.check("hud menu: Go live", c.call(cmd="ui_trigger", path=["Go live"], surface="hud")["ok"])
        end = time.time() + 20
        while time.time() < end:
            h = hud_state()
            if h.get("dot") == "live" and h.get("label") == "LIVE":
                break
            time.sleep(0.2)
        h = hud_state()
        rep.check("hud: live dot", h.get("dot") == "live", h.get("dot"))
        rep.check("hud: LIVE label", h.get("label") == "LIVE", h.get("label"))
        rep.check("hud: the rate is on the bar", (h.get("kbps") or "").endswith("M"), h.get("kbps"))
        rep.check("Pause is disabled in stream mode", (hud_action("Pause") or {}).get("enabled") is False)
        rep.check("Output is locked while live", (hud_action("Output") or {}).get("enabled") is False)
        rep.check("the first action is End stream", hud_action("End stream") is not None)
        s = ui_state()
        rep.check("tooltip: LIVE with the rate",
                  s["tooltip"].startswith("Voloire-video | LIVE ") and "Mbps" in s["tooltip"], s["tooltip"])
        rep.check("ingest: connection received", ing.wait_connected(20))
        time.sleep(5)
        rep.check("hud menu: End stream", c.call(cmd="ui_trigger", path=["End stream"], surface="hud")["ok"])
        end = time.time() + 20
        while time.time() < end:
            h = hud_state()
            if h.get("dot") == "idle" and h.get("label") == "IDLE":
                break
            time.sleep(0.2)
        h = hud_state()
        rep.check("hud: back to idle", h.get("dot") == "idle", h.get("dot"))
        u = ui_state()
        rep.check("state returns to idle", u["state"] == "idle", u["state"])

        ing.stop()
        if rep.check("ingest: received file written", ing.received.exists()):
            ok, why = decodes_clean(ing.received)
            rep.check("ingest: decodes clean", ok, why)
        hits = scan_for_secrets([str(d / "telemetry"), str(d / "settings"), *dumps], [key])
        rep.check("no key leak in the dumps, the settings or the telemetry", not hits, hits)
    finally:
        gui.close()
        subj.stop()
        ing.stop()
    return rep


def sc_installers(out):
    import sc_installers
    return sc_installers.run(out)


SCENARIOS = {
    "harness_selftest": sc_harness_selftest,
    "synthetic_basic": sc_synthetic_basic,
    "quality_presets": sc_quality_presets,
    "share_copy": sc_share_copy,
    "stream_rtmp": sc_stream_rtmp,
    "stream_stored_key": sc_stream_stored_key,
    "stream_reconnect": sc_stream_reconnect,
    "stream_stall": sc_stream_stall,
    "stream_unreachable": sc_stream_unreachable,
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
    "hud": sc_hud,
    "ui_stream": sc_ui_stream,
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
