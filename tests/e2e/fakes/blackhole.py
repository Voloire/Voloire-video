"""A TCP server that accepts connections and never reads from them: a stalled peer.

A pusher that connects gets past the handshake only if the app never blocks capture on the
network; whatever it writes fills the socket buffer and then goes nowhere.
"""
import socket
import threading
from fakes.rtmp_ingest import free_port


class Blackhole:
    def __init__(self, port=0):
        self.port = port or free_port()
        self.url = f"rtmp://127.0.0.1:{self.port}/live2"
        self._run = True
        self._sock = socket.socket()
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.bind(("127.0.0.1", self.port))
        self._sock.listen(8)
        self._sock.settimeout(0.2)
        self._conns = []
        self._th = threading.Thread(target=self._accept_loop, daemon=True)
        self._th.start()

    def _accept_loop(self):
        while self._run:
            try:
                c, _ = self._sock.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            self._conns.append(c)  # accepted and deliberately never read

    def stop(self):
        self._run = False
        for c in self._conns:
            try:
                c.close()
            except OSError:
                pass
        self._conns.clear()
        try:
            self._sock.close()
        except OSError:
            pass
        self._th.join(2)
