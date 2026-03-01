/*
 * Copyright 2025 sekigon-gonnoc
 * SPDX-License-Identifier: GPL-2.0 or later
 */

#include <stdint.h>
#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/device_runtime.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(lpps, CONFIG_ZMK_LOG_LEVEL);

#define DT_DRV_COMPAT lpps

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct lpps_config {
    struct i2c_dt_spec i2c;
    struct gpio_dt_spec irq_gpio;
    struct gpio_dt_spec power_gpio;
};

struct lpps_data {
    const struct device *dev;
    struct k_work motion_work;
    struct k_work_delayable irq_reenable_work;
    struct gpio_callback motion_cb;
    atomic_t behavior_calibration_pending;
};

static int lpps_i2c_read_reg(const struct device *dev, uint8_t reg, uint8_t *data, uint8_t len) {
    const struct lpps_config *cfg = dev->config;
    // return i2c_burst_read_dt(&cfg->i2c, reg, data, len);
    // i2c_write_dt(&cfg->i2c, &reg, 1);
    return i2c_read_dt(&cfg->i2c, data, len);
}

static int lpps_i2c_write_reg(const struct device *dev, uint8_t reg, const uint8_t *data,
                              uint8_t len) {
    const struct lpps_config *cfg = dev->config;
    return i2c_burst_write_dt(&cfg->i2c, reg, data, len);
}

static int lpps_interrupt_configure(const struct device *dev, gpio_flags_t flags) {
    const struct lpps_config *cfg = dev->config;

    if (!gpio_is_ready_dt(&cfg->irq_gpio)) {
        return -ENODEV;
    }

    return gpio_pin_interrupt_configure_dt(&cfg->irq_gpio, flags);
}

static void lpps_interrupt_enable(const struct device *dev) {
    int ret = lpps_interrupt_configure(dev, GPIO_INT_LEVEL_LOW);

    if (ret < 0) {
        LOG_ERR("Failed to enable IRQ interrupt: %d", ret);
    }
}

static void lpps_interrupt_disable(const struct device *dev) {
    int ret = lpps_interrupt_configure(dev, GPIO_INT_DISABLE);

    if (ret < 0) {
        LOG_WRN("Failed to disable IRQ interrupt: %d", ret);
    }
}

static bool lpps_irq_is_active(const struct device *dev) {
    const struct lpps_config *cfg = dev->config;

    return gpio_pin_get_dt(&cfg->irq_gpio) == 0;
}

static void lpps_irq_reenable_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct lpps_data *data = CONTAINER_OF(dwork, struct lpps_data, irq_reenable_work);
    const struct device *dev = data->dev;

    if (lpps_irq_is_active(dev)) {
        k_work_reschedule(&data->irq_reenable_work, K_MSEC(1));
        return;
    }

    lpps_interrupt_enable(dev);
}

static void lpps_interrupt_enable_when_inactive(struct lpps_data *data) {
    const struct device *dev = data->dev;

    if (lpps_irq_is_active(dev)) {
        k_work_reschedule(&data->irq_reenable_work, K_MSEC(1));
        return;
    }

    lpps_interrupt_enable(dev);
}

static bool lpps_is_ready(const struct device *dev) {
    const struct lpps_config *cfg = dev->config;

    if (!gpio_is_ready_dt(&cfg->irq_gpio)) {
        return true; // Assume ready if no IRQ pin configured
    }

    // RDY pin is active LOW, so device is ready when pin is LOW
    return !gpio_pin_get_dt(&cfg->irq_gpio);
}

static void lpps_motion_work_handler(struct k_work *work) {
    struct lpps_data *data = CONTAINER_OF(work, struct lpps_data, motion_work);
    const struct device *dev = data->dev;
    int ret;
    int64_t current_time = k_uptime_get();

    // Only read data if device is ready
    if (!lpps_is_ready(dev)) {
        LOG_WRN("Device not ready for motion data");
        lpps_interrupt_enable_when_inactive(data);
        return;
    }

    LOG_DBG("Motion detected at %lld ms", current_time);

    /* If behavior requested a write, perform it here (on the device workqueue) */
    if (atomic_set(&data->behavior_calibration_pending, 0)) {
        uint8_t reg = 0x04;
        uint8_t wdata[1] = {1};
        ret = lpps_i2c_write_reg(dev, reg, wdata, sizeof(wdata));
        if (ret < 0) {
            LOG_ERR("Behavior-requested I2C write failed: %d", ret);
        } else {
            LOG_DBG("Behavior-requested I2C write succeeded");
        }
    }

    uint8_t transfer_bytes[3];
    const struct lpps_config *cfg = dev->config;
    ret = i2c_read_dt(&cfg->i2c, transfer_bytes, sizeof(transfer_bytes));
    LOG_DBG("I2C read: data=%02X %02X %02X", transfer_bytes[0], transfer_bytes[1],
            transfer_bytes[2]);
    int8_t x = (int8_t)transfer_bytes[0];
    int8_t y = (int8_t)transfer_bytes[1];
    int8_t z = (int8_t)transfer_bytes[2];

    if (ret < 0) {
        LOG_ERR("I2C read failed: %d", ret);
        lpps_interrupt_enable_when_inactive(data);
        return;
    }

    // close i2c bus of module
    ret = lpps_i2c_read_reg(dev, 0, transfer_bytes, 1);

    LOG_DBG("Movement: x=%4d y=%4d z=%4d", x, y, z);
    input_report_rel(dev, INPUT_REL_X, x, false, K_FOREVER);
    input_report_rel(dev, INPUT_REL_Y, y, true, K_FOREVER);

    lpps_interrupt_enable_when_inactive(data);
}

/* Public API: request that the driver perform the write on its work handler */
int lpps_request_write(const struct device *dev) {
    if (!dev) {
        return -ENODEV;
    }

    struct lpps_data *data = dev->data;
    if (!data) {
        return -ENODEV;
    }

    atomic_set(&data->behavior_calibration_pending, 1);
    return 0;
}

static void lpps_motion_handler(const struct device *gpio_dev, struct gpio_callback *cb,
                                uint32_t pins) {
    struct lpps_data *data = CONTAINER_OF(cb, struct lpps_data, motion_cb);

    ARG_UNUSED(gpio_dev);
    ARG_UNUSED(pins);

    k_work_cancel_delayable(&data->irq_reenable_work);
    lpps_interrupt_disable(data->dev);
    k_work_submit(&data->motion_work);
}

static int lpps_init(const struct device *dev) {
    const struct lpps_config *cfg = dev->config;
    struct lpps_data *data = dev->data;
    int ret;

    if (!device_is_ready(cfg->i2c.bus)) {
        LOG_ERR("I2C bus %s is not ready", cfg->i2c.bus->name);
        return -ENODEV;
    }

    data->dev = dev;

    k_work_init(&data->motion_work, lpps_motion_work_handler);
    k_work_init_delayable(&data->irq_reenable_work, lpps_irq_reenable_work_handler);
    atomic_set(&data->behavior_calibration_pending, 0);

#if DT_INST_NODE_HAS_PROP(0, power_gpios)
    if (gpio_is_ready_dt(&cfg->power_gpio)) {
        ret = gpio_pin_configure_dt(&cfg->power_gpio, GPIO_OUTPUT_INACTIVE);
        if (ret != 0) {
            LOG_ERR("Power pin configuration failed: %d", ret);
            return ret;
        }

        k_sleep(K_MSEC(500));

        ret = gpio_pin_set_dt(&cfg->power_gpio, 1);
        if (ret != 0) {
            LOG_ERR("Power pin set failed: %d", ret);
            return ret;
        }

        k_sleep(K_MSEC(10));
    }
#endif

    if (gpio_is_ready_dt(&cfg->irq_gpio)) {
        ret = gpio_pin_configure_dt(&cfg->irq_gpio, GPIO_INPUT);
        if (ret != 0) {
            LOG_ERR("IRQ pin configuration failed: %d", ret);
            return ret;
        }

        gpio_init_callback(&data->motion_cb, lpps_motion_handler, BIT(cfg->irq_gpio.pin));

        ret = gpio_add_callback_dt(&cfg->irq_gpio, &data->motion_cb);
        if (ret < 0) {
            LOG_ERR("Could not set motion callback: %d", ret);
            return ret;
        }

        LOG_DBG("IRQ pin configured, pin: %d", cfg->irq_gpio.pin);
    }

    if (gpio_is_ready_dt(&cfg->irq_gpio)) {
        ret = lpps_interrupt_configure(dev, GPIO_INT_LEVEL_LOW);
        if (ret != 0) {
            LOG_ERR("Motion interrupt configuration failed: %d", ret);
            return ret;
        }
    }

    ret = pm_device_runtime_enable(dev);
    if (ret < 0) {
        LOG_ERR("Failed to enable runtime power management: %d", ret);
        return ret;
    }

    return 0;
}

#ifdef CONFIG_PM_DEVICE
static int lpps_pm_action(const struct device *dev, enum pm_device_action action) {
    const struct lpps_config *cfg = dev->config;
    struct lpps_data *data = dev->data;
    int ret;

    switch (action) {
    case PM_DEVICE_ACTION_SUSPEND:
        k_work_cancel_delayable(&data->irq_reenable_work);
        if (gpio_is_ready_dt(&cfg->irq_gpio)) {
            ret = lpps_interrupt_configure(dev, GPIO_INT_DISABLE);
            if (ret < 0) {
                LOG_ERR("Failed to disable IRQ interrupt: %d", ret);
                return ret;
            }

            ret = gpio_pin_configure(cfg->irq_gpio.port, cfg->irq_gpio.pin,
                                     GPIO_INPUT | GPIO_PULL_DOWN);
            if (ret < 0) {
                LOG_ERR("Failed to disconnect IRQ GPIO: %d", ret);
                return ret;
            }
        }

#if DT_INST_NODE_HAS_PROP(0, power_gpios)
        if (gpio_is_ready_dt(&cfg->power_gpio)) {
            ret = gpio_pin_configure_dt(&cfg->power_gpio, GPIO_DISCONNECTED);
            if (ret < 0) {
                LOG_ERR("Failed to disconnect power: %d", ret);
                return ret;
            }
        }
#endif
        break;

    case PM_DEVICE_ACTION_RESUME:
#if DT_INST_NODE_HAS_PROP(0, power_gpios)
        if (gpio_is_ready_dt(&cfg->power_gpio)) {
            ret = gpio_pin_configure_dt(&cfg->power_gpio, GPIO_OUTPUT_ACTIVE);
            if (ret < 0) {
                LOG_ERR("Failed to enable power: %d", ret);
                return ret;
            }
            k_sleep(K_MSEC(10));
        }
#endif

        if (gpio_is_ready_dt(&cfg->irq_gpio)) {
            ret = gpio_pin_configure_dt(&cfg->irq_gpio, GPIO_INPUT);
            if (ret < 0) {
                LOG_ERR("Failed to configure IRQ GPIO: %d", ret);
                return ret;
            }

            ret = lpps_interrupt_configure(dev, GPIO_INT_LEVEL_LOW);
            if (ret < 0) {
                LOG_ERR("Failed to enable IRQ interrupt: %d", ret);
                return ret;
            }
        }

        break;

    default:
        return -ENOTSUP;
    }

    return 0;
}
#endif

#define LPPS_INIT(n)                                                                               \
    static const struct lpps_config lpps_cfg_##n = {                                               \
        .i2c = I2C_DT_SPEC_INST_GET(n),                                                            \
        .irq_gpio = GPIO_DT_SPEC_INST_GET(n, irq_gpios),                                           \
        .power_gpio = GPIO_DT_SPEC_INST_GET_OR(n, power_gpios, {0}),                               \
    };                                                                                             \
                                                                                                   \
    static struct lpps_data lpps_data_##n;                                                         \
                                                                                                   \
    PM_DEVICE_DT_INST_DEFINE(n, lpps_pm_action);                                                   \
                                                                                                   \
    DEVICE_DT_INST_DEFINE(n, lpps_init, PM_DEVICE_DT_INST_GET(n), &lpps_data_##n, &lpps_cfg_##n,   \
                          POST_KERNEL, CONFIG_INPUT_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(LPPS_INIT)

#endif // DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)