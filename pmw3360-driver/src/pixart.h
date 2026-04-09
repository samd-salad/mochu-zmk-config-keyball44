#pragma once

/**
 * @file pixart.h
 *
 * @brief Common header file for all optical motion sensor by PIXART
 */

#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>

#ifdef __cplusplus
extern "C" {
#endif

enum pixart_input_mode { MOVE = 0, SCROLL, SNIPE };

/* adaptive polling states */
enum poll_state {
    POLL_ACTIVE,  /* motion detected recently — full rate */
    POLL_IDLE,    /* no motion for a while — reduced rate */
    POLL_SLEEP,   /* no motion for a long time — minimal rate */
};

#define POLL_ACTIVE_MS  16   /* ~60 Hz */
#define POLL_IDLE_MS    100  /* 10 Hz */
#define POLL_SLEEP_MS   250  /* 4 Hz — lower wake latency for scroll */

#define POLL_IDLE_THRESHOLD   20  /* empty polls before ACTIVE→IDLE (~500ms) */
#define POLL_SLEEP_THRESHOLD  30  /* empty polls before IDLE→SLEEP (~3s) */

/* device data structure */
struct pixart_data {
    const struct device *dev;

    enum pixart_input_mode curr_mode;
    uint32_t curr_cpi;
    int32_t scroll_delta_x;
    int32_t scroll_delta_y;

#ifdef CONFIG_PMW3610_POLLING_RATE_125_SW
    int64_t last_poll_time;
    int16_t last_x;
    int16_t last_y;
#endif

    // motion interrupt isr
    struct gpio_callback irq_gpio_cb;
    // the work structure holding the trigger job
    struct k_work trigger_work;

    // polling timer (when no IRQ pin)
    struct k_timer poll_timer;
    struct k_work poll_work;

    // adaptive polling
    enum poll_state poll_state;
    uint32_t no_motion_count;

    // the work structure for delayable init steps
    struct k_work_delayable init_work;
    int async_init_step;

    //
    bool ready;           // whether init is finished successfully
    bool last_read_burst; // todo: needed?
    int err;              // error code during async init

    // for pmw3610 smart algorithm
    bool sw_smart_flag;
};

// device config data structure
struct pixart_config {
    struct gpio_dt_spec irq_gpio;
    struct spi_dt_spec bus;
    struct gpio_dt_spec cs_gpio;
    size_t scroll_layers_len;
    int32_t *scroll_layers;
    size_t snipe_layers_len;
    int32_t *snipe_layers;
};

/** Force the sensor into POLL_ACTIVE state immediately (e.g., on layer change). */
void pmw3360_force_wake(const struct device *dev);

#ifdef __cplusplus
}
#endif

/**
 * @}
 */
