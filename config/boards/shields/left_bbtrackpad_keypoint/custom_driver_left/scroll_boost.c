/*
 * Scroll boost input processor.
 *
 * Scales relative wheel input (INPUT_REL_WHEEL / INPUT_REL_HWHEEL) by
 * `boost-factor` while any key position listed in `boost-positions` is
 * held AND the `active-layer` is currently active. Used so that holding
 * the left layer-2 key during scroll mode doubles the scroll speed, but
 * only while the MOUSE layer is up (i.e. during real scrolling);
 * releasing the key returns to normal.
 *
 * Position events are seen on the split central for keys of BOTH halves,
 * so a single instance can watch positions from either side. Non-wheel
 * events pass through untouched.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_input_processor_scroll_boost

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <drivers/input_processor.h>
#include <zephyr/logging/log.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/keymap.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct scroll_boost_config {
    uint8_t boost_factor;
    int16_t active_layer;
    const uint16_t *boost_positions;
    size_t num_positions;
};

struct scroll_boost_data {
    atomic_t boost_keys_down;
};

static bool is_boost_position(const struct scroll_boost_config *cfg, uint32_t position) {
    for (size_t i = 0; i < cfg->num_positions; i++) {
        if (cfg->boost_positions[i] == position) {
            return true;
        }
    }
    return false;
}

static int scroll_boost_handle_event(const struct device *dev, struct input_event *event,
                                     uint32_t param1, uint32_t param2,
                                     struct zmk_input_processor_state *state) {
    if (event->type != INPUT_EV_REL ||
        (event->code != INPUT_REL_WHEEL && event->code != INPUT_REL_HWHEEL)) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    struct scroll_boost_data *data = (struct scroll_boost_data *)dev->data;
    if (atomic_get(&data->boost_keys_down) <= 0) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    const struct scroll_boost_config *cfg = dev->config;

    if (cfg->active_layer >= 0 &&
        !zmk_keymap_layer_active(
            zmk_keymap_layer_index_to_id((zmk_keymap_layer_index_t)cfg->active_layer))) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    event->value = (int16_t)CLAMP((int32_t)event->value * cfg->boost_factor, INT16_MIN, INT16_MAX);
    LOG_DBG("Scroll boost x%d: %d", cfg->boost_factor, event->value);

    return ZMK_INPUT_PROC_CONTINUE;
}

#define SCROLL_BOOST_POSITION_HANDLER(n)                                                           \
    {                                                                                              \
        const struct device *dev = DEVICE_DT_INST_GET(n);                                          \
        const struct scroll_boost_config *cfg = dev->config;                                       \
        struct scroll_boost_data *data = dev->data;                                                \
        if (is_boost_position(cfg, ev->position)) {                                                \
            if (ev->state) {                                                                       \
                atomic_inc(&data->boost_keys_down);                                                \
                LOG_DBG("Scroll boost key %d pressed (%ld down)", (int)ev->position,               \
                        (long)atomic_get(&data->boost_keys_down));                                 \
            } else if (atomic_get(&data->boost_keys_down) > 0) {                                   \
                atomic_dec(&data->boost_keys_down);                                                \
                LOG_DBG("Scroll boost key %d released (%ld down)", (int)ev->position,              \
                        (long)atomic_get(&data->boost_keys_down));                                 \
            }                                                                                      \
        }                                                                                          \
    }

static int scroll_boost_position_dispatcher(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    DT_INST_FOREACH_STATUS_OKAY(SCROLL_BOOST_POSITION_HANDLER)

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(scroll_boost_position_listener, scroll_boost_position_dispatcher);
ZMK_SUBSCRIPTION(scroll_boost_position_listener, zmk_position_state_changed);

static const struct zmk_input_processor_driver_api scroll_boost_driver_api = {
    .handle_event = scroll_boost_handle_event,
};

#define SCROLL_BOOST_INST(n)                                                                       \
    static struct scroll_boost_data scroll_boost_data_##n;                                         \
    static const uint16_t scroll_boost_positions_##n[] = DT_INST_PROP(n, boost_positions);         \
    static const struct scroll_boost_config scroll_boost_config_##n = {                            \
        .boost_factor = DT_INST_PROP(n, boost_factor),                                             \
        .active_layer = DT_INST_PROP_OR(n, active_layer, -1),                                      \
        .boost_positions = scroll_boost_positions_##n,                                             \
        .num_positions = DT_INST_PROP_LEN(n, boost_positions),                                     \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL, &scroll_boost_data_##n, &scroll_boost_config_##n,         \
                          POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                        \
                          &scroll_boost_driver_api);

DT_INST_FOREACH_STATUS_OKAY(SCROLL_BOOST_INST)
