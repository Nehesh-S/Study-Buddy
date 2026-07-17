#!/usr/bin/env python3
"""Tiny stand-in for the Study-Buddy backend, for testing the ESP8266 firmware
without running the real YOLO/FastAPI stack.

It answers the firmware's `POST /api/esp8266-sync` with exactly the shape the
real backend returns:

    {"status": "success", "current_state": <state>}

`current_state` starts as "working" and is controlled live from the keyboard in
this terminal. The state is *sticky* (it stays until you change it) so you can
actually watch the focus timer pause and resume:

    SPACE  -> distracted   (firmware pauses the focus timer, blue LED blinks)
    ENTER  -> away         (same effect as distracted)
    w      -> working      (firmware resumes the focus timer)
    q      -> quit

Stdlib only — run it with any Python 3:

    python simulate_backend.py

Then point the firmware's `serverHost` at this machine's IP (printed below) and
keep `serverPort` = 8000.
"""

import json
import socket
import threading
import time
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HOST = "0.0.0.0"
PORT = 8000
PATH = "/api/esp8266-sync"

VALID_STATES = ("working", "distracted", "away")

# Shared state: HTTP requests run on server threads, the keyboard loop on main.
_state = "working"
_state_lock = threading.Lock()


def get_state() -> str:
    with _state_lock:
        return _state


def set_state(new_state: str) -> None:
    global _state
    with _state_lock:
        _state = new_state
    print(f"  [state] now -> {new_state}", flush=True)


class SyncHandler(BaseHTTPRequestHandler):
    def _respond(self) -> None:
        state = get_state()
        payload = json.dumps({"status": "success", "current_state": state}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_POST(self) -> None:
        length = int(self.headers.get("Content-Length", 0) or 0)
        body = self.rfile.read(length) if length else b""
        try:
            data = json.loads(body) if body else {}
        except json.JSONDecodeError:
            data = body.decode("utf-8", "replace")
        print(f"[POST {self.path}] telemetry={data} -> current_state={get_state()}",
              flush=True)
        self._respond()

    def do_GET(self) -> None:
        # Convenience: open http://<ip>:8000/ in a browser to see the state.
        self._respond()

    def log_message(self, *args) -> None:
        pass  # silence the default per-request logging; we print our own


def local_ip() -> str:
    """Best-effort primary LAN IP (the address the ESP should POST to)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def read_key() -> str:
    """Blocking read of a single keypress. Windows (msvcrt) with POSIX fallback."""
    try:
        import msvcrt
        ch = msvcrt.getch()
        if ch in (b"\x00", b"\xe0"):  # arrow/function key: swallow the 2nd byte
            msvcrt.getch()
            return ""
        return ch.decode("latin-1")
    except ImportError:
        import sys
        import termios
        import tty
        fd = sys.stdin.fileno()
        old = termios.tcgetattr(fd)
        try:
            tty.setraw(fd)
            return sys.stdin.read(1)
        finally:
            termios.tcsetattr(fd, termios.TCSADRAIN, old)


def print_controls() -> None:
    print("Controls:  SPACE=distracted   ENTER=away   w=working   q=quit",
          flush=True)


def keyboard_loop(server: ThreadingHTTPServer) -> None:
    print_controls()
    while True:
        key = read_key()
        if key == " ":
            set_state("distracted")
        elif key in ("\r", "\n"):
            set_state("away")
        elif key in ("w", "W"):
            set_state("working")
        elif key in ("q", "Q", "\x03"):  # q or Ctrl-C
            print("Shutting down...", flush=True)
            server.shutdown()
            return
        # any other key: ignore


def main() -> None:
    parser = argparse.ArgumentParser(description="Study-Buddy backend simulator")
    parser.add_argument("--auto", action="store_true", help="Automatically cycle between working (30s) and away (10s) to simulate a study phase and break.")
    args = parser.parse_args()

    server = ThreadingHTTPServer((HOST, PORT), SyncHandler)
    server_thread = threading.Thread(target=server.serve_forever, daemon=True)
    server_thread.start()

    print(f"Study-Buddy backend simulator listening on http://{local_ip()}:{PORT}{PATH}")
    print(f"  -> set the firmware's serverHost = \"{local_ip()}\" (serverPort = {PORT})")
    print(f"  -> default current_state = \"{get_state()}\"")
    
    if args.auto:
        print("  -> AUTO MODE: Will automatically cycle states (30s working, 10s away).")
        def auto_cycle():
            while True:
                set_state("working")
                time.sleep(30)
                set_state("away")
                time.sleep(10)
        threading.Thread(target=auto_cycle, daemon=True).start()

    try:
        keyboard_loop(server)
    except KeyboardInterrupt:
        server.shutdown()
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
