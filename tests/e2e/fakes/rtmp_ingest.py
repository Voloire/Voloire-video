"""Local RTMP ingest: a plain `ffmpeg -listen 1` listener that remuxes what it receives.

start() begins listening; a pusher connects at <url>/<key>. The received stream lands remuxed
with -c copy in <out_dir>/ingest_<port>.mkv. freeze()/thaw() suspend the listener process so the
TCP connection stays open but nothing is read, which is exactly a stalled network.
"""
import ctypes
import socket
import subprocess
import time
from pathlib import Path

from fakes import NO_WINDOW
from vvlib import FFMPEG


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class RtmpIngest:
    def __init__(self, out_dir, port=0, app="live2", key="testkey"):
        self.out_dir = Path(out_dir)
        self.out_dir.mkdir(parents=True, exist_ok=True)
        self.port = port or free_port()
        self.app = app
        self.key = key
        self.url = f"rtmp://127.0.0.1:{self.port}/{app}"
        self.received = self.out_dir / f"ingest_{self.port}.mkv"
        self.log = self.out_dir / f"ingest_{self.port}.log"
        self.proc = None

    def start(self):
        if self.received.exists():
            self.received.unlink()
        cmd = [FFMPEG, "-hide_banner", "-loglevel", "warning", "-listen", "1", "-timeout", "30",
               "-f", "flv", "-i", f"{self.url}/{self.key}",
               "-c", "copy", "-f", "matroska", str(self.received)]
        with open(self.log, "wb") as log:
            self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=log, stderr=log,
                                         creationflags=NO_WINDOW)
        return self

    def wait_connected(self, timeout=10.0):
        """True once the listener has written the received stream's header."""
        end = time.time() + timeout
        while time.time() < end:
            if self.received.exists() and self.received.stat().st_size > 0:
                return True
            if self.proc is None or self.proc.poll() is not None:
                return False
            time.sleep(0.05)
        return False

    def stop(self, timeout=10.0):
        """Graceful: `q` on stdin lets ffmpeg finalise the received file; kill() as fallback."""
        if self.proc is None or self.proc.poll() is not None:
            return
        try:
            self.proc.stdin.write(b"q\n")
            self.proc.stdin.flush()
        except OSError:
            pass
        try:
            self.proc.wait(timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(5)

    def kill(self):
        if self.proc is not None and self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait(5)

    def freeze(self):
        """Suspend the listener: it stops reading, the connection stays established."""
        self._ntcall("NtSuspendProcess")

    def thaw(self):
        self._ntcall("NtResumeProcess")

    def _ntcall(self, name):
        PROCESS_SUSPEND_RESUME = 0x0800
        h = ctypes.windll.kernel32.OpenProcess(PROCESS_SUSPEND_RESUME, False, self.proc.pid)
        if not h:
            raise OSError(f"OpenProcess({self.proc.pid}) failed")
        try:
            r = getattr(ctypes.windll.ntdll, name)(h)
            if r != 0:
                raise OSError(f"{name} -> {r:#x}")
        finally:
            ctypes.windll.kernel32.CloseHandle(h)
