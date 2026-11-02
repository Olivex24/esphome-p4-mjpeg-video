#pragma once
// mjpeg_video — smooth MJPEG-over-HTTP video on ESP32-P4 for ESPHome.
//
// Pipeline (no CPU pixel work):
//   HTTP (esp_http_client, own FreeRTOS task)
//     → hardware JPEG decoder (driver/jpeg_decode.h) → RGB565
//     → PPA SRM (driver/ppa.h): scale + rotate in hardware
//     → written straight into the MIPI-DSI (DPI) frame buffer.
//
// LVGL is bypassed for the video itself. Rendering a 1280x720 image through LVGL (software blend + rotation +
// copy) only reached ~6 fps on the Waveshare ESP32-P4 10.1"; the direct path draws every frame (measured 16-27 fps
// at 1280x720, decode ~13 ms + PPA ~30 ms per frame).
//
// Without `display:` the component falls back to updating an LVGL image widget (slower, but simple).

#include <atomic>
#include <string>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/components/display/display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "driver/jpeg_decode.h"
#include "driver/ppa.h"

namespace esphome {
namespace mjpeg_video {

class MjpegVideo : public Component {
 public:
  void set_size(int w, int h) { this->w_ = w; this->h_ = h; }
  void set_bgr(bool b) { this->bgr_ = b; }
  /// image (and its layer) to draw on; switching layers (e.g. cameras <-> photo viewer) restores the old page first
  void set_image(lv_obj_t *img) {
    if (img != this->img_ && this->siblings_hidden_)
      this->stop();
    this->img_ = img;
  }
  void set_display(display::Display *d) { this->disp_ = d; }
  /// rotation configured for LVGL (0/90/180/270, clockwise) — the direct path must rotate the same way
  void set_rotation(int r) { this->rotation_ = r; }
  /// area (LVGL coordinates) the video is fitted into, keeping aspect ratio; call before start(). w/h <= 0 = full screen
  void set_rect(int x, int y, int w, int h) { this->rx_ = x; this->ry_ = y; this->rw_ = w; this->rh_ = h; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  /// start (or switch) the stream; safe to call from lambdas. repeat=false: do not reconnect at the end (clips)
  void start(const std::string &url, bool repeat = true);
  /// stop the stream (connection closes within ~0.5 s); in direct mode the LVGL page is restored
  void stop();
  bool is_running() const { return this->want_run_.load(); }
  uint32_t frames() const { return this->frames_.load(); }

 protected:
  static void task_trampoline_(void *arg);
  void task_();
  void stream_once_(const std::string &url, uint32_t gen);
  void decode_(size_t len);
  void draw_direct_(uint32_t w, uint32_t h, uint32_t hp);
  void hide_siblings_();
  void restore_siblings_();

  int w_{640};
  int h_{360};
  int hpad_{368};
  bool bgr_{true};
  lv_obj_t *img_{nullptr};
  lv_image_dsc_t dsc_[2]{};
  uint8_t *out_[2]{nullptr, nullptr};
  size_t out_size_{0};
  uint8_t *in_{nullptr};
  size_t in_cap_{0};
  uint8_t *rb_{nullptr};
  jpeg_decoder_handle_t dec_{nullptr};
  TaskHandle_t task_h_{nullptr};
  SemaphoreHandle_t mtx_{nullptr};
  std::string url_;
  std::atomic<bool> want_run_{false};
  std::atomic<bool> repeat_{true};
  std::atomic<uint32_t> gen_{0};
  std::atomic<int> ready_{-1};   // LVGL path: buffer ready to show (-1 = none)
  std::atomic<int> shown_{0};    // LVGL path: buffer currently shown by LVGL (never written)
  std::atomic<uint32_t> frames_{0};
  bool warned_size_{false};
  // statistics, logged every 5 s while running
  std::atomic<uint32_t> st_rx_{0}, st_dec_{0}, st_skip_{0}, st_dec_us_max_{0};
  uint32_t st_shown_{0}, st_gap_max_{0}, st_t_log_{0}, st_t_shown_{0};
  // direct path (PPA → frame buffer)
  display::Display *disp_{nullptr};
  int rotation_{0};
  bool fb_looked_up_{false};
  void *fb_{nullptr};
  size_t fb_size_{0};
  int fb_w_{0}, fb_h_{0};                // physical panel (e.g. 800x1280 portrait)
  ppa_client_handle_t ppa_{nullptr};
  std::atomic<bool> direct_{false};      // current stream uses the direct path
  std::atomic<bool> busy_{false};        // video task is writing into the frame buffer right now
  uint32_t cur_gen_{0};                  // generation of the stream being read (video task only)
  std::atomic<uint32_t> st_ppa_{0}, st_ppa_us_max_{0}, st_ppa_err_{0}, st_gap_dir_{0};
  int64_t t_dir_{0};                     // time of the last direct draw (video task only)
  int rx_{0}, ry_{0}, rw_{0}, rh_{0};
  std::vector<lv_obj_t *> hidden_;       // page objects hidden while video runs (LVGL would repaint them over it)
  bool siblings_hidden_{false};
};

}  // namespace mjpeg_video
}  // namespace esphome
