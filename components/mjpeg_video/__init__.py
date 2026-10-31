# mjpeg_video — smooth MJPEG-over-HTTP video on ESP32-P4 (ESPHome external component).
# Hardware JPEG decoder + PPA scale/rotate written straight into the MIPI-DSI frame buffer (bypasses LVGL for video).
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import display
from esphome.const import CONF_DISPLAY, CONF_HEIGHT, CONF_ID, CONF_ROTATION, CONF_WIDTH

DEPENDENCIES = ["esp32", "lvgl"]
CONF_RGB_BGR = "rgb_bgr"

mjpeg_ns = cg.esphome_ns.namespace("mjpeg_video")
MjpegVideo = mjpeg_ns.class_("MjpegVideo", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(MjpegVideo),
        # largest frame accepted (buffers are allocated for it); frame width must be a multiple of 16
        cv.Optional(CONF_WIDTH, default=640): cv.int_range(min=16, max=1920),
        cv.Optional(CONF_HEIGHT, default=360): cv.int_range(min=16, max=1088),
        cv.Optional(CONF_RGB_BGR, default=True): cv.boolean,
        # the mipi_dsi display: enables the direct path (PPA → frame buffer). Without it, an LVGL image is updated.
        cv.Optional(CONF_DISPLAY): cv.use_id(display.Display),
        # must match `lvgl: rotation:` (clockwise degrees)
        cv.Optional(CONF_ROTATION, default=0): cv.one_of(0, 90, 180, 270, int=True),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    from esphome.components.esp32 import include_builtin_idf_component

    include_builtin_idf_component("esp_driver_jpeg")
    include_builtin_idf_component("esp_http_client")
    include_builtin_idf_component("esp_driver_ppa")
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_size(config[CONF_WIDTH], config[CONF_HEIGHT]))
    cg.add(var.set_bgr(config[CONF_RGB_BGR]))
    cg.add(var.set_rotation(config[CONF_ROTATION]))
    if CONF_DISPLAY in config:
        cg.add(var.set_display(await cg.get_variable(config[CONF_DISPLAY])))
