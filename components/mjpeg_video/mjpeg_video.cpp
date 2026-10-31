#include "mjpeg_video.h"

#include <algorithm>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/components/mipi_dsi/mipi_dsi.h"
#include "esp_lcd_mipi_dsi.h"
// lv_image_cache_drop is not in the public lvgl.h; use it from the LVGL sources when available
#if __has_include("src/misc/cache/instance/lv_image_cache.h")
#include "src/misc/cache/instance/lv_image_cache.h"
#define MJV_HAS_CACHE_DROP 1
#endif

// MipiDsi is `final` and keeps the panel handle protected. We only need it to look up the DPI frame buffer, so we
// read it through an explicit template instantiation (the standard allows naming protected members there).
namespace {
template<typename Tag, typename Tag::type M> struct Grab {
  friend typename Tag::type get(Tag) { return M; }
};
struct DsiHandle {
  typedef esp_lcd_panel_handle_t esphome::mipi_dsi::MipiDsi::*type;
  friend type get(DsiHandle);
};
template struct Grab<DsiHandle, &esphome::mipi_dsi::MipiDsi::handle_>;
}  // namespace

namespace esphome {
namespace mjpeg_video {

static const char *const TAG = "mjpeg_video";
static const size_t IN_CAP = 256 * 1024;   // one 1280x720 JPEG at q:v 7 is ~30-60 KB
static const size_t RB_SIZE = 8192;

void MjpegVideo::setup() {
  this->hpad_ = (this->h_ + 15) & ~15;   // the decoder writes in 16-line blocks
  jpeg_decode_memory_alloc_cfg_t out_cfg = {.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER};
  jpeg_decode_memory_alloc_cfg_t in_cfg = {.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER};
  for (int i = 0; i < 2; i++) {
    size_t got = 0;
    this->out_[i] = (uint8_t *) jpeg_alloc_decoder_mem((size_t) this->w_ * this->hpad_ * 2, &out_cfg, &got);
    if (this->out_[i] == nullptr) {
      ESP_LOGE(TAG, "Cannot allocate output buffer %d", i);
      this->mark_failed();
      return;
    }
    this->out_size_ = got;
    memset(this->out_[i], 0, got);
    lv_image_dsc_t &d = this->dsc_[i];
    memset(&d, 0, sizeof(d));
    d.header.magic = LV_IMAGE_HEADER_MAGIC;
    d.header.cf = LV_COLOR_FORMAT_RGB565;
    d.header.w = this->w_;
    d.header.h = this->h_;
    d.header.stride = this->w_ * 2;
    d.data_size = (uint32_t) this->w_ * this->h_ * 2;
    d.data = this->out_[i];
  }
  this->in_ = (uint8_t *) jpeg_alloc_decoder_mem(IN_CAP, &in_cfg, &this->in_cap_);
  this->rb_ = (uint8_t *) heap_caps_malloc(RB_SIZE, MALLOC_CAP_SPIRAM);
  if (this->in_ == nullptr || this->rb_ == nullptr) {
    ESP_LOGE(TAG, "Cannot allocate input buffers");
    this->mark_failed();
    return;
  }
  jpeg_decode_engine_cfg_t ec = {.intr_priority = 0, .timeout_ms = 200};
  if (jpeg_new_decoder_engine(&ec, &this->dec_) != ESP_OK) {
    ESP_LOGE(TAG, "Cannot start the hardware JPEG decoder");
    this->mark_failed();
    return;
  }
  if (this->disp_ != nullptr) {
    ppa_client_config_t pc = {};
    pc.oper_type = PPA_OPERATION_SRM;
    pc.max_pending_trans_num = 1;
    if (ppa_register_client(&pc, &this->ppa_) != ESP_OK) {
      ESP_LOGW(TAG, "Cannot register a PPA client — using the LVGL path");
      this->ppa_ = nullptr;
    }
  }
  this->mtx_ = xSemaphoreCreateMutex();
  xTaskCreate(&MjpegVideo::task_trampoline_, "mjpeg_video", 6144, this, 4, &this->task_h_);
}

void MjpegVideo::dump_config() {
  ESP_LOGCONFIG(TAG, "MJPEG video (hardware JPEG): max %dx%d (buffer %dx%d), %s order, rotation %d, %s path", this->w_,
                this->h_, this->w_, this->hpad_, this->bgr_ ? "BGR" : "RGB", this->rotation_,
                this->disp_ != nullptr ? "direct (PPA → frame buffer)" : "LVGL");
}

void MjpegVideo::start(const std::string &url, bool repeat) {
  if (this->mtx_ == nullptr)
    return;
  xSemaphoreTake(this->mtx_, portMAX_DELAY);
  this->url_ = url;
  xSemaphoreGive(this->mtx_);
  this->ready_.store(-1);
  this->repeat_.store(repeat);
  bool d = this->fb_ != nullptr && this->ppa_ != nullptr && this->img_ != nullptr;
  if (d) {
    this->hide_siblings_();
    lv_obj_invalidate(lv_obj_get_parent(this->img_));   // repaint the (black) layer: no leftovers if the rect changed
  }
  this->direct_.store(d);
  this->gen_.fetch_add(1);
  this->want_run_.store(true);
  this->st_t_shown_ = 0;
  this->st_shown_ = 0;
  this->st_gap_max_ = 0;
  this->st_t_log_ = millis();
  ESP_LOGI(TAG, "Start%s: %s", d ? " (direct)" : "", url.c_str());
}

void MjpegVideo::stop() {
  bool was_running = this->want_run_.exchange(false);
  this->gen_.fetch_add(1);
  this->ready_.store(-1);
  // direct path: let a frame in progress finish (PPA ~30 ms) before LVGL repaints the page over it
  for (int i = 0; i < 100 && this->busy_.load(); i++)
    vTaskDelay(pdMS_TO_TICKS(2));
  this->direct_.store(false);
  this->restore_siblings_();
  if (was_running)
    ESP_LOGI(TAG, "Stop (%u frames total)", (unsigned) this->frames_.load());
}

// The video is written behind LVGL's back, so anything LVGL repaints in that area (a clock ticking on the page
// underneath the video layer, for example) would flash over it. While the video runs, every other child of the
// page is hidden; hidden objects never invalidate. They are shown again on stop().
void MjpegVideo::hide_siblings_() {
  if (this->siblings_hidden_)
    return;
  lv_obj_t *layer = lv_obj_get_parent(this->img_);
  lv_obj_t *page = layer != nullptr ? lv_obj_get_parent(layer) : nullptr;
  if (page == nullptr)
    return;
  lv_obj_add_flag(this->img_, LV_OBJ_FLAG_HIDDEN);
  uint32_t n = lv_obj_get_child_count(page);
  for (uint32_t i = 0; i < n; i++) {
    lv_obj_t *c = lv_obj_get_child(page, i);
    if (c == layer || lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN))
      continue;
    lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
    this->hidden_.push_back(c);
  }
  this->siblings_hidden_ = true;
}

void MjpegVideo::restore_siblings_() {
  if (!this->siblings_hidden_)
    return;
  for (lv_obj_t *c : this->hidden_)
    lv_obj_clear_flag(c, LV_OBJ_FLAG_HIDDEN);
  this->hidden_.clear();
  this->siblings_hidden_ = false;
  lv_obj_clear_flag(this->img_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_t *layer = lv_obj_get_parent(this->img_);
  lv_obj_t *page = layer != nullptr ? lv_obj_get_parent(layer) : nullptr;
  lv_obj_invalidate(page != nullptr ? page : layer);   // full repaint over the last video frame
}

void MjpegVideo::loop() {
  uint32_t now = millis();
  // look up the frame buffer once, after the display has been set up
  if (!this->fb_looked_up_ && this->disp_ != nullptr && this->ppa_ != nullptr) {
    this->fb_looked_up_ = true;
    auto *dsi = static_cast<mipi_dsi::MipiDsi *>(this->disp_);
    esp_lcd_panel_handle_t h = dsi->*get(DsiHandle());
    void *fb = nullptr;
    if (h != nullptr && esp_lcd_dpi_panel_get_frame_buffer(h, 1, &fb) == ESP_OK && fb != nullptr) {
      this->fb_w_ = dsi->get_width_internal();
      this->fb_h_ = dsi->get_height_internal();
      this->fb_size_ = (size_t) this->fb_w_ * this->fb_h_ * 2;
      this->fb_ = fb;
      ESP_LOGI(TAG, "Direct path: panel %dx%d, frame buffer %p", this->fb_w_, this->fb_h_, fb);
    } else {
      ESP_LOGW(TAG, "Frame buffer not found — using the LVGL path");
    }
  }
  if (this->want_run_.load() && now - this->st_t_log_ >= 5000) {
    if (this->direct_.load()) {
      ESP_LOGI(TAG, "5 s: received %u, decoded %u, drawn %u (direct) | decode max %u ms, PPA max %u ms, errors %u, max gap %u ms",
               (unsigned) this->st_rx_.exchange(0), (unsigned) this->st_dec_.exchange(0),
               (unsigned) this->st_ppa_.exchange(0), (unsigned) (this->st_dec_us_max_.exchange(0) / 1000),
               (unsigned) (this->st_ppa_us_max_.exchange(0) / 1000), (unsigned) this->st_ppa_err_.exchange(0),
               (unsigned) (this->st_gap_dir_.exchange(0) / 1000));
    } else {
      ESP_LOGI(TAG, "5 s: received %u, decoded %u, skipped %u, shown %u | decode max %u ms, max gap %u ms",
               (unsigned) this->st_rx_.exchange(0), (unsigned) this->st_dec_.exchange(0),
               (unsigned) this->st_skip_.exchange(0), (unsigned) this->st_shown_,
               (unsigned) (this->st_dec_us_max_.exchange(0) / 1000), (unsigned) this->st_gap_max_);
    }
    this->st_shown_ = 0;
    this->st_gap_max_ = 0;
    this->st_t_log_ = now;
  }
  if (this->siblings_hidden_) {
    lv_obj_t *layer = lv_obj_get_parent(this->img_);
    // the video layer was closed (by any means): stop right away and give the page back
    if (lv_obj_has_flag(layer, LV_OBJ_FLAG_HIDDEN)) {
      this->stop();
      return;
    }
    // some other code showed a page object meanwhile (a badge, a screensaver...): hide it again and restore it on
    // stop — otherwise LVGL would paint it over the video
    lv_obj_t *page = lv_obj_get_parent(layer);
    uint32_t n = lv_obj_get_child_count(page);
    for (uint32_t i = 0; i < n; i++) {
      lv_obj_t *c = lv_obj_get_child(page, i);
      if (c != layer && !lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
        this->hidden_.push_back(c);
      }
    }
    return;   // the direct path does not use the LVGL image
  }
  // LVGL path: show the new buffer, if any (only the main loop touches LVGL)
  int r = this->ready_.load();
  if (r < 0 || this->img_ == nullptr || !this->want_run_.load())
    return;
  if (this->st_t_shown_ != 0 && now - this->st_t_shown_ > this->st_gap_max_)
    this->st_gap_max_ = now - this->st_t_shown_;
  this->st_t_shown_ = now;
  this->st_shown_++;
  this->shown_.store(r);
#ifdef MJV_HAS_CACHE_DROP
  lv_image_cache_drop(&this->dsc_[r]);
#endif
  lv_image_set_src(this->img_, &this->dsc_[r]);
  lv_obj_invalidate(this->img_);
  this->ready_.store(-1);
}

void MjpegVideo::task_trampoline_(void *arg) { static_cast<MjpegVideo *>(arg)->task_(); }

void MjpegVideo::task_() {
  while (true) {
    if (!this->want_run_.load()) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    uint32_t g = this->gen_.load();
    xSemaphoreTake(this->mtx_, portMAX_DELAY);
    std::string url = this->url_;
    xSemaphoreGive(this->mtx_);
    this->stream_once_(url, g);
    // clip finished (repeat=false): do not reconnect, the last frame stays on screen
    if (!this->repeat_.load() && this->gen_.load() == g) {
      this->want_run_.store(false);
      continue;
    }
    // connection ended: if the same stream is still wanted, retry after 1 s
    for (int i = 0; i < 20 && this->want_run_.load() && this->gen_.load() == g; i++)
      vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void MjpegVideo::stream_once_(const std::string &url, uint32_t g) {
  this->cur_gen_ = g;
  this->t_dir_ = 0;
  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.timeout_ms = 5000;
  cfg.buffer_size = 4096;
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (c == nullptr)
    return;
  if (esp_http_client_open(c, 0) != ESP_OK) {
    ESP_LOGW(TAG, "Cannot connect to %s", url.c_str());
    esp_http_client_cleanup(c);
    return;
  }
  esp_http_client_fetch_headers(c);
  int st = esp_http_client_get_status_code(c);
  if (st != 200) {
    ESP_LOGW(TAG, "HTTP %d from %s", st, url.c_str());
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return;
  }
  // frames are found by their JPEG markers (SOI ... EOI), so any multipart boundary or a raw `-f mjpeg` stream works
  size_t len = 0;
  bool in_frame = false;
  uint8_t prev = 0;
  while (this->want_run_.load() && this->gen_.load() == g) {
    int n = esp_http_client_read(c, (char *) this->rb_, RB_SIZE);
    if (n <= 0)
      break;
    for (int i = 0; i < n; i++) {
      uint8_t b = this->rb_[i];
      if (!in_frame) {
        if (prev == 0xFF && b == 0xD8) {   // SOI
          in_frame = true;
          this->in_[0] = 0xFF;
          this->in_[1] = 0xD8;
          len = 2;
          prev = 0;
          continue;
        }
        prev = b;
        continue;
      }
      if (len < this->in_cap_) {
        this->in_[len++] = b;
      } else {   // frame too large: drop it
        in_frame = false;
        len = 0;
        prev = 0;
        continue;
      }
      if (prev == 0xFF && b == 0xD9) {   // EOI (inside entropy-coded data 0xFF is always followed by 0x00)
        this->st_rx_.fetch_add(1);
        this->decode_(len);
        in_frame = false;
        len = 0;
        prev = 0;
        continue;
      }
      prev = b;
    }
  }
  esp_http_client_close(c);
  esp_http_client_cleanup(c);
}

void MjpegVideo::decode_(size_t len) {
  bool direct = this->direct_.load();
  if (!direct && this->ready_.load() >= 0) {
    this->st_skip_.fetch_add(1);
    return;   // LVGL path: previous frame not shown yet → drop this one (never wait)
  }
  jpeg_decode_picture_info_t info;
  if (jpeg_decoder_get_info(this->in_, len, &info) != ESP_OK)
    return;
  // any size up to the configured one; width must be a multiple of 16
  uint32_t hp = (info.height + 15) & ~15u;
  if ((int) info.width > this->w_ || info.width % 16 != 0 || (size_t) info.width * hp * 2 > this->out_size_) {
    if (!this->warned_size_) {
      ESP_LOGW(TAG, "Frame %ux%u does not fit %dx%d (or width not a multiple of 16) — skipping", (unsigned) info.width,
               (unsigned) info.height, this->w_, this->h_);
      this->warned_size_ = true;
    }
    return;
  }
  // direct path: a single buffer is enough (the PPA reads it, blocking, before the next decode)
  int back = direct ? 0 : (this->shown_.load() == 0 ? 1 : 0);
  if (!direct) {
    lv_image_dsc_t &d = this->dsc_[back];   // the back buffer is not read by LVGL now → header may change
    d.header.w = info.width;
    d.header.h = info.height;
    d.header.stride = info.width * 2;
    d.data_size = (uint32_t) info.width * info.height * 2;
  }
  jpeg_decode_cfg_t dc = {};
  dc.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
  dc.rgb_order = this->bgr_ ? JPEG_DEC_RGB_ELEMENT_ORDER_BGR : JPEG_DEC_RGB_ELEMENT_ORDER_RGB;
  dc.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
  uint32_t out = 0;
  int64_t t0 = esp_timer_get_time();
  if (jpeg_decoder_process(this->dec_, &dc, this->in_, len, this->out_[back], this->out_size_, &out) == ESP_OK) {
    uint32_t us = (uint32_t) (esp_timer_get_time() - t0);
    if (us > this->st_dec_us_max_.load())
      this->st_dec_us_max_.store(us);
    this->st_dec_.fetch_add(1);
    this->frames_.fetch_add(1);
    if (direct)
      this->draw_direct_(info.width, info.height, hp);
    else
      this->ready_.store(back);
  }
}

void MjpegVideo::draw_direct_(uint32_t w, uint32_t h, uint32_t hp) {
  this->busy_.store(true);
  if (!this->want_run_.load() || this->gen_.load() != this->cur_gen_ || !this->direct_.load()) {
    this->busy_.store(false);   // stop was requested: never write over the LVGL page again
    return;
  }
  // logical (LVGL) screen size
  bool swap = this->rotation_ == 90 || this->rotation_ == 270;
  int scr_w = swap ? this->fb_h_ : this->fb_w_;
  int scr_h = swap ? this->fb_w_ : this->fb_h_;
  int rx = this->rx_, ry = this->ry_, rw = this->rw_, rh = this->rh_;
  if (rw <= 0 || rh <= 0) {
    rx = 0;
    ry = 0;
    rw = scr_w;
    rh = scr_h;
  }
  // largest PPA scale (steps of 1/16) that fits the rect; sizes computed exactly like the PPA driver does
  int s16 = std::min((int) (rw * 16 / (int) w), (int) (rh * 16 / (int) h));
  if (s16 > 15 * 16)
    s16 = 15 * 16;
  if (s16 < 1) {
    this->busy_.store(false);
    return;
  }
  uint32_t si = s16 / 16, sf = s16 % 16;
  int lw = (int) (si * w + sf * w / 16);   // size on the LVGL screen
  int lh = (int) (si * h + sf * h / 16);
  int lx = (rx + (rw - lw) / 2) & ~1;
  int ly = (ry + (rh - lh) / 2) & ~1;
  // map the logical rect to the physical panel exactly like LvglComponent::draw_buffer_ does for its rotation
  // (ESPHome rotation is clockwise, PPA rotation is counter-clockwise)
  ppa_srm_rotation_angle_t angle;
  int px, py, ow, oh;
  switch (this->rotation_) {
    case 90:
      angle = PPA_SRM_ROTATION_ANGLE_270;
      px = this->fb_w_ - ly - lh;
      py = lx;
      ow = lh;
      oh = lw;
      break;
    case 180:
      angle = PPA_SRM_ROTATION_ANGLE_180;
      px = this->fb_w_ - lx - lw;
      py = this->fb_h_ - ly - lh;
      ow = lw;
      oh = lh;
      break;
    case 270:
      angle = PPA_SRM_ROTATION_ANGLE_90;
      px = ly;
      py = this->fb_h_ - lx - lw;
      ow = lh;
      oh = lw;
      break;
    default:
      angle = PPA_SRM_ROTATION_ANGLE_0;
      px = lx;
      py = ly;
      ow = lw;
      oh = lh;
      break;
  }
  if (px < 0 || py < 0 || px + ow > this->fb_w_ || py + oh > this->fb_h_) {
    this->busy_.store(false);
    this->st_ppa_err_.fetch_add(1);
    return;
  }
  ppa_srm_oper_config_t c = {};
  c.in.buffer = this->out_[0];
  c.in.pic_w = w;
  c.in.pic_h = hp;
  c.in.block_w = w;
  c.in.block_h = h;
  c.in.block_offset_x = 0;
  c.in.block_offset_y = 0;
  c.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
  c.out.buffer = this->fb_;
  c.out.buffer_size = this->fb_size_;
  c.out.pic_w = this->fb_w_;
  c.out.pic_h = this->fb_h_;
  c.out.block_offset_x = px;
  c.out.block_offset_y = py;
  c.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
  c.rotation_angle = angle;
  c.scale_x = s16 / 16.0f;
  c.scale_y = s16 / 16.0f;
  c.mode = PPA_TRANS_MODE_BLOCKING;
  int64_t t0 = esp_timer_get_time();
  esp_err_t e = ppa_do_scale_rotate_mirror(this->ppa_, &c);
  int64_t t1 = esp_timer_get_time();
  this->busy_.store(false);
  if (e != ESP_OK) {
    if (this->st_ppa_err_.fetch_add(1) == 0)
      ESP_LOGW(TAG, "PPA: %s (in %ux%u, scale %d/16, at %d,%d)", esp_err_to_name(e), (unsigned) w, (unsigned) h, s16,
               px, py);
    return;
  }
  uint32_t us = (uint32_t) (t1 - t0);
  if (us > this->st_ppa_us_max_.load())
    this->st_ppa_us_max_.store(us);
  this->st_ppa_.fetch_add(1);
  if (this->t_dir_ != 0) {
    uint32_t gap = (uint32_t) (t1 - this->t_dir_);
    if (gap > this->st_gap_dir_.load())
      this->st_gap_dir_.store(gap);
  }
  this->t_dir_ = t1;
}

}  // namespace mjpeg_video
}  // namespace esphome
