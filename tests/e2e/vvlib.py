"""Shared helpers of the Voloire-video end-to-end harness.

Everything here observes the product from the outside: it runs the binaries, talks to the control
pipe, and judges the produced media with ffprobe/ffmpeg plus numpy. Nothing imports the app.
"""
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[2]
BIN = Path(os.environ.get("VV_BIN", REPO / "build" / "bin"))
CLI = BIN / "vvcli.exe"
GUI = BIN / "voloire-video.exe"
SUBJECT = BIN / "testsubject.exe"


def _find_tool(name):
    p = shutil.which(name)
    if p:
        return p
    deps = Path(os.environ.get("VV_DEPS", Path.home() / "deps")) / "ffmpeg"
    for d in deps.glob("*/bin"):
        if (d / f"{name}.exe").exists():
            return str(d / f"{name}.exe")
    raise SystemExit(f"{name} not found on PATH or in {deps}")


FFMPEG = _find_tool("ffmpeg")
FFPROBE = _find_tool("ffprobe")
RATE = 48000


# ------------------------------------------------------------------ reporting

class Report:
    def __init__(self, name):
        self.name = name
        self.checks = []
        self.data = {}

    def check(self, label, ok, detail=""):
        self.checks.append({"check": label, "ok": bool(ok), "detail": str(detail)})
        return bool(ok)

    @property
    def failed(self):
        return [c for c in self.checks if not c["ok"]]

    def print(self):
        print(f"\n== {self.name}")
        for c in self.checks:
            mark = "PASS" if c["ok"] else "FAIL"
            print(f"  [{mark}] {c['check']}" + (f"  ({c['detail']})" if c["detail"] else ""))


# ------------------------------------------------------------------ processes

def run_cli(args, timeout=120):
    """Runs vvcli and returns (returncode, parsed result json or None, stderr)."""
    p = subprocess.run([str(CLI), *map(str, args)], capture_output=True, text=True, timeout=timeout, encoding="utf-8")
    result = None
    for line in reversed(p.stdout.strip().splitlines()):
        try:
            result = json.loads(line)
            break
        except ValueError:
            continue
    return p.returncode, result, p.stderr


def popen_cli(args, **kw):
    return subprocess.Popen([str(CLI), *map(str, args)], **kw)


class Pipe:
    """Client of the line-delimited JSON control pipe."""

    def __init__(self, name, wait=15):
        self.path = "\\\\.\\pipe\\" + name
        self.f = None
        end = time.time() + wait
        while time.time() < end:
            try:
                self.f = open(self.path, "r+b", buffering=0)
                return
            except OSError:
                time.sleep(0.1)
        raise TimeoutError(f"pipe {name} did not appear")

    def call(self, **req):
        self.f.write((json.dumps(req) + "\n").encode())
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = self.f.read(65536)
            if not chunk:
                raise EOFError("pipe closed")
            buf += chunk
        return json.loads(buf)

    def close(self):
        try:
            self.f.close()
        except Exception:
            pass


_SUBJECTS = []


def kill_subjects():
    for s in _SUBJECTS:
        s.stop()
    _SUBJECTS.clear()


class Subject:
    """A TestSubject process: window plus its own audio, or the noise role."""

    def __init__(self, out, title="VV TestSubject", hz=440, role="tone", borderless=False, exit_after=0):
        Path(out).mkdir(parents=True, exist_ok=True)
        self.ready = Path(out) / f"subject_{role}_{hz}.json"
        if self.ready.exists():
            self.ready.unlink()
        cmd = [str(SUBJECT), "--role", role, "--hz", str(hz), "--title", title, "--ready-file", str(self.ready)]
        if borderless:
            cmd.append("--borderless")
        if exit_after:
            cmd += ["--exit-after", str(exit_after)]
        self.proc = subprocess.Popen(cmd)
        _SUBJECTS.append(self)
        end = time.time() + 10
        while not self.ready.exists() and time.time() < end:
            time.sleep(0.05)
        info = json.loads(self.ready.read_text())
        self.pid, self.hwnd = info["pid"], info["hwnd"]
        time.sleep(1.0)  # let the window paint and the audio stream start

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                self.proc.kill()


# ------------------------------------------------------------------ media

def probe(path):
    p = subprocess.run([FFPROBE, "-v", "error", "-show_streams", "-show_format", "-of", "json", str(path)],
                       capture_output=True, text=True, encoding="utf-8")
    return json.loads(p.stdout) if p.returncode == 0 and p.stdout.strip() else None


def streams(info, kind):
    return [s for s in info["streams"] if s["codec_type"] == kind]


def fps_of(stream):
    n, d = stream["r_frame_rate"].split("/")
    return float(n) / float(d)


def decodes_clean(path):
    """True when ffmpeg can decode every stream to the end without a single error.

    -copyts keeps the AAC priming offset (-21 ms) from re-rounding 24 fps frame times, a harmless artefact.
    """
    p = subprocess.run([FFMPEG, "-v", "error", "-copyts", "-i", str(path), "-fps_mode", "passthrough", "-f", "null", "-"], capture_output=True, text=True)
    return p.returncode == 0 and not p.stderr.strip(), p.stderr.strip()[:200]


def track_index(path, title):
    """Index among the audio streams of the track titled MIX, SYS or MIC (None when absent)."""
    titles = [s.get("tags", {}).get("title") for s in streams(probe(path), "audio")]
    return titles.index(title) if title in titles else None


def decode_audio(path, index):
    """Mono float32 samples at 48 kHz of the index-th audio stream."""
    p = subprocess.run([FFMPEG, "-v", "error", "-i", str(path), "-map", f"0:a:{index}", "-ac", "1", "-ar", str(RATE),
                        "-f", "f32le", "-"], capture_output=True)
    return np.frombuffer(p.stdout, dtype=np.float32)


def decode_luma(path, w=64, h=36):
    """Per-frame luma of the video scaled to w x h, shape (n, h, w), plus the frame rate."""
    info = probe(path)
    v = streams(info, "video")[0]
    p = subprocess.run([FFMPEG, "-v", "error", "-i", str(path), "-map", "0:v:0", "-vf", f"scale={w}:{h},format=gray",
                        "-f", "rawvideo", "-"], capture_output=True)
    a = np.frombuffer(p.stdout, dtype=np.uint8)
    n = len(a) // (w * h)
    return a[: n * w * h].reshape(n, h, w), fps_of(v), float(v.get("start_time", 0) or 0)


def decode_counters(path):
    """The 24-bit frame counter drawn by the synthetic source / TestSubject, one per frame."""
    p = subprocess.run([FFMPEG, "-v", "error", "-i", str(path), "-map", "0:v:0", "-vf",
                        "scale=1280:720,crop=480:24:0:0,format=gray", "-f", "rawvideo", "-"], capture_output=True)
    a = np.frombuffer(p.stdout, dtype=np.uint8)
    n = len(a) // (480 * 24)
    frames = a[: n * 480 * 24].reshape(n, 24, 480)
    cells = frames[:, 12, 10::20][:, :24] > 128
    weights = 1 << np.arange(23, -1, -1)
    return (cells * weights).sum(axis=1)


def flash_times(luma, fps, start=0.0):
    """Start time (s) of every white flash, from the mean luma of each frame."""
    m = luma.reshape(len(luma), -1).mean(axis=1)
    lo, hi = np.percentile(m, 5), np.percentile(m, 95)
    if hi - lo < 40:
        return np.array([]), m
    on = m > (lo + hi) / 2
    edges = np.flatnonzero(on[1:] & ~on[:-1]) + 1
    if on[0]:
        edges = np.insert(edges, 0, 0)
    return start + edges / fps, m


def beep_onsets(x, win_ms=5):
    """Onset times (s) of beeps in a mono signal, with at least 0.5 s between them."""
    w = RATE * win_ms // 1000
    n = len(x) // w
    env = np.sqrt((x[: n * w].reshape(n, w) ** 2).mean(axis=1)) if n else np.array([])
    if len(env) == 0 or env.max() < 0.02:
        return np.array([]), env
    on = env > 0.3 * env.max()
    idx = np.flatnonzero(on[1:] & ~on[:-1]) + 1
    out, last = [], -10.0
    for i in idx:
        t = i * win_ms / 1000.0
        if t - last >= 0.5:
            out.append(t)
            last = t
    return np.array(out), env


def band_power(x, f0, half=15):
    """Power of x inside f0 +/- half Hz (Hann windowed FFT)."""
    if len(x) < 4096:
        return 0.0
    n = len(x)
    spec = np.abs(np.fft.rfft(x * np.hanning(n))) ** 2
    freqs = np.fft.rfftfreq(n, 1.0 / RATE)
    return float(spec[(freqs > f0 - half) & (freqs < f0 + half)].sum() / n)


def dominant_freq(x):
    n = len(x)
    if n < 4096:
        return 0.0
    spec = np.abs(np.fft.rfft(x * np.hanning(n)))
    return float(np.fft.rfftfreq(n, 1.0 / RATE)[spec.argmax()])


def av_offset(video_flashes, audio_onsets):
    """Median (audio - video) offset in ms over flash/beep pairs closer than 400 ms."""
    d = []
    for t in audio_onsets:
        if len(video_flashes) == 0:
            break
        j = np.abs(video_flashes - t).argmin()
        if abs(video_flashes[j] - t) < 0.4:
            d.append((t - video_flashes[j]) * 1000)
    return (float(np.median(d)), len(d)) if d else (None, 0)


def read_jsonl(path):
    out = []
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        try:
            out.append(json.loads(line))
        except ValueError:
            pass
    return out


def png_rgb(path):
    """Decodes a PNG to an (h, w, 3) uint8 array through ffmpeg."""
    info = probe(path)
    s = streams(info, "video")[0]
    w, h = s["width"], s["height"]
    p = subprocess.run([FFMPEG, "-v", "error", "-i", str(path), "-f", "rawvideo", "-pix_fmt", "rgb24", "-"], capture_output=True)
    return np.frombuffer(p.stdout, dtype=np.uint8).reshape(h, w, 3)
