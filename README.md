# mjpeg_video — smooth camera video on ESP32-P4 with ESPHome

An ESPHome external component that plays **MJPEG over HTTP at 1280×720, 16–27 fps** on an ESP32-P4 MIPI-DSI display (tested on the Waveshare ESP32-P4-WIFI6-Touch-LCD-X 10.1"), with the CPU doing almost no pixel work.

Use it for camera feeds (Frigate / go2rtc / any ffmpeg source), doorbell clips, intercom screens — anything where an LVGL dashboard needs real video, not a snapshot every second.

## Why

Showing video through LVGL on the P4 tops out around **6 fps at 1280×720**: every frame is blended in software, rotated, then copied to the panel. Snapshot widgets (`online_image`) are worse — their software JPEG decode blocks the main loop for 300–650 ms per image.

This component uses the P4's hardware instead:

```
HTTP (own FreeRTOS task)
  → hardware JPEG decoder            ~13 ms per 1280×720 frame
  → PPA (scale + rotate, hardware)   ~30 ms
  → straight into the DPI frame buffer
```

LVGL is bypassed for the video itself, so a busy main loop (Home Assistant API, BLE proxy, other widgets) does not drop frames.

## How we got there (measure, don't guess)

Every step below was driven by numbers the component logs every 5 s (`received / decoded / skipped / shown`, decode time, longest gap between frames):

1. **Snapshots → MJPEG.** Polling camera snapshots through `online_image` gave ~1 frame per second. Switching to a continuous MJPEG stream decoded by the P4's **hardware JPEG engine** brought decode time down to 6–20 ms per frame.
2. **The main loop was the bottleneck, not the network.** Frames arrived at 12 fps, but only 3–11 were shown: the image could only be swapped from ESPHome's main loop, and that loop was busy. ESPHome's own "took a long time" warnings pointed at `online_image`: grid thumbnails hidden *behind* the video were still being downloaded and decoded in software, 300–650 ms each. Pausing them while video is open: 55–59 of 60 frames shown at 800×448.
3. **LVGL itself caps out at ~6 fps for 1280×720.** Even with an idle loop, drawing a full-size frame (software blend + rotation + copy to the panel) costs ~150 ms. LVGL's PPA draw unit exists but ESPHome keeps it disabled for stability, and we did not want to change that for the whole UI.
4. **So the video skips LVGL.** The decoded frame goes through the **PPA** (the P4's 2D accelerator) which rotates/scales it in hardware and writes it directly into the MIPI-DSI frame buffer, from the video task. The LVGL page underneath is hidden while video runs so nothing repaints over it. Result: **every frame drawn, 16–27 fps at 1280×720**, ~43 ms of hardware time per frame and almost zero CPU.
5. **The server matters too.** A few ffmpeg/Python details cost us hours (see *Serving the stream* below): a buffered `read()` that delivered frames in bursts, a missing `-fps_mode passthrough` that made ffmpeg repeat one frame forever, and a queue on the server that turned a slow Wi-Fi link into slow motion.

## Measured (Waveshare 10.1", LVGL rotation 270, ESP-IDF 5.5, ESPHome 2026.10)

| Path | 1280×720 frames received / shown per 5 s |
|---|---|
| LVGL image widget | 60 / 14–33 |
| **mjpeg_video direct (this)** | **80–135 / 80–135** (every frame) |

## Install

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/Olivex24/esphome-p4-mjpeg-video
    components: [mjpeg_video]

mjpeg_video:
  id: video
  width: 1280          # largest frame accepted (frame width must be a multiple of 32)
  height: 720
  display: my_display  # your mipi_dsi display → enables the direct (hardware) path
  rotation: 270        # must match `lvgl: rotation:`
```

Give it the LVGL image that sits on your video layer, then start/stop from any lambda:

```yaml
esphome:
  on_boot:
    priority: -100
    then:
      - lambda: 'id(video).set_image(id(img_video));'
```

```cpp
id(video).set_rect(0, 40, 1280, 720);   // area to fit the video into (LVGL coordinates); optional, default = full screen
id(video).start("http://YOUR_SERVER/stream.mjpeg");
// ...
id(video).stop();
```

`start(url, false)` plays a clip once and keeps the last frame instead of reconnecting.

A complete minimal example is in [`example/p4-video-example.yaml`](example/p4-video-example.yaml).

**Video intercom with a Reolink doorbell** (live video + Listen + Hold to talk, through go2rtc): see [`example/reolink-intercom`](example/reolink-intercom).

## How the direct path coexists with LVGL

The video is written into the frame buffer behind LVGL's back. To keep LVGL from painting over it:

- the video image widget must sit on a full-screen, opaque **layer object** (a plain `obj` with a black background) that is a child of the page;
- while video runs, every *other* child of the page is hidden (hidden objects never repaint — e.g. a clock under the layer would otherwise flash through once a second). If your code shows one of them meanwhile, it is hidden again and restored later;
- labels/buttons placed **inside** the video layer keep working; keep them outside the video rect (`set_rect`) so the video does not cover them;
- hiding the layer (or calling `stop()`) restores the page and repaints it.

## Serving the stream

Any HTTP response containing back-to-back JPEGs works (frames are found by their SOI/EOI markers, `multipart/x-mixed-replace` or raw). The JPEG width must be a **multiple of 32**. Example with ffmpeg from an RTSP source (e.g. go2rtc/Frigate):

```sh
ffmpeg -fflags nobuffer -flags low_delay -rtsp_transport tcp -i rtsp://YOUR_RTSP_SOURCE \
  -an -vf "scale=1280:720:force_original_aspect_ratio=decrease,pad=1280:720:(ow-iw)/2:(oh-ih)/2" \
  -fps_mode passthrough -q:v 7 -f mjpeg -
```

Lessons learned the hard way:

- **`-fps_mode passthrough`** — without it (and without an `fps` filter) ffmpeg kept repeating the *first* frame forever (245 identical frames in 4 s): the screen froze although the P4 was drawing 27 fps.
- When piping ffmpeg's output through Python, use `stdout.read1()`, not `read(16384)`: the latter waits until 2–3 small frames pile up → stutter and delay.
- **Send only the newest frame.** If the panel's Wi-Fi cannot keep up with a high-bitrate camera (one of ours: ~79 KB × 20 fps ≈ 12.6 Mbps), a plain pipe lets 0.5–0.7 MB queue up on the server: the video plays in slow motion and the delay keeps growing. Keep one "latest frame" slot that a reader thread overwrites, write from it, and shrink the socket send buffer (`SO_SNDBUF` 32 KB) — then a slow link only lowers the frame rate and the picture stays real-time. `example/reolink-intercom/intercom_server.py` does exactly this.
- **Frame width must be a multiple of 32**, not just 16: the P4 hardware decoder aligns each RGB565 row to 64 bytes, so 528- or 1072-pixel-wide frames come out sheared into diagonal stripes.

Wrap it in a tiny HTTP server (or use go2rtc's own MJPEG endpoint). Keep it on your local network and allow only the panel to connect.

## Notes / limits

- ESP32-P4 only (hardware JPEG + PPA). Tested with ESP-IDF 5.5.6 and LVGL 9.5 on ESPHome 2026.10.
- Rotation 270 is what has been tested on hardware; 0/90/180 follow the same mapping ESPHome's LVGL flush uses but are untested.
- The panel's frame buffer is single (`num_fbs = 1`), so very fast motion can show slight tearing.
- ESPHome intentionally keeps LVGL's own PPA draw unit disabled (`LV_USE_PPA 0`, "until upstream bugs fixed") — this component does not touch that.
- The frame-buffer lookup reads `MipiDsi::handle_` through an explicit-template-instantiation trick because the class is `final` with the handle protected. If ESPHome changes that member, the component falls back to the (slower) LVGL path and logs a warning.

## License

MIT
