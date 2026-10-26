"""
esp_stream.py — auto-detect the ESP32-S3 BadUSB stick on your PC, capture
webcam frames, and stream them via a tiny local HTTP server (MJPEG) that the
ESP's web UI can embed as an <img> or that any browser can open directly.

Why a local server: pushing frames all the way through the ESP's 4 MHz WiFi
AP would cap around 2-5 fps. Serving them from the PC that's already on the
same LAN (or the ESP's own AP) hits 20-30 fps easily with default JPEG
quality, and any browser tab pointed at http://<pc-ip>:8765/mjpeg gets the
live feed.

Standalone-executable build:
    pip install pyinstaller opencv-python pyserial
    pyinstaller --onefile --console --name ESPStream esp_stream.py

Run it, note the URL it prints, and the ESP web UI's "Streaming" tab (if
configured) or any browser embeds it as <img src="http://<pc-ip>:8765/mjpeg">.
"""
from __future__ import annotations

import argparse
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:
    import cv2  # opencv-python
except ImportError:
    sys.exit("Missing dependency: pip install opencv-python")

try:
    from serial.tools import list_ports
except ImportError:
    sys.exit("Missing dependency: pip install pyserial")


ESP_VID = 0x303A  # Espressif


def detect_esp_port() -> str | None:
    """Return the COM/tty path of the first ESP32-S3 that appears, or None."""
    for p in list_ports.comports():
        if p.vid == ESP_VID:
            return p.device
    return None


def wait_for_esp(timeout: float = 15.0) -> str | None:
    t0 = time.time()
    while time.time() - t0 < timeout:
        port = detect_esp_port()
        if port:
            return port
        time.sleep(0.4)
    return None


class Frames:
    """Thread-safe holder for the latest JPEG frame."""
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._frame: bytes = b""
        self._n = 0

    def put(self, jpeg: bytes) -> None:
        with self._lock:
            self._frame = jpeg
            self._n += 1

    def get(self) -> tuple[bytes, int]:
        with self._lock:
            return self._frame, self._n


def capture_loop(frames: Frames, cam_index: int, target_fps: float, jpeg_q: int,
                 stop_flag: threading.Event) -> None:
    cap = cv2.VideoCapture(cam_index, cv2.CAP_DSHOW if sys.platform == "win32" else 0)
    if not cap.isOpened():
        print(f"[cam] could not open capture device {cam_index}", file=sys.stderr)
        stop_flag.set()
        return
    # Try to hint the camera at a reasonable resolution/fps; some drivers ignore.
    cap.set(cv2.CAP_PROP_FRAME_WIDTH,  640)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)
    cap.set(cv2.CAP_PROP_FPS,          target_fps)
    period = 1.0 / max(1.0, target_fps)
    encode_params = [int(cv2.IMWRITE_JPEG_QUALITY), jpeg_q]
    n = 0
    t0 = time.time()
    while not stop_flag.is_set():
        ok, frame = cap.read()
        if not ok:
            time.sleep(0.05)
            continue
        ok, buf = cv2.imencode(".jpg", frame, encode_params)
        if not ok:
            continue
        frames.put(buf.tobytes())
        n += 1
        if n % 60 == 0:
            fps = n / (time.time() - t0)
            print(f"[cam] {fps:5.1f} fps served")
        elapsed = time.time() - t0
        target = n * period
        if elapsed < target:
            time.sleep(target - elapsed)
    cap.release()


class MJPEGHandler(BaseHTTPRequestHandler):
    frames: Frames = None       # set from main
    def log_message(self, *a, **kw): pass  # quiet

    def do_GET(self) -> None:
        if self.path in ("/", "/mjpeg", "/stream.mjpg"):
            return self._serve_mjpeg()
        if self.path in ("/snapshot", "/snapshot.jpg"):
            return self._serve_snapshot()
        if self.path == "/status":
            return self._serve_status()
        self.send_response(404)
        self.end_headers()

    def _serve_mjpeg(self) -> None:
        self.send_response(200)
        self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        last_n = -1
        while True:
            try:
                jpeg, n = self.frames.get()
                if n == last_n or not jpeg:
                    time.sleep(0.005)
                    continue
                last_n = n
                self.wfile.write(b"--frame\r\n")
                self.wfile.write(b"Content-Type: image/jpeg\r\n")
                self.wfile.write(f"Content-Length: {len(jpeg)}\r\n\r\n".encode())
                self.wfile.write(jpeg)
                self.wfile.write(b"\r\n")
            except (BrokenPipeError, ConnectionResetError):
                return
            except Exception as e:
                print(f"[srv] mjpeg client error: {e}", file=sys.stderr)
                return

    def _serve_snapshot(self) -> None:
        jpeg, _ = self.frames.get()
        if not jpeg:
            self.send_response(503); self.end_headers(); return
        self.send_response(200)
        self.send_header("Content-Type", "image/jpeg")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        self.wfile.write(jpeg)

    def _serve_status(self) -> None:
        jpeg, n = self.frames.get()
        body = f'{{"frames":{n},"lastBytes":{len(jpeg)}}}'.encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)


def get_lan_ip() -> str:
    """Best-effort LAN IP (works even when there is no default gateway)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("10.255.255.255", 1))  # doesn't actually send anything
        return s.getsockname()[0]
    except Exception:
        return "127.0.0.1"
    finally:
        s.close()


def main() -> int:
    ap = argparse.ArgumentParser(description="Stream a webcam to the ESP32-S3 BadUSB web UI.")
    ap.add_argument("--cam",  type=int,   default=0,     help="OpenCV camera index (default 0)")
    ap.add_argument("--fps",  type=float, default=15.0,  help="Target frames per second (default 15)")
    ap.add_argument("--port", type=int,   default=8765,  help="HTTP server port (default 8765)")
    ap.add_argument("--jpeg", type=int,   default=70,    help="JPEG quality 1..100 (default 70)")
    ap.add_argument("--no-wait", action="store_true",    help="Don't wait for the ESP to appear")
    args = ap.parse_args()

    if not args.no_wait:
        print("[esp] scanning USB for VID_303A ...")
        esp = wait_for_esp(timeout=15)
        if esp:
            print(f"[esp] found ESP on {esp}")
        else:
            print("[esp] no ESP detected — continuing anyway; stream still works.")

    ip = get_lan_ip()
    print(f"[srv] serving MJPEG at:")
    print(f"       http://{ip}:{args.port}/mjpeg          (video stream)")
    print(f"       http://{ip}:{args.port}/snapshot.jpg   (single frame)")
    print(f"       http://{ip}:{args.port}/status         (JSON stats)")

    frames = Frames()
    MJPEGHandler.frames = frames

    stop = threading.Event()
    cap_t = threading.Thread(target=capture_loop,
                             args=(frames, args.cam, args.fps, args.jpeg, stop),
                             daemon=True)
    cap_t.start()

    srv = ThreadingHTTPServer(("0.0.0.0", args.port), MJPEGHandler)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\n[srv] shutting down.")
    finally:
        stop.set()
        srv.server_close()
        cap_t.join(timeout=1.0)
    return 0


if __name__ == "__main__":
    sys.exit(main())
