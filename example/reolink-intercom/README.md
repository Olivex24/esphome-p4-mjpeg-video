# Example: video intercom with a Reolink doorbell

Live doorbell video on the P4 panel plus two-way audio: **Listen** (doorbell microphone → panel speaker) and **Hold to talk** (panel microphone → doorbell speaker). Tested with a **Reolink Video Doorbell WiFi (D340W, dual-band)**; the same idea fits any camera whose RTSP stream has a backchannel that go2rtc supports.

```
             video  (MJPEG 1280x720)         ┌──────────────────────┐
  doorbell ──RTSP──► go2rtc ──► ffmpeg ──────►│                      │
      ▲                │    listen (WAV 16k)  │  intercom_server.py  │──HTTP──► ESP32-P4 panel
      │                └──► ffmpeg ──────────►│  (your server)       │◄──UDP─── (mic, PCM 16k)
      │  backchannel   ◄── go2rtc ◄── ffmpeg ◄┘                      │
      └──── PCMU/8000 ─────────────────────── └──────────────────────┘
```

| File | What |
|---|---|
| `go2rtc.yaml` | the three go2rtc streams (doorbell with backchannel, listen-only copy, panel voice) |
| `intercom_server.py` | small HTTP/UDP bridge, Python standard library + ffmpeg |
| `panel-intercom.yaml` | ESPHome config for the Waveshare 10.1" (ES8311 speaker, ES7210 mics, buttons, video) |
| `intercom_udp.h` | sends the microphone as UDP packets |

## Why a bridge server

- The panel plays audio through ESPHome's speaker media player, which here decodes **WAV**: the server wraps the doorbell's audio in an endless WAV stream (and sends silence only until the first audio arrives, so the player does not give up — padding *between* chunks made the sound choppy).
- Talking needs the doorbell's **RTSP backchannel**. The panel streams raw PCM over UDP (lowest latency, no codec on the P4); ffmpeg turns it into PCMU 8 kHz and publishes it to go2rtc, which plays it into the backchannel. Latency is about 1 s.
- Listening uses a second go2rtc stream with `#backchannel=0`, so it never takes the backchannel away from talking.

## Setup

1. Add the streams from `go2rtc.yaml` (replace `USER`, `PASSWORD`, `DOORBELL_IP`; with Frigate prefer its `{FRIGATE_...}` environment variables over plain text).
2. Edit the configuration block at the top of `intercom_server.py` (`PANEL_IP`, `BIND_IP`, stream names, `CONTAINER` if ffmpeg lives inside the Frigate container) and run it as a service.
3. In `panel-intercom.yaml` set `server` to your server's LAN address and flash the panel.

## Security

- The server listens on one LAN address, answers **only the panel's IP**, and only serves stream names from its whitelist.
- Allow the HTTP and UDP ports **only from the panel** in your firewall; never forward them to the internet.
- Keep camera credentials out of config files you share (environment variables / `!secret`).
- The server never prints go2rtc API responses: they can contain camera URLs with passwords.

## Tested

go2rtc inside Frigate, Reolink Video Doorbell WiFi D340W (dual-band), ESPHome 2026.10, Waveshare ESP32-P4 10.1". Server smoke test: 86 distinct MJPEG frames in 8 s, WAV listen stream, unknown stream names rejected.
