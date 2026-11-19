import concurrent.futures
import contextlib
import http.client
import json
import os
from pathlib import Path
import signal
import socket
import socketserver
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Backend(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_):
        pass

    def do_GET(self):
        if self.path == "/unavailable":
            self.wfile.write(b"not an HTTP response\r\n\r\n")
            self.close_connection = True
            return
        if self.path == "/api/v2/close-delimited":
            self.wfile.write(b"HTTP/1.0 200 OK\r\nConnection: close\r\n\r\nunframed")
            self.close_connection = True
            return
        if self.path == "/api/v2/no-content":
            self.wfile.write(b"HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n")
            self.close_connection = True
            return
        if self.path == "/api/v2/chunked":
            self.wfile.write(
                b"HTTP/1.1 103 Early Hints\r\nLink: </test>\r\n\r\n"
                b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
                b"Trailer: X-Trailer\r\nConnection: close, X-Private\r\n"
                b"X-Private: remove-me\r\n\r\n"
                b"5;test=yes\r\nhello\r\n6\r\n world\r\n0\r\nX-Trailer: discard\r\n\r\n"
            )
            self.close_connection = True
            return
        self.respond(self.server.label.encode())

    def do_POST(self):
        body = self.rfile.read(int(self.headers["Content-Length"]))
        metadata = {
            "path": self.path,
            "host": self.headers["Host"],
            "connection": self.headers["Connection"],
            "via": self.headers["Via"],
            "private": self.headers["X-Private"],
            "test": self.headers["X-Test"],
        }
        self.respond(json.dumps(metadata).encode() + b"\n" + body)

    def respond(self, body):
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
        self.close_connection = True


class SilentBackend(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.recv(4096)
        self.server.release.wait(timeout=12)


@contextlib.contextmanager
def background(server):
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield server
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


def address(server):
    return f"127.0.0.1:{server.server_address[1]}"


def request(port, path, method="GET", body=None, headers=None):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=8)
    try:
        conn.request(method, path, body=body, headers=headers or {})
        response = conn.getresponse()
        return response.status, response.headers, response.read()
    finally:
        conn.close()


def raw(port, *parts):
    with socket.create_connection(("127.0.0.1", port), timeout=8) as client:
        for part in parts:
            client.sendall(part)
        client.shutdown(socket.SHUT_WR)
        with client.makefile("rb") as stream:
            return stream.readline(), stream.read()


def await_start(process, port):
    deadline = time.monotonic() + 8
    while process.poll() is None and time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise AssertionError("Server did not begin listening")


def exercise(port, upstream_authority, root):
    status, headers, body = request(port, "/")
    assert status == 200 and body == b"<h1>hello</h1>"
    assert headers["Content-Type"].startswith("text/html")
    assert headers["Connection"] == "close"
    assert request(port, "/photo.JPG?q=1")[1]["Content-Type"] == "image/jpeg"
    assert request(port, "/hello%20world.txt")[2] == b"with spaces"
    assert request(port, "/nested/")[2] == b"nested"
    assert request(port, "/large.bin")[2] == (root / "large.bin").read_bytes()
    assert request(port, "/empty.txt")[2] == b""
    assert request(port, "/missing")[0] == 404
    assert request(port, "/%2e%2e/secret")[0] == 403
    assert request(port, "/outside.txt")[0] == 403
    assert request(port, "/outside-dir/secret.txt")[0] == 403
    assert request(port, "/fifo")[0] == 403
    assert request(port, "/index.html", "POST", b"a")[0] == 405
    assert request(port, "/echo")[1]["Allow"] == "POST"
    assert request(port, "/apix")[0] == 404

    binary = b"\x00hello\xff" * 16384
    assert request(port, "/echo", "POST", binary)[2] == binary
    assert request(port, "/echo", "POST", bytes(1024 * 1024))[0] == 200
    assert request(port, "/echo", "POST", b"")[2] == b""
    assert raw(port, b"POST /echo HTTP/1.1\r\nHost: local\r\nCont",
               b"ent-Length: 5\r\n\r", b"\n", b"a\x00b", b"cd")[1].endswith(b"a\x00bcd")

    with socket.create_connection(("127.0.0.1", port), timeout=8) as client:
        with client.makefile("rb") as stream:
            client.sendall(b"POST /echo HTTP/1.1\r\nHost: local\r\n"
                           b"Content-Length: 3\r\nExpect: 100-continue\r\n\r\n")
            assert stream.read(25) == b"HTTP/1.1 100 Continue\r\n\r\n"
            client.sendall(b"abc")
            assert stream.readline().startswith(b"HTTP/1.1 200 ")
            assert stream.read().endswith(b"abc")

    cases = [
        (b"GET / HTTP/1.1\r\nHost: local\r\n", 400),
        (b"GET / HTTP/1.1\nHost: local\n\n", 400),
        (b"GET / HTTP/1.1\r\nHost: a\x00b\r\n\r\n", 400),
        (b"POST /echo HTTP/1.1\r\nHost: local\r\nContent-Length: 3\r\n\r\na", 400),
        (b"DELETE / HTTP/1.1\r\nHost: local\r\n\r\n", 405),
        (b"POST /echo HTTP/1.1\r\nHost: local\r\nContent-Length: 1048577\r\n\r\n", 413),
        (b"GET /" + b"x" * 2100 + b" HTTP/1.1\r\nHost: local\r\n\r\n", 414),
        (b"GET / HTTP/1.1\r\nHost: local\r\nX-Long: " + b"a" * 16500 + b"\r\n\r\n", 431),
        (b"GET / HTTP/3.0\r\nHost: local\r\n\r\n", 505),
    ]
    for wire, code in cases:
        line, _ = raw(port, wire)
        assert line.startswith(f"HTTP/1.1 {code} ".encode()), (line, code)

    for expected in (b"A", b"B", b"A", b"B"):
        assert request(port, "/api/items")[2] == expected

    status, headers, body = request(
        port, "/api/v2/echo?q=one%20two", "POST", binary,
        {"Connection": "keep-alive, X-Private", "X-Private": "strip", "X-Test": "pass"}
    )
    metadata, forwarded = body.split(b"\n", 1)
    assert status == 200 and forwarded == binary
    assert json.loads(metadata) == {
        "path": "/api/v2/echo?q=one%20two", "host": upstream_authority,
        "connection": "close", "via": "1.1 jungle-webserver", "private": None,
        "test": "pass",
    }
    status, headers, body = request(port, "/api/v2/chunked")
    assert status == 200 and body == b"hello world"
    assert headers["Transfer-Encoding"] == "chunked" and headers["X-Private"] is None
    line, data = raw(port, b"GET /api/v2/chunked HTTP/1.0\r\n\r\n")
    assert line.startswith(b"HTTP/1.0 200 ") and data.endswith(b"\r\n\r\nhello world")
    assert request(port, "/api/v2/close-delimited")[2] == b"unframed"
    assert request(port, "/api/v2/no-content")[0] == 204

    with socket.create_connection(("127.0.0.1", port), timeout=8) as disconnected:
        disconnected.sendall(b"GET /large.bin HTTP/1.1\r\nHost: local\r\n\r\n")
    with concurrent.futures.ThreadPoolExecutor(max_workers=32) as executor:
        statuses = executor.map(
            lambda index: request(port, "/api/items" if index % 2 else "/large.bin")[0],
            range(96),
        )
        assert all(code == 200 for code in statuses)

    unavailable = request(port, "/unavailable")
    assert unavailable[0] == 502, unavailable
    slow = request(port, "/slow")
    assert slow[0] == 504, slow


def test(binary):
    with contextlib.ExitStack() as cleanup:
        temp = Path(cleanup.enter_context(tempfile.TemporaryDirectory()))
        root = temp / "public"
        root.mkdir()
        (root / "index.html").write_bytes(b"<h1>hello</h1>")
        (root / "photo.JPG").write_bytes(b"\xff\xd8\xff")
        (root / "hello world.txt").write_bytes(b"with spaces")
        (root / "empty.txt").touch()
        (root / "nested").mkdir()
        (root / "nested" / "index.html").write_bytes(b"nested")
        (root / "large.bin").write_bytes(bytes(range(256)) * 4096)
        (temp / "secret.txt").write_text("outside the root")
        (root / "outside.txt").symlink_to(temp / "secret.txt")
        (root / "outside-dir").symlink_to(temp, target_is_directory=True)
        os.mkfifo(root / "fifo")

        backends = []
        for label in "ABV":
            server = ThreadingHTTPServer(("127.0.0.1", 0), Backend)
            server.label = label
            backends.append(cleanup.enter_context(background(server)))
        extra = socketserver.ThreadingTCPServer(("127.0.0.1", 0), SilentBackend)
        extra.daemon_threads = True
        extra.release = threading.Event()
        slow = cleanup.enter_context(background(extra))
        cleanup.callback(slow.release.set)

        with socket.socket() as reserved:
            reserved.bind(("127.0.0.1", 0))
            port = reserved.getsockname()[1]
        args = [
            binary, str(port), str(root),
            f"/api={address(backends[0])},{address(backends[1])}",
            f"/api/v2={address(backends[2])}",
            f"/unavailable={address(backends[2])}",
            f"/slow={address(slow)}",
        ]
        with subprocess.Popen(args, stderr=subprocess.PIPE, text=True) as process:
            try:
                await_start(process, port)
                exercise(port, address(backends[2]), root)
                with socket.create_connection(("127.0.0.1", port), timeout=2) as partial:
                    partial.sendall(b"POST /echo HTTP/1.1\r\nHost: local\r\n"
                                    b"Content-Length: 3\r\nExpect: 100-continue\r\n\r\n")
                    with partial.makefile("rb") as stream:
                        assert stream.readline().startswith(b"HTTP/1.1 100 ")
                        process.send_signal(signal.SIGTERM)
                        process.wait(timeout=2)
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                try:
                    _, stderr = process.communicate(timeout=8)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.communicate()
                    raise
                assert process.returncode == 0, stderr
                assert "Sanitizer" not in stderr and "runtime error:" not in stderr, stderr


if __name__ == "__main__":
    test(str(Path(sys.argv[1]).resolve()))
    print("Integration tests passed")
