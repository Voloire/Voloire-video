"""A local stand-in for Twitch (id, Helix, ingest list) and for the public YouTube live page.

The app is pointed at it with the VV_TWITCH_* / VV_YOUTUBE_BASE environment variables (see .env),
so no test ever talks to the real services. State is plain attributes the scenarios can read:
patches (PATCH bodies received), refreshes, requests, channel.
"""
import json
import threading
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class FakeTwitch:
    client_id = "fake-client-id"
    user = {"id": "123456", "login": "voloire", "display_name": "voloire"}
    stream_key = "live_123456_fakekey9876"
    device_code, user_code = "devcode-XYZ", "ABCD-1234"
    categories = [{"id": "509658", "name": "Just Chatting"}, {"id": "33214", "name": "Fortnite"}]
    ingests = [{"name": "Default", "url_template": "rtmp://live.twitch.tv/app/{stream_key}"},
               {"name": "Europe: Milan, Italy", "url_template": "rtmp://mil.contribute.live-video.net/app/{stream_key}"}]

    def __init__(self):
        self._lock = threading.Lock()
        self.access, self.refresh_token = "", ""  # nothing is issued before the device flow is approved
        self.issued_access, self.issued_refresh = [], []
        self._n = 0
        self.approved = False
        self.expired = False
        self.refresh_broken = False
        self.refreshes = 0
        self.requests = []   # (method, path) of every request
        self.patches = []    # PATCH /helix/channels bodies
        self.channel = {"title": "Old title", "game_id": "33214", "game_name": "Fortnite",
                        "broadcaster_language": "en", "tags": []}
        self.stream_id = ""
        self.vods = []       # newest first
        self.youtube = {}    # channel id -> video id
        self._reject = None  # (status, message) for the next PATCH
        fake = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *a):  # silence
                pass

            def _send(self, status, body=None, ctype="application/json"):
                data = b"" if body is None else (body if isinstance(body, bytes) else
                                                 (json.dumps(body) if not isinstance(body, str) else body).encode("utf-8"))
                self.send_response(status)
                if status != 204:
                    self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                if data:
                    self.wfile.write(data)

            def _handle(self, method):
                u = urllib.parse.urlparse(self.path)
                q = {k: v[0] for k, v in urllib.parse.parse_qs(u.query).items()}
                n = int(self.headers.get("Content-Length") or 0)
                raw = self.rfile.read(n) if n else b""
                with fake._lock:
                    fake.requests.append((method, u.path))
                    status, body, ctype = fake.route(method, u.path, q, raw, self.headers)
                self._send(status, body, ctype or "application/json")

            def do_GET(self):
                self._handle("GET")

            def do_POST(self):
                self._handle("POST")

            def do_PATCH(self):
                self._handle("PATCH")

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.port = self.server.server_address[1]
        self.base = f"http://127.0.0.1:{self.port}"
        self._th = threading.Thread(target=self.server.serve_forever, daemon=True)
        self._th.start()

    # -------------------------------------------------------------- environment for the app

    @property
    def env(self):
        return {"VV_TWITCH_API_BASE": self.base + "/helix", "VV_TWITCH_ID_BASE": self.base + "/oauth2",
                "VV_TWITCH_INGEST_BASE": self.base, "VV_YOUTUBE_BASE": self.base + "/yt",
                "VV_LINK_POLL_MS": "500", "VV_LINK_GRACE_MS": "3000", "VV_NO_BROWSER": "1"}

    @property
    def all_tokens(self):
        """Every token ever issued, for the leak scans."""
        with self._lock:
            return list(self.issued_access) + list(self.issued_refresh)

    # -------------------------------------------------------------- scenario controls

    def approve(self):
        with self._lock:
            self.approved = True

    def expire_token(self):
        with self._lock:
            self.expired = True

    def break_refresh(self):
        with self._lock:
            self.refresh_broken = True

    def set_live(self, stream_id):
        with self._lock:
            self.stream_id = stream_id

    def set_offline(self):
        with self._lock:
            self.stream_id = ""

    def add_vod(self, vod_id, stream_id):
        with self._lock:
            self.vods.insert(0, {"id": vod_id, "stream_id": stream_id, "user_id": self.user["id"],
                                 "url": f"https://www.twitch.tv/videos/{vod_id}", "type": "archive",
                                 "created_at": "2026-10-25T10:00:00Z"})

    def reject_patch(self, status=409, message="title too long"):
        with self._lock:
            self._reject = (status, message)

    def set_youtube_video(self, channel_id, video_id):
        with self._lock:
            self.youtube[channel_id] = video_id

    def stop(self):
        self.server.shutdown()
        self.server.server_close()

    # -------------------------------------------------------------- routing (called with the lock held)

    def _issue(self):
        self._n += 1
        self.access, self.refresh_token = f"fake-access-AAAA{self._n}", f"fake-refresh-BBBB{self._n}"
        self.issued_access.append(self.access)
        self.issued_refresh.append(self.refresh_token)
        self.expired = False

    def _authorized(self, headers):
        if self.expired or not self.access:
            return False
        return (headers.get("Authorization") == "Bearer " + self.access and headers.get("Client-Id") == self.client_id)

    def route(self, method, path, q, raw, headers):
        if path == "/oauth2/device" and method == "POST":
            f = {k: v[0] for k, v in urllib.parse.parse_qs(raw.decode()).items()}
            if f.get("client_id") != self.client_id:
                return 400, {"status": 400, "message": "invalid client"}, None
            self.approved = False
            return 200, {"device_code": self.device_code, "user_code": self.user_code,
                         "verification_uri": self.base + "/activate", "expires_in": 1800, "interval": 1}, None
        if path == "/oauth2/token" and method == "POST":
            f = {k: v[0] for k, v in urllib.parse.parse_qs(raw.decode()).items()}
            if f.get("client_id") != self.client_id:
                return 400, {"status": 400, "message": "invalid client"}, None
            if f.get("grant_type") == "refresh_token":
                if self.refresh_broken or f.get("refresh_token") != self.refresh_token:
                    return 400, {"status": 400, "message": "Invalid refresh token"}, None
                self.refreshes += 1
                self._issue()
                return 200, {"access_token": self.access, "refresh_token": self.refresh_token,
                             "expires_in": 14400, "token_type": "bearer"}, None
            if f.get("device_code") != self.device_code:
                return 400, {"status": 400, "message": "invalid device code"}, None
            if not self.approved:
                return 400, {"status": 400, "message": "authorization_pending"}, None
            self.approved = False  # a device code works once
            self.refresh_broken = False
            self._issue()
            return 200, {"access_token": self.access, "refresh_token": self.refresh_token, "expires_in": 14400,
                         "scope": ["channel:manage:broadcast", "channel:read:stream_key"], "token_type": "bearer"}, None
        if path == "/activate":
            return 200, "<html><body>activate</body></html>", "text/html"
        if path == "/ingests":
            return 200, {"ingests": self.ingests}, None
        if path.startswith("/yt/channel/") and path.endswith("/live"):
            cid = path[len("/yt/channel/"):-len("/live")]
            vid = self.youtube.get(cid)
            link = (f'<link rel="canonical" href="https://www.youtube.com/watch?v={vid}">' if vid
                    else '<link rel="canonical" href="https://www.youtube.com/channel/' + cid + '">')
            return 200, f"<html><head>{link}</head><body></body></html>", "text/html"
        if path.startswith("/helix/"):
            if not self._authorized(headers):
                return 401, {"status": 401, "error": "Unauthorized", "message": "Invalid OAuth token"}, None
            sub = path[len("/helix"):]
            if sub == "/users" and method == "GET":
                return 200, {"data": [self.user]}, None
            if sub == "/streams/key" and method == "GET":
                return 200, {"data": [{"stream_key": self.stream_key}]}, None
            if sub == "/channels" and method == "GET":
                c = self.channel
                return 200, {"data": [{"broadcaster_id": self.user["id"], "broadcaster_login": self.user["login"],
                                       "broadcaster_name": self.user["display_name"], "broadcaster_language":
                                       c["broadcaster_language"], "game_id": c["game_id"], "game_name": c["game_name"],
                                       "title": c["title"], "tags": c["tags"]}]}, None
            if sub == "/channels" and method == "PATCH":
                if self._reject:
                    status, message = self._reject
                    self._reject = None
                    return status, {"status": status, "error": "Conflict", "message": message}, None
                body = json.loads(raw.decode() or "{}")
                self.patches.append(body)
                if "title" in body:
                    self.channel["title"] = body["title"]
                if "game_id" in body:
                    self.channel["game_id"] = body["game_id"]
                    self.channel["game_name"] = next((c["name"] for c in self.categories if c["id"] == body["game_id"]), "")
                if "broadcaster_language" in body:
                    self.channel["broadcaster_language"] = body["broadcaster_language"]
                if "tags" in body:
                    self.channel["tags"] = body["tags"]
                return 204, None, None
            if sub == "/search/categories" and method == "GET":
                needle = q.get("query", "").lower()
                return 200, {"data": [dict(c, box_art_url="") for c in self.categories if needle in c["name"].lower()]}, None
            if sub == "/streams" and method == "GET":
                data = [{"id": self.stream_id, "user_id": self.user["id"], "type": "live"}] if self.stream_id else []
                return 200, {"data": data}, None
            if sub == "/videos" and method == "GET":
                return 200, {"data": [v for v in self.vods if v["user_id"] == q.get("user_id", self.user["id"])]}, None
        return 404, {"status": 404, "message": "not found: " + path}, None
