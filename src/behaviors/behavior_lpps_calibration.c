
#define DT_DRV_COMPAT zmk_behavior_lpps_calibration

// Dependencies
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <drivers/behavior.h>
#include <zephyr/drivers/i2c.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/behavior.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_lpps_binding_pressed(struct zmk_behavior_binding *binding,
                                   struct zmk_behavior_binding_event event) {
    const struct device *dev = DEVICE_DT_GET(DT_INST(0, lpps));
    if (!device_is_ready(dev)) {
        LOG_ERR("LPPS device not ready");
        return ZMK_BEHAVIOR_OPAQUE;
    }

    extern int lpps_request_write(const struct device *dev);
    int ret = lpps_request_write(dev);
    if (ret < 0) {
        LOG_ERR("Failed to request LPPS write: %d", ret);
    } else {
        LOG_DBG("Requested LPPS write via motion work handler");
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_lpps_binding_released(struct zmk_behavior_binding *binding,
                                    struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

// API struct
static const struct behavior_driver_api lpps_driver_api = {
    .binding_pressed = on_lpps_binding_pressed,
    .binding_released = on_lpps_binding_released,
};

BEHAVIOR_DT_INST_DEFINE(
    0, // Instance Number (0)
    NULL,
    NULL, // Power Management Device Pointer
    NULL, NULL, POST_KERNEL,
    CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, // Initialization Level, Device Priority
    &lpps_driver_api);                   // API struct

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
