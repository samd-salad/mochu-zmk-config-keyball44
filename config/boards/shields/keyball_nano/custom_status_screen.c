/*
 * Custom rotated status screen for Keyball44
 *
 * Renders widgets to a virtual 32x128 portrait canvas, then rotates 90°
 * onto the physical 128x32 SSD1306 OLED (mounted sideways).
 *
 * Central (right) half: full status — battery, split pair, BT, layer,
 *                       key press, WPM, caps/num lock
 * Peripheral (left) half: battery + connection status only
 */

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zephyr/drivers/display.h>
#include <zmk/display.h>
#include <zmk/battery.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/keymap.h>
#include <zmk/hid_indicators.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/split/bluetooth/central.h>
#endif

#if !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/split/bluetooth/peripheral.h>
#include <zmk/events/split_peripheral_status_changed.h>
#endif

#include <lvgl.h>

/* Virtual portrait canvas dimensions (drawn as if 32 wide x 128 tall) */
#define VIRT_W 32
#define VIRT_H 128

/* Physical display dimensions */
#define DISP_W 128
#define DISP_H 32

/* Canvas buffers */
static lv_color_t virtual_buf[VIRT_W * VIRT_H];
static lv_color_t display_buf[DISP_W * DISP_H];

static lv_obj_t *virtual_canvas;
static lv_obj_t *display_canvas;

/* ================================================================
 * Current display state
 * ================================================================ */

static uint8_t cur_battery_pct = 0;

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
/* Central-only state */
static bool cur_split_connected = false;
static bool cur_bt_connected = false;
static bool cur_bt_bonded = false;
static uint8_t cur_bt_profile = 0;
static bool cur_usb_output = false;
static const char *cur_layer_name = "";
static char cur_key_str[8] = "";
static bool cur_display_blanked = false; /* actual blanking state */
static uint8_t cur_layer_index = 0;
static int64_t conn_change_time = 0; /* timestamp of last BT connect/profile change */
#define CONN_DISPLAY_DURATION_MS 60000 /* show display for 60s after connection event */
static void conn_timer_handler(struct k_timer *timer);
K_TIMER_DEFINE(conn_display_timer, conn_timer_handler, NULL);
static bool cur_caps_lock = false;
static bool cur_num_lock = false;
#else
/* Peripheral-only state */
static bool cur_periph_connected = false;
#endif

/* ================================================================
 * HID keycode → display string (central only)
 * ================================================================ */

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static const char *hid_keycode_to_str(uint16_t usage_page, uint32_t keycode) {
    if (usage_page != 0x07) {
        return "?";
    }
    if (keycode >= 0x04 && keycode <= 0x1D) {
        static char letter[2] = {0};
        letter[0] = 'A' + (keycode - 0x04);
        return letter;
    }
    if (keycode >= 0x1E && keycode <= 0x26) {
        static char digit[2] = {0};
        digit[0] = '1' + (keycode - 0x1E);
        return digit;
    }
    switch (keycode) {
    case 0x27: return "0";
    case 0x28: return "RET";
    case 0x29: return "ESC";
    case 0x2A: return "BSPC";
    case 0x2B: return "TAB";
    case 0x2C: return "SPC";
    case 0x2D: return "-";
    case 0x2E: return "=";
    case 0x2F: return "[";
    case 0x30: return "]";
    case 0x31: return "\\";
    case 0x33: return ";";
    case 0x34: return "'";
    case 0x35: return "`";
    case 0x36: return ",";
    case 0x37: return ".";
    case 0x38: return "/";
    case 0x39: return "CAPS";
    case 0x4A: return "HOME";
    case 0x4B: return "PGUP";
    case 0x4C: return "DEL";
    case 0x4D: return "END";
    case 0x4E: return "PGDN";
    case 0x4F: return "RGHT";
    case 0x50: return "LEFT";
    case 0x51: return "DOWN";
    case 0x52: return "UP";
    case 0xE0: return "LCTL";
    case 0xE1: return "LSFT";
    case 0xE2: return "LALT";
    case 0xE3: return "LGUI";
    case 0xE4: return "RCTL";
    case 0xE5: return "RSFT";
    case 0xE6: return "RALT";
    case 0xE7: return "RGUI";
    default:   return "?";
    }
}
#endif

/* ================================================================
 * Drawing helpers
 * ================================================================ */

/* Throttle redraws to max ~5/sec to save I2C + CPU */
#define DRAW_MIN_INTERVAL_MS 200
static int64_t last_draw_time = 0;
static bool draw_pending = false;

static void draw_screen(void);

static void request_draw(void) {
    int64_t now = k_uptime_get();
    if (now - last_draw_time >= DRAW_MIN_INTERVAL_MS) {
        draw_screen();
        last_draw_time = now;
        draw_pending = false;
    } else {
        draw_pending = true;
    }
}

static void rotate_and_flush(void) {
    /*
     * 90° CW rotation: virtual(vx, vy) → display(vy, 31 - vx)
     * If the text appears upside-down on your display, change to:
     *   display_buf[vx * DISP_W + (DISP_W - 1 - vy)]
     */
    for (int vy = 0; vy < VIRT_H; vy++) {
        for (int vx = 0; vx < VIRT_W; vx++) {
            display_buf[vx * DISP_W + (DISP_W - 1 - vy)] =
                virtual_buf[vy * VIRT_W + vx];
        }
    }
    lv_obj_invalidate(display_canvas);
}

/* Reusable drawing descriptors */
static lv_draw_label_dsc_t lbl_center;
static lv_draw_label_dsc_t lbl_left;
static lv_draw_rect_dsc_t rect_black;
static lv_draw_rect_dsc_t rect_outline;
static bool dsc_inited = false;

static void update_display_blanking(void);

static void conn_timer_handler(struct k_timer *timer) {
    /* Timer expired — re-evaluate blanking (conn window has closed) */
    update_display_blanking();
}

/* Determine if display should be on:
 * - ON if on NUM layer
 * - ON if BT not connected (need to see pairing status)
 * - ON for 60s after BT connect or profile change
 * - OFF otherwise */
static bool should_display_be_on(void) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    if (cur_layer_index == 1) return true; /* NUM layer */
    if (!cur_bt_connected && !cur_usb_output) return true;
    if (conn_change_time > 0 &&
        (k_uptime_get() - conn_change_time) < CONN_DISPLAY_DURATION_MS) return true;
#endif
    return false;
}

static void update_display_blanking(void) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    const struct device *disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
    bool want_on = should_display_be_on();
    if (want_on && cur_display_blanked) {
        display_blanking_off(disp);
        cur_display_blanked = false;
    } else if (!want_on && !cur_display_blanked) {
        display_blanking_on(disp);
        cur_display_blanked = true;
    }
#endif
}

static void init_draw_dsc(void) {
    if (dsc_inited) return;
    dsc_inited = true;

    lv_draw_label_dsc_init(&lbl_center);
    lbl_center.color = lv_color_black();
    lbl_center.font = &lv_font_montserrat_10;
    lbl_center.align = LV_TEXT_ALIGN_CENTER;

    lv_draw_label_dsc_init(&lbl_left);
    lbl_left.color = lv_color_black();
    lbl_left.font = &lv_font_montserrat_10;
    lbl_left.align = LV_TEXT_ALIGN_LEFT;

    lv_draw_rect_dsc_init(&rect_black);
    rect_black.bg_color = lv_color_black();
    rect_black.bg_opa = LV_OPA_COVER;
    rect_black.radius = 0;
    rect_black.border_width = 0;

    lv_draw_rect_dsc_init(&rect_outline);
    rect_outline.bg_opa = LV_OPA_TRANSP;
    rect_outline.border_color = lv_color_black();
    rect_outline.border_width = 1;
    rect_outline.radius = 1;
}

static int draw_battery(int y) {
    /* Battery icon: centered, 22px wide × 10px tall */
    int icon_x = (VIRT_W - 24) / 2;
    lv_canvas_draw_rect(virtual_canvas, icon_x, y, 20, 10, &rect_outline);
    /* Battery nub */
    lv_canvas_draw_rect(virtual_canvas, icon_x + 20, y + 3, 3, 4, &rect_black);
    /* Battery fill proportional to percentage */
    int fill_w = 18 * cur_battery_pct / 100;
    if (fill_w > 0) {
        lv_canvas_draw_rect(virtual_canvas, icon_x + 1, y + 1, fill_w, 8, &rect_black);
    }
    y += 12;
    /* Percentage text on its own line */
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", cur_battery_pct);
    lv_canvas_draw_text(virtual_canvas, 0, y, VIRT_W, &lbl_center, buf);
    return y + 12;
}

/* ================================================================
 * Central (right half) drawing
 * ================================================================ */

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

static void draw_screen(void) {
    init_draw_dsc();
    lv_canvas_fill_bg(virtual_canvas, lv_color_white(), LV_OPA_COVER);

    int y = 4;

    /* Battery icon + percentage */
    y = draw_battery(y);
    y += 6;

    /* Split pair status */
    {
        const char *st = cur_split_connected ? "OK" : "--";
        lv_canvas_draw_text(virtual_canvas, 0, y, VIRT_W, &lbl_center, st);
        y += 14;
    }

    /* BT/USB + profile (+ x if disconnected) */
    {
        char buf[16];
        if (cur_usb_output) {
            snprintf(buf, sizeof(buf), "USB");
        } else if (cur_bt_connected) {
            snprintf(buf, sizeof(buf), "BT %d", cur_bt_profile + 1);
        } else {
            snprintf(buf, sizeof(buf), "BT %d x", cur_bt_profile + 1);
        }
        lv_canvas_draw_text(virtual_canvas, 0, y, VIRT_W, &lbl_center, buf);
        y += 14;
    }

    /* Layer name */
    {
        const char *name = (cur_layer_name && cur_layer_name[0]) ? cur_layer_name : "KBR";
        lv_canvas_draw_text(virtual_canvas, 0, y, VIRT_W, &lbl_center, name);
        y += 14;
    }

    /* Last key pressed */
    if (cur_key_str[0]) {
        lv_canvas_draw_text(virtual_canvas, 0, y, VIRT_W, &lbl_center, cur_key_str);
        y += 14;
    }

    /* Caps / Num Lock indicators (only when active) */
    if (cur_caps_lock || cur_num_lock) {
        if (cur_caps_lock && cur_num_lock) {
            lv_canvas_draw_text(virtual_canvas, 0, y, VIRT_W, &lbl_center, "CAP NUM");
        } else if (cur_caps_lock) {
            lv_canvas_draw_text(virtual_canvas, 0, y, VIRT_W, &lbl_center, "CAPS");
        } else {
            lv_canvas_draw_text(virtual_canvas, 0, y, VIRT_W, &lbl_center, "NUM");
        }
    }

    rotate_and_flush();
}

#else /* Peripheral (left half) */

static void draw_screen(void) {
    init_draw_dsc();
    lv_canvas_fill_bg(virtual_canvas, lv_color_white(), LV_OPA_COVER);

    int y = 4;

    /* Battery icon + percentage */
    y = draw_battery(y);
    y += 6;

    /* Connection to central */
    lv_canvas_draw_text(virtual_canvas, 0, y, VIRT_W, &lbl_center,
                        cur_periph_connected ? "OK" : "--");

    rotate_and_flush();
}

#endif /* CONFIG_ZMK_SPLIT_ROLE_CENTRAL */

/* ================================================================
 * Event listeners — Battery (both halves)
 * ================================================================ */

struct battery_state {
    uint8_t level;
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    bool split_connected;
#endif
};

static void battery_update_cb(struct battery_state state) {
    cur_battery_pct = state.level;
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    cur_split_connected = state.split_connected;
#endif
    request_draw();
}

static struct battery_state battery_get_state(const zmk_event_t *eh) {
    struct battery_state s = {
        .level = zmk_battery_state_of_charge(),
    };
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    uint8_t periph_lvl = 0;
    s.split_connected = (zmk_split_get_peripheral_battery_level(0, &periph_lvl) == 0);
#endif
    return s;
}

ZMK_DISPLAY_WIDGET_LISTENER(battery_listener, struct battery_state,
                            battery_update_cb, battery_get_state)
ZMK_SUBSCRIPTION(battery_listener, zmk_battery_state_changed);

/* ================================================================
 * Peripheral-only: connection listener
 * ================================================================ */

#if !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

struct periph_conn_state {
    bool connected;
};

static void periph_conn_update_cb(struct periph_conn_state state) {
    cur_periph_connected = state.connected;
    request_draw();
}

static struct periph_conn_state periph_conn_get_state(const zmk_event_t *eh) {
    return (struct periph_conn_state){
        .connected = zmk_split_bt_peripheral_is_connected(),
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(periph_conn_listener, struct periph_conn_state,
                            periph_conn_update_cb, periph_conn_get_state)
ZMK_SUBSCRIPTION(periph_conn_listener, zmk_split_peripheral_status_changed);

#endif

/* ================================================================
 * Central-only listeners
 * ================================================================ */

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

/* --- Layer (also controls display blanking) --- */
struct layer_state {
    const char *label;
    uint8_t index;
};

static void layer_update_cb(struct layer_state state) {
    cur_layer_name = state.label;
    cur_layer_index = state.index;
    update_display_blanking();
    request_draw();
}

static struct layer_state layer_get_state(const zmk_event_t *eh) {
    zmk_keymap_layer_index_t index = zmk_keymap_highest_layer_active();
    return (struct layer_state){
        .label = zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(index)),
        .index = index,
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(layer_listener, struct layer_state,
                            layer_update_cb, layer_get_state)
ZMK_SUBSCRIPTION(layer_listener, zmk_layer_state_changed);

/* --- Output / BT --- */
struct output_state {
    uint8_t profile_index;
    bool connected;
    bool bonded;
    bool usb;
};

static void output_update_cb(struct output_state state) {
    /* Detect connection or profile change — start 60s display timer */
    if (state.connected != cur_bt_connected ||
        state.profile_index != cur_bt_profile) {
        conn_change_time = k_uptime_get();
        k_timer_start(&conn_display_timer,
                      K_MSEC(CONN_DISPLAY_DURATION_MS), K_NO_WAIT);
    }
    cur_bt_profile = state.profile_index;
    cur_bt_connected = state.connected;
    cur_bt_bonded = state.bonded;
    cur_usb_output = state.usb;
    update_display_blanking();
    request_draw();
}

static struct output_state output_get_state(const zmk_event_t *eh) {
    struct zmk_endpoint_instance ep = zmk_endpoints_selected();
    return (struct output_state){
        .profile_index = zmk_ble_active_profile_index(),
        .connected = zmk_ble_active_profile_is_connected(),
        .bonded = !zmk_ble_active_profile_is_open(),
        .usb = (ep.transport == ZMK_TRANSPORT_USB),
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(output_listener, struct output_state,
                            output_update_cb, output_get_state)
ZMK_SUBSCRIPTION(output_listener, zmk_ble_active_profile_changed);
ZMK_SUBSCRIPTION(output_listener, zmk_endpoint_changed);

/* --- Keycode --- */
struct keycode_state {
    uint16_t usage_page;
    uint32_t keycode;
    bool pressed;
};

static void keycode_update_cb(struct keycode_state state) {
    if (state.pressed) {
        const char *name = hid_keycode_to_str(state.usage_page, state.keycode);
        strncpy(cur_key_str, name, sizeof(cur_key_str) - 1);
        cur_key_str[sizeof(cur_key_str) - 1] = '\0';
        request_draw();
    }
}

static struct keycode_state keycode_get_state(const zmk_event_t *eh) {
    if (!eh) {
        return (struct keycode_state){0};
    }
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev) {
        return (struct keycode_state){
            .usage_page = ev->usage_page,
            .keycode = ev->keycode,
            .pressed = ev->state,
        };
    }
    return (struct keycode_state){0};
}

ZMK_DISPLAY_WIDGET_LISTENER(keycode_listener, struct keycode_state,
                            keycode_update_cb, keycode_get_state)
ZMK_SUBSCRIPTION(keycode_listener, zmk_keycode_state_changed);

/* --- HID Indicators (Caps Lock, Num Lock) --- */
struct indicator_state {
    bool caps;
    bool num;
};

static void indicator_update_cb(struct indicator_state state) {
    cur_caps_lock = state.caps;
    cur_num_lock = state.num;
    request_draw();
}

static struct indicator_state indicator_get_state(const zmk_event_t *eh) {
    zmk_hid_indicators_t ind = zmk_hid_indicators_get_current_profile();
    return (struct indicator_state){
        .caps = (ind & 0x02) != 0,  /* bit 1 = Caps Lock */
        .num = (ind & 0x01) != 0,   /* bit 0 = Num Lock */
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(indicator_listener, struct indicator_state,
                            indicator_update_cb, indicator_get_state)
ZMK_SUBSCRIPTION(indicator_listener, zmk_hid_indicators_changed);

#endif /* CONFIG_ZMK_SPLIT_ROLE_CENTRAL */

/* ================================================================
 * Screen entry point
 * ================================================================ */

lv_obj_t *zmk_display_status_screen(void) {
    lv_obj_t *screen = lv_obj_create(NULL);

    /* Hidden virtual canvas for portrait rendering */
    virtual_canvas = lv_canvas_create(screen);
    lv_canvas_set_buffer(virtual_canvas, virtual_buf, VIRT_W, VIRT_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_add_flag(virtual_canvas, LV_OBJ_FLAG_HIDDEN);

    /* Visible display canvas */
    display_canvas = lv_canvas_create(screen);
    lv_canvas_set_buffer(display_canvas, display_buf, DISP_W, DISP_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_align(display_canvas, LV_ALIGN_TOP_LEFT, 0, 0);

    /* Init listeners — triggers first draw */
    battery_listener_init();

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    layer_listener_init();
    output_listener_init();
    keycode_listener_init();
    indicator_listener_init();

    /* Show display for first 60s after boot (connection settling) */
    conn_change_time = k_uptime_get();
    k_timer_start(&conn_display_timer,
                  K_MSEC(CONN_DISPLAY_DURATION_MS), K_NO_WAIT);
#else
    periph_conn_listener_init();
#endif

    return screen;
}
