"""Headless functional tests. Standard library only; no media, devices, UI or browser."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, str(Path(__file__).parent / "e2e"))
from fakes.twitch_api import FakeTwitch

DRIVER = Path(__file__).resolve().parents[1] / "build/bin/vv_functional_driver.exe"


class FunctionalTests(unittest.TestCase):
    def setUp(self):
        self.fake = FakeTwitch()
        self.addCleanup(self.fake.stop)
        self.tmp = tempfile.TemporaryDirectory(prefix="vv-functional-")
        self.addCleanup(self.tmp.cleanup)
        self.settings = Path(self.tmp.name)
        self.dumps = []
        env = {**os.environ, **self.fake.env, "VV_SETTINGS_DIR": self.tmp.name}
        self.proc = subprocess.Popen([str(DRIVER)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, text=True, encoding="utf-8", env=env,
                                     creationflags=subprocess.CREATE_NO_WINDOW)
        self.addCleanup(self.close_driver)

    def close_driver(self):
        self.proc.stdin.close()
        try:
            self.proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
            self.fail("functional driver did not stop")
        finally:
            self.proc.stdout.close()
            self.proc.stderr.close()
        self.assertEqual(self.proc.returncode, 0)

    def call(self, cmd, **kw):
        self.proc.stdin.write(json.dumps({"cmd": cmd, **kw}) + "\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        self.assertTrue(line, "functional driver exited before replying")
        self.dumps.append(line)
        return json.loads(line)

    def wait_for(self, cmd, predicate, timeout=8):
        deadline = time.monotonic() + timeout
        while True:
            result = self.call(cmd)
            if predicate(result):
                return result
            self.assertLess(time.monotonic(), deadline, "functional state transition timed out")
            time.sleep(0.05)

    def sign_in(self):
        result = self.call("twitch_auth_start", clientId=self.fake.client_id)
        self.assertTrue(result["ok"])
        self.assertEqual(result["userCode"], self.fake.user_code)
        self.assertEqual(result["verificationUri"], self.fake.base + "/activate")
        self.fake.approve()
        result = self.wait_for("twitch_status", lambda r: r["auth"]["state"] == "ok")
        self.assertTrue(result["signedIn"])
        self.assertEqual(result["login"], "voloire")

    def assert_no_secrets(self):
        data = "".join(self.dumps).encode()
        for path in self.settings.rglob("*"):
            if path.is_file():
                data += path.read_bytes()
        for secret in [*self.fake.all_tokens, self.fake.stream_key, self.fake.device_code]:
            self.assertNotIn(secret.encode(), data, "secret appeared outside encrypted storage")

    def begin_twitch_links(self):
        return self.call("test_links_begin", channel={"id": "tw_123456", "platform": "twitch",
                         "twitchLogin": "voloire", "twitchUserId": "123456"})

    def test_auth_key_refresh_and_signout(self):
        self.assertFalse(self.call("twitch_status")["signedIn"])
        self.sign_in()
        prefs = self.call("test_prefs")
        channel = next(c for c in prefs["channels"] if c["id"] == "tw_123456")
        self.assertEqual(channel["platform"], "twitch")
        self.assertEqual(prefs["twitchClientId"], self.fake.client_id)
        self.assertTrue(self.call("twitch_fetch_key", channel=channel["id"])["streamKeySet"])
        self.assertTrue(self.call("get_config", channel=channel["id"])["config"]["streamKeySet"])
        self.assertTrue((self.settings / "secrets/stream_key_tw_123456.bin").exists())
        self.fake.expire_token()
        self.assertTrue(self.call("twitch_get_info")["ok"])
        self.assertEqual(self.fake.refreshes, 1)
        self.fake.break_refresh()
        self.fake.expire_token()
        result = self.call("twitch_get_info")
        self.assertFalse(result["ok"])
        self.assertIn("sign in", result["error"])
        self.assertFalse(self.call("twitch_status")["signedIn"])
        self.sign_in()
        self.assertTrue(self.call("twitch_sign_out")["ok"])
        before = len(self.fake.requests)
        self.assertFalse(self.call("twitch_get_info")["ok"])
        self.assertEqual(len(self.fake.requests), before)
        self.assertFalse((self.settings / "secrets/twitch_access_token.bin").exists())
        for cmd in ["status", "get_config", "logs"]:
            self.call(cmd)
        self.assert_no_secrets()

    def test_broadcast_validation_and_api_rejection(self):
        self.sign_in()
        info = dict(title="Late session", categoryId="509658", categoryName="Just Chatting",
                    language="it", tags=["gaming", "chill"])
        self.assertTrue(self.call("twitch_set_info", **info)["ok"])
        self.assertEqual(self.fake.patches[-1], dict(title="Late session", game_id="509658",
                         broadcaster_language="it", tags=["gaming", "chill"]))
        self.assertEqual(self.call("twitch_get_info")["title"], "Late session")
        for invalid in [dict(title=""), dict(title="x" * 141), dict(tags=["a"] * 11),
                        dict(tags=["bad tag"]), dict(language="invalid")]:
            before = len(self.fake.patches)
            self.assertFalse(self.call("twitch_set_info", **{**info, **invalid})["ok"])
            self.assertEqual(len(self.fake.patches), before)
        self.fake.reject_patch(409, "broadcast refused")
        rejected = self.call("twitch_set_info", **{**info, "title": "Rejected"})
        self.assertFalse(rejected["ok"])
        self.assertIn("broadcast refused", rejected["error"])
        channel = next(c for c in self.call("test_prefs")["channels"] if c["id"] == "tw_123456")
        self.assertEqual(channel["title"], "Late session")
        self.assertEqual(self.call("twitch_search_categories", query="just")["categories"],
                         [{"id": "509658", "name": "Just Chatting"}])
        self.assertEqual(len(self.call("twitch_ingests")["ingests"]), 2)
        self.assert_no_secrets()

    def test_signout_cancels_pending_auth(self):
        self.assertTrue(self.call("twitch_auth_start", clientId=self.fake.client_id)["ok"])
        self.assertTrue(self.call("twitch_sign_out")["ok"])
        self.fake.approve()
        # Past the fake's polling interval: a pending worker must not sign back in.
        time.sleep(1.3)
        status = self.call("twitch_status")
        self.assertFalse(status["signedIn"])
        self.assertEqual(status["auth"]["state"], "idle")

    def test_cancel_and_shutdown_during_auth(self):
        self.assertTrue(self.call("twitch_auth_start", clientId=self.fake.client_id)["ok"])
        self.assertTrue(self.call("twitch_auth_cancel")["ok"])
        self.assertEqual(self.call("twitch_status")["auth"]["state"], "idle")
        self.assertTrue(self.call("twitch_auth_start", clientId=self.fake.client_id)["ok"])
        # Cleanup must join the pending worker without any browser interaction.

    def test_twitch_links_match_current_broadcast(self):
        self.sign_in()
        self.fake.set_live("999")
        self.fake.add_vod("old", "998", age_s=3600)
        result = self.begin_twitch_links()
        self.assertEqual(result["liveUrl"], "https://www.twitch.tv/voloire")
        self.assertEqual(result["state"], "watching")
        time.sleep(0.7)
        self.assertEqual(self.call("test_links_status")["vodUrl"], "")
        self.fake.add_vod("41234567", "999")
        result = self.wait_for("test_links_status", lambda r: r["state"] == "found")
        self.assertEqual(result["vodUrl"], "https://www.twitch.tv/videos/41234567")
        ended = self.call("test_links_end")
        self.assertEqual(ended["state"], "ended")
        self.assertEqual(ended["vodUrl"], result["vodUrl"])
        self.assert_no_secrets()

    def test_twitch_ended_without_vod_keeps_resolving(self):
        self.sign_in()
        self.fake.set_live("999")
        self.begin_twitch_links()
        self.assertEqual(self.call("test_links_end")["state"], "ended")
        self.fake.add_vod("late", "999")
        result = self.wait_for("test_links_status", lambda r: bool(r["vodUrl"]))
        self.assertEqual(result["state"], "ended")
        self.assertEqual(result["vodUrl"], "https://www.twitch.tv/videos/late")

    def test_youtube_links_and_profile_replacement(self):
        channel = "UCDSLIDCQKYVHFLqKEb9CMqw"
        result = self.call("test_links_begin", channel={"studioUrl": channel})
        self.assertEqual(result["liveUrl"], self.fake.base + "/yt/channel/" + channel + "/live")
        self.fake.set_youtube_video(channel, "dQw4w9WgXcQ")
        result = self.wait_for("test_links_status", lambda r: r["state"] == "found")
        self.assertEqual(result["vodUrl"], "https://youtu.be/dQw4w9WgXcQ")
        self.assertEqual(self.call("test_links_end")["state"], "ended")
        result = self.call("test_links_begin", channel={"studioUrl": "https://studio.youtube.com/"})
        self.assertEqual(result["state"], "ended")
        self.assertIn("channel ID", result["error"])
        self.assertEqual(result["vodUrl"], "")

    def test_twitch_vod_grace_period_stops_polling(self):
        self.sign_in()
        self.fake.set_live("999")
        self.begin_twitch_links()
        self.assertEqual(self.call("test_links_end")["state"], "ended")
        time.sleep(3.8)  # the fake environment sets the grace period to three seconds
        before = len(self.fake.requests)
        time.sleep(0.7)
        self.assertEqual(len(self.fake.requests), before)
        self.assertEqual(self.call("test_links_status")["vodUrl"], "")

    def test_driver_disables_media_commands(self):
        for cmd in ["start", "share", "list_devices", "list_windows", "list_monitors"]:
            self.assertFalse(self.call(cmd)["ok"])
        self.assertEqual(self.call("status")["state"], "idle")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--driver", type=Path, default=DRIVER)
    args, rest = parser.parse_known_args()
    DRIVER = args.driver.resolve()
    unittest.main(argv=[sys.argv[0], *rest], verbosity=2)
