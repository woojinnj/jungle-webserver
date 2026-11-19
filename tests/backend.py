"""Tiny test backend: python3 backend.py PORT NAME

GET /slow       sleeps 30s (proxy timeout test)
GET /chunked    chunked reply (proxy must close the client connection after it)
GET /noclen     close-delimited reply without Content-Length
anything else   "backend NAME path=... xff=... host=... len=..." (POST echoes body size)
"""
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT, NAME = int(sys.argv[1]), sys.argv[2]


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def _reply(self, body=b"", ctype="text/plain"):
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _handle(self):
        n = int(self.headers.get("Content-Length") or 0)
        data = self.rfile.read(n) if n else b""
        if self.path == "/slow":
            time.sleep(30)
        if self.path == "/chunked":
            self.send_response(200)
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for part in (b"hello ", b"chunked"):
                self.wfile.write(b"%x\r\n%s\r\n" % (len(part), part))
            self.wfile.write(b"0\r\n\r\n")
            return
        if self.path == "/noclen":
            self.send_response(200)
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(b"until-eof")
            self.close_connection = True
            return
        body = "backend %s path=%s xff=%s host=%s len=%d\n" % (
            NAME, self.path, self.headers.get("X-Forwarded-For"), self.headers.get("Host"), len(data))
        self._reply(body.encode())

    do_GET = do_POST = do_PUT = do_DELETE = do_HEAD = _handle


ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
