"""Self-test of the e2e fakes.

Proves that the local RTMP ingest really receives a pushed stream, that killing it breaks the
push, that the blackhole accepts and stays silent, and that the secret scanner works.
"""
import json
import socket
import subprocess
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

from fakes.blackhole import Blackhole
from fakes.rtmp_ingest import NO_WINDOW, RtmpIngest
from fakes.twitch_api import FakeTwitch
from vvlib import FFMPEG, Report, capabilities, probe, scan_for_secrets, streams


def _push(url, seconds=4, fps=30):
    return subprocess.Popen(
        [FFMPEG, "-hide_banner", "-loglevel", "error", "-nostdin", "-re",
         "-f", "lavfi", "-i", f"testsrc2=s=640x360:r={fps}", "-f", "lavfi", "-i", "sine=440",
         "-t", str(seconds), "-c:v", "h264_mf", "-c:a", "aac", "-f", "flv", url],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, creationflags=NO_WINDOW)


def _http(method, url, data=None, headers=None):
    """(status, parsed json or text) of one request; HTTP errors are answers, not exceptions."""
    req = urllib.request.Request(url, data=data, method=method, headers=headers or {})
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            status, body = r.status, r.read().decode("utf-8")
    except urllib.error.HTTPError as e:
        status, body = e.code, e.read().decode("utf-8")
    try:
        return status, json.loads(body)
    except ValueError:
        return status, body


def _twitch_selftest(rep):
    fake = FakeTwitch()
    try:
        form = {"Content-Type": "application/x-www-form-urlencoded"}
        enc = lambda d: urllib.parse.urlencode(d).encode()
        st, dev = _http("POST", fake.base + "/oauth2/device", enc({"client_id": fake.client_id, "scopes": "x"}), form)
        rep.check("twitch fake: device code", st == 200 and dev["user_code"] == fake.user_code, dev)
        tokreq = enc({"client_id": fake.client_id, "device_code": fake.device_code,
                      "grant_type": "urn:ietf:params:oauth:grant-type:device_code"})
        st, tok = _http("POST", fake.base + "/oauth2/token", tokreq, form)
        rep.check("twitch fake: pending until approved", st == 400 and tok["message"] == "authorization_pending", tok)
        fake.approve()
        st, tok = _http("POST", fake.base + "/oauth2/token", tokreq, form)
        rep.check("twitch fake: tokens after approval", st == 200 and tok["access_token"] and tok["refresh_token"], tok)
        auth = {"Authorization": "Bearer " + tok["access_token"], "Client-Id": fake.client_id}
        st, me = _http("GET", fake.base + "/helix/users", headers=auth)
        rep.check("twitch fake: users with the bearer", st == 200 and me["data"][0]["login"] == "voloire", me)
        st, _ = _http("GET", fake.base + "/helix/users", headers={"Client-Id": fake.client_id})
        rep.check("twitch fake: users without the bearer is 401", st == 401)
        fake.expire_token()
        st, _ = _http("GET", fake.base + "/helix/users", headers=auth)
        rep.check("twitch fake: expired token is 401", st == 401)
        rtreq = enc({"client_id": fake.client_id, "grant_type": "refresh_token", "refresh_token": tok["refresh_token"]})
        st, tok2 = _http("POST", fake.base + "/oauth2/token", rtreq, form)
        rep.check("twitch fake: refresh rotates both tokens",
                  st == 200 and tok2["access_token"] != tok["access_token"] and fake.refreshes == 1, tok2)
        auth2 = {"Authorization": "Bearer " + tok2["access_token"], "Client-Id": fake.client_id}
        st, key = _http("GET", fake.base + "/helix/streams/key?broadcaster_id=123456", headers=auth2)
        rep.check("twitch fake: stream key", st == 200 and key["data"][0]["stream_key"] == fake.stream_key, key)
        st, _ = _http("PATCH", fake.base + "/helix/channels?broadcaster_id=123456",
                      json.dumps({"title": "T", "tags": ["a"]}).encode(), {**auth2, "Content-Type": "application/json"})
        rep.check("twitch fake: PATCH is recorded and answers 204",
                  st == 204 and fake.patches[-1]["title"] == "T" and fake.channel["title"] == "T", fake.patches)
        fake.add_vod("41234567", "999")
        st, v = _http("GET", fake.base + "/helix/videos?user_id=123456&type=archive", headers=auth2)
        rep.check("twitch fake: videos", st == 200 and v["data"][0]["url"] == "https://www.twitch.tv/videos/41234567", v)
        fake.set_youtube_video("UCfake", "dQw4w9WgXcQ")
        st, page = _http("GET", fake.base + "/yt/channel/UCfake/live")
        rep.check("twitch fake: youtube live page carries the canonical link",
                  st == 200 and "watch?v=dQw4w9WgXcQ" in page, page)
    finally:
        fake.stop()


def run(out):
    rep = Report("harness_selftest")
    d = out / "harness_selftest"
    d.mkdir(parents=True, exist_ok=True)
    rep.data["capabilities"] = capabilities()

    # 1. the ingest receives a real push
    ing = RtmpIngest(d, key="selkey-1234")
    ing.start()
    push = _push(f"{ing.url}/{ing.key}")
    rep.check("ingest: reports the connection", ing.wait_connected(15))
    push.wait(30)
    ing.stop()
    info = probe(ing.received)
    if rep.check("ingest: received file probes", info is not None):
        v, a = streams(info, "video"), streams(info, "audio")
        rep.check("ingest: one h264 video and one aac audio stream",
                  len(v) == 1 and v[0]["codec_name"] == "h264" and len(a) == 1 and a[0]["codec_name"] == "aac",
                  [(s["codec_type"], s["codec_name"]) for s in v + a])
        dur = float(info["format"]["duration"])
        rep.check("ingest: duration 3.5..4.5 s", 3.5 <= dur <= 4.5, f"{dur:.2f}")

    # 2. killing the ingest mid-push breaks the pusher
    ing2 = RtmpIngest(out / "harness_selftest" / "kill", key="selkey-1234")
    ing2.start()
    push2 = _push(f"{ing2.url}/{ing2.key}", seconds=30)
    try:
        rep.check("ingest: second connection accepted", ing2.wait_connected(15))
        ing2.kill()
        try:
            rc = push2.wait(10)
            rep.check("ingest: kill makes the pusher exit non-zero within 10 s", rc != 0, f"rc={rc}")
        except subprocess.TimeoutExpired:
            push2.kill()
            rep.check("ingest: kill makes the pusher exit non-zero within 10 s", False, "still running after 10 s")
    finally:
        ing2.kill()
        if push2.poll() is None:
            push2.kill()

    # 3. the blackhole accepts and never sends anything
    bh = Blackhole()
    try:
        s = socket.create_connection(("127.0.0.1", bh.port), timeout=5)
        rep.check("blackhole: accepts a connection", True)
        s.settimeout(1.0)
        try:
            data = s.recv(64)
            rep.check("blackhole: 1 s read returns no data", data == b"", f"{len(data)} bytes")
        except socket.timeout:
            rep.check("blackhole: 1 s read returns no data", True)
        s.close()
    finally:
        bh.stop()

    # 4. the secret scanner
    secret = "sk-live-abcd1234"
    planted = d / "leak.txt"
    planted.write_text("prefix " + secret + " suffix", encoding="utf-8")
    clean = d / "clean.txt"
    clean.write_text("nothing to see here", encoding="utf-8")
    hits = scan_for_secrets([planted, clean, "hash " + secret, "hash clean"], [secret])
    rep.check("scan: finds the planted file", any(str(planted) in h for h in hits), hits)
    rep.check("scan: finds the planted string", any("text" in h for h in hits), hits)
    rep.check("scan: reports the 4-char prefix only", all("sk-live-abcd1234" not in h and "sk-l" in h for h in hits), hits)
    rep.check("scan: clean file and text give no hit", len(hits) == 2, hits)
    _twitch_selftest(rep)
    return rep
