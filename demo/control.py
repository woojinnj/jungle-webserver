"""Demo controller: starts/stops the proxy and the two backends so the demo page can break
things on purpose, and sends malformed requests a browser cannot send.

Run from anywhere:  bash demo/run.sh      (listens on 127.0.0.1:9000 only)
"""
import atexit
import json
import os
import signal
import socket
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PORTS = {"A": 8001, "B": 8002, "proxy": 8080}
CMDS = {
    "A": [sys.executable, os.path.join(HERE, "backend.py"), "8001", "A"],
    "B": [sys.executable, os.path.join(HERE, "backend.py"), "8002", "B"],
    "proxy": [os.path.join(ROOT, "build", "webserver"), "-p", "8080", "-r", os.path.join(HERE, "www"),
              "-t", "2", "-R", "/api=127.0.0.1:8001,127.0.0.1:8002", "-R", "/slow=127.0.0.1:8001"],
}
RAW = {
    "garbage": b"GARBAGE\r\n\r\n",
    "nohost": b"GET / HTTP/1.1\r\n\r\n",
    "traversal": b"GET /../etc/passwd HTTP/1.1\r\nHost: x\r\n\r\n",
    "longuri": b"GET /" + b"a" * 3000 + b" HTTP/1.1\r\nHost: x\r\n\r\n",
    "chunked": b"POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n",
    "badver": b"GET / HTTP/9.9\r\nHost: x\r\n\r\n",
    "delete": b"DELETE /index.html HTTP/1.1\r\nHost: x\r\n\r\n",
}
# only the demo page itself may call this controller (it can start/stop processes)
ALLOWED_ORIGINS = {"http://localhost:8080", "http://127.0.0.1:8080"}

procs = {}
lock = threading.Lock()


def alive(name):
    p = procs.get(name)
    return p is not None and p.poll() is None


def port_open(port):
    try:
        socket.create_connection(("127.0.0.1", port), timeout=0.3).close()
        return True
    except OSError:
        return False


def start(name):
    with lock:
        if alive(name):
            return
        err = None if name == "proxy" else subprocess.DEVNULL  # proxy keeps its access log
        procs[name] = subprocess.Popen(CMDS[name], stdout=subprocess.DEVNULL, stderr=err)
    for _ in range(50):
        if port_open(PORTS[name]):
            return
        time.sleep(0.1)


def stop(name):
    with lock:
        p = procs.get(name)
        if p is None or p.poll() is not None:
            return
        p.terminate()
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()


def status():
    return {n: alive(n) for n in CMDS}


def raw(case):
    try:
        s = socket.create_connection(("127.0.0.1", PORTS["proxy"]), timeout=3)
    except OSError:
        return "connection refused"
    try:
        s.sendall(RAW[case])
        buf = b""
        while b"\r\n" not in buf:
            chunk = s.recv(4096)
            if not chunk:
                break
            buf += chunk
        return buf.split(b"\r\n")[0].decode("latin1")
    except OSError as e:
        return "error: %s" % e
    finally:
        s.close()


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def reply(self, obj, code=200):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        origin = self.headers.get("Origin")
        if origin in ALLOWED_ORIGINS:
            self.send_header("Access-Control-Allow-Origin", origin)
        self.end_headers()
        try:
            self.wfile.write(body)
        except OSError:
            pass

    def do_GET(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)
        name = (q.get("name") or [""])[0]
        if u.path == "/status":
            return self.reply(status())
        if u.path in ("/start", "/stop") and name in CMDS:
            (start if u.path == "/start" else stop)(name)
            return self.reply(status())
        if u.path == "/raw" and (q.get("case") or [""])[0] in RAW:
            return self.reply({"line": raw(q["case"][0])})
        self.reply({"error": "unknown request"}, 404)


def stop_all():
    for n in list(procs):
        stop(n)


if __name__ == "__main__":
    if not os.path.exists(CMDS["proxy"][0]):
        sys.exit("build/webserver not found: run `make` first")
    atexit.register(stop_all)
    signal.signal(signal.SIGTERM, lambda *a: sys.exit(0))
    for n in ("A", "B", "proxy"):
        start(n)
    print("demo ready: open http://localhost:8080/   (Ctrl+C to stop everything)", flush=True)
    try:
        ThreadingHTTPServer(("127.0.0.1", 9000), H).serve_forever()
    except KeyboardInterrupt:
        pass
