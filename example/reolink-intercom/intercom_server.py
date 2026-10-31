#!/usr/bin/env python3
# Two-way intercom between an ESP32-P4 panel (ESPHome) and a Reolink doorbell, through go2rtc.
# Standard library only; audio/video work is done by ffmpeg.
#
#   LISTEN   panel GETs /listen.wav            → doorbell microphone as an endless WAV (PCM 16 kHz mono)
#   TALK     panel sends its microphone as raw PCM s16le 16 kHz mono in UDP packets → ffmpeg → PCMU 8 kHz →
#            RTSP publish to go2rtc → go2rtc plays it into the doorbell's RTSP backchannel (speaker).
#            The talk session ends 1 s after the last packet.
#   VIDEO    panel GETs /live.mjpeg?src=NAME   → MJPEG for the mjpeg_video component (1280x720)
#            panel GETs /live.wav?src=NAME     → that camera's audio as WAV
#
# Security: binds to one LAN address only and answers only the panel's IP; stream names come from a whitelist.
# Do not expose these ports to the internet (allow only the panel in your firewall).
import http.server
import queue
import socket
import socketserver
import struct
import subprocess
import threading
import time
import urllib.parse

# ---------------------------------------------------------------- configuration (edit these)
PANEL_IP = "PANEL_IP"              # the ESP32-P4 panel — the only client accepted
BIND_IP = "SERVER_LAN_IP"          # this machine's LAN address (never 0.0.0.0 on a multi-homed host)
HTTP_PORT, UDP_PORT = 8088, 5004
RTSP = "rtsp://127.0.0.1:8554"     # go2rtc RTSP server
GO2RTC_API = "http://127.0.0.1:1984"
CONTAINER = None                   # e.g. "frigate" when ffmpeg/curl live inside the Frigate container
FFMPEG = ["ffmpeg"] if not CONTAINER else ["docker", "exec", "-i", CONTAINER, "/usr/lib/ffmpeg/8.0/bin/ffmpeg"]
CURL = ["curl"] if not CONTAINER else ["docker", "exec", CONTAINER, "curl"]
DOORBELL = "doorbell"              # go2rtc stream with the backchannel (talk target)
DOORBELL_LISTEN = "doorbell_listen"  # go2rtc stream with #backchannel=0 (listening never blocks talking)
PANEL_TALK = "panel_talk"          # go2rtc stream the panel's voice is published to
LIVE = {DOORBELL_LISTEN, "garden", "driveway"}   # streams the panel may watch / listen to
VW, VH = 1280, 720
# ----------------------------------------------------------------

ONE_LISTENER = threading.Semaphore(1)
ONE_VIDEO = threading.Semaphore(1)


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def wav_header(rate=16000, channels=1, bits=16):
    block = channels * bits // 8
    big = 0x7FFFFFF0   # "very long": the stream never ends
    return (b"RIFF" + struct.pack("<I", big + 36) + b"WAVE" +
            b"fmt " + struct.pack("<IHHIIHH", 16, 1, channels, rate, rate * block, block, bits) +
            b"data" + struct.pack("<I", big))


def go2rtc_post(query):
    # the response is not printed: it can contain the camera URL with credentials
    r = subprocess.run(CURL + ["-s", "-o", "/dev/null", "-w", "%{http_code}", "-X", "POST",
                               f"{GO2RTC_API}/api/streams?{query}"], capture_output=True, text=True, timeout=15)
    return r.stdout.strip()


def kill_tagged(tag):
    # killing `docker exec` does not stop ffmpeg inside the container → kill it by its unique tag
    cmd = ["pkill", "-f", tag]
    if CONTAINER:
        cmd = ["docker", "exec", CONTAINER] + cmd
    subprocess.run(cmd, capture_output=True, timeout=10)


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.client_address[0] != PANEL_IP:
            return self.send_error(404)
        path, _, qs = self.path.partition("?")
        q = urllib.parse.parse_qs(qs)
        if path == "/listen.wav":
            # doorbell mic is quiet: x3 with a limiter, a bit more bass, a bit less treble
            return self.audio(DOORBELL_LISTEN, "bass=g=6:f=200,treble=g=-6:f=3000,volume=3.0,alimiter=limit=0.9")
        src = q.get("src", [""])[0]
        if src not in LIVE:
            return self.send_error(404)
        if path == "/live.wav":
            return self.audio(src, "volume=2.0,alimiter=limit=0.9")
        if path == "/live.mjpeg":
            return self.video(src)
        return self.send_error(404)

    def audio(self, src, filt):
        if not ONE_LISTENER.acquire(blocking=False):
            return self.send_error(429)
        tag = "intercom-audio-%d" % int(time.time() * 1000)
        p = subprocess.Popen(FFMPEG + ["-hide_banner", "-loglevel", "error", "-user_agent", tag,
                                       "-fflags", "nobuffer", "-flags", "low_delay",
                                       # short probing: first sound in ~0.7 s (the panel gives up on slow starts)
                                       "-probesize", "4096", "-analyzeduration", "300000", "-rtsp_transport", "tcp",
                                       "-i", f"{RTSP}/{src}", "-map", "0:a:0", "-ac", "1", "-ar", "16000",
                                       "-af", filt, "-t", "300", "-f", "s16le", "-"],
                             stdout=subprocess.PIPE, stdin=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            self.send_response(200)
            self.send_header("Content-Type", "audio/wav")
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            # ESPHome's speaker media player decodes WAV: send the header at once, then raw PCM
            self.wfile.write(wav_header())
            self.wfile.flush()
            chunks = queue.Queue()

            def reader():
                while True:
                    b = p.stdout.read(1600)
                    chunks.put(b)
                    if not b:
                        break
            threading.Thread(target=reader, daemon=True).start()
            silence = bytes(1600)   # 50 ms
            started = False
            while True:
                # silence ONLY until the first audio arrives (and on gaps > 1 s); padding between ffmpeg's regular
                # chunks would make the sound choppy
                try:
                    b = chunks.get(timeout=1.0 if started else 0.05)
                    started = True
                except queue.Empty:
                    b = silence
                if not b:
                    break
                self.wfile.write(b)
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            p.kill()
            p.wait()
            kill_tagged(tag)
            ONE_LISTENER.release()

    def video(self, src):
        if not ONE_VIDEO.acquire(blocking=False):
            return self.send_error(429)
        tag = "intercom-video-%d" % int(time.time() * 1000)
        p = subprocess.Popen(FFMPEG + ["-hide_banner", "-loglevel", "error", "-user_agent", tag,
                                       "-fflags", "nobuffer", "-flags", "low_delay", "-probesize", "32768",
                                       "-analyzeduration", "500000", "-rtsp_transport", "tcp", "-i", f"{RTSP}/{src}",
                                       "-an", "-vf", f"scale={VW}:{VH}:force_original_aspect_ratio=decrease,"
                                                     f"pad={VW}:{VH}:(ow-iw)/2:(oh-ih)/2",
                                       # without passthrough ffmpeg repeated the first frame forever (frozen image)
                                       "-fps_mode", "passthrough",
                                       "-q:v", "7", "-flush_packets", "1", "-t", "300", "-f", "mjpeg", "-"],
                             stdout=subprocess.PIPE, stdin=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            self.send_response(200)
            self.send_header("Content-Type", "video/x-motion-jpeg")
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            while True:
                b = p.stdout.read1(65536)   # read1: forward each frame as soon as it exists (read(n) batches them)
                if not b:
                    break
                self.wfile.write(b)
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            p.kill()
            p.wait()
            kill_tagged(tag)
            ONE_VIDEO.release()


class Talk:
    def __init__(self):
        self.p = None
        self.last = 0.0
        self.lock = threading.Lock()

    def start(self):
        log("talk: start")
        self.p = subprocess.Popen(FFMPEG + ["-hide_banner", "-loglevel", "error",
                                            "-f", "s16le", "-ar", "16000", "-ac", "1", "-i", "-",
                                            "-af", "volume=2.0", "-c:a", "pcm_mulaw", "-ar", "8000",
                                            "-f", "rtsp", "-rtsp_transport", "tcp", f"{RTSP}/{PANEL_TALK}"],
                                  stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        threading.Thread(target=self._connect, daemon=True).start()

    def _connect(self):
        time.sleep(0.6)   # ffmpeg must have published the stream before go2rtc is asked to play it
        log("talk: play into doorbell", go2rtc_post(f"dst={DOORBELL}&src={RTSP}/{PANEL_TALK}"))

    def stop(self):
        log("talk: stop", go2rtc_post(f"dst={DOORBELL}&src="))
        try:
            self.p.stdin.close()
        except Exception:
            pass
        try:
            self.p.wait(timeout=3)
        except Exception:
            self.p.kill()
        self.p = None

    def udp_loop(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind((BIND_IP, UDP_PORT))
        s.settimeout(0.25)
        while True:
            try:
                data, addr = s.recvfrom(4096)
            except socket.timeout:
                data, addr = None, None
            with self.lock:
                if data and addr[0] == PANEL_IP:
                    if self.p is None:
                        self.start()
                    self.last = time.time()
                    try:
                        self.p.stdin.write(data)
                    except Exception:
                        pass
                elif self.p is not None and time.time() - self.last > 1.0:
                    self.stop()


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


if __name__ == "__main__":
    t = Talk()
    threading.Thread(target=t.udp_loop, daemon=True).start()
    log("intercom server: http", HTTP_PORT, "udp", UDP_PORT)
    Server((BIND_IP, HTTP_PORT), Handler).serve_forever()
