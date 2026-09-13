/*
 * Pointer boost input processor.
 *
 * Two pointer behaviors, both active while a key position listed in
 * `boost-positions` is held AND the `active-layer` is currently active:
 *
 *  - Wheel: INPUT_REL_WHEEL / INPUT_REL_HWHEEL values are multiplied by
 *    `boost-factor` (scroll speed boost).
 *  - Movement: INPUT_REL_X / INPUT_REL_Y values are divided by
 *    `move-divisor` with a fractional remainder carried across events,
 *    then capped at +/- `move-max-step` pixels per event. The cap turns
 *    hard pushes into a fixed, slow movement rate, which makes precise
 *    cursor positioning near a target easy.
 *
 * Position events are seen on the split central for keys of BOTH halves,
 * so a single instance can watch positions from either side. Non-pointer
 * events pass through untouched.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_input_processor_pointer_boost

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

struct pointer_boost_config {
    uint8_t boost_factor;
    uint8_t move_divisor;
    uint8_t move_max_step;
    int16_t active_layer;
    const uint16_t *boost_positions;
    size_t num_positions;
};

struct pointer_boost_data {
    atomic_t boost_keys_down;
    /* Fractional movement left over from the divisor, per axis. Only the
     * input thread touches these, so no locking is needed. */
    int32_t move_residual_x;
    int32_t move_residual_y;
};

static bool is_boost_position(const struct pointer_boost_config *cfg, uint32_t position) {
    for (size_t i = 0; i < cfg->num_positions; i++) {
        if (cfg->boost_positions[i] == position) {
            return true;
        }
    }
    return false;
}

static bool boost_allowed(const struct device *dev) {
    const struct pointer_boost_config *cfg = dev->config;
    struct pointer_boost_data *data = dev->data;

    if (atomic_get(&data->boost_keys_down) <= 0) {
        return false;
    }

    if (cfg->active_layer >= 0 &&
        !zmk_keymap_layer_active(
            zmk_keymap_layer_index_to_id((zmk_keymap_layer_index_t)cfg->active_layer))) {
        return false;
    }

    return true;
}

static inline void apply_move_precision(struct pointer_boost_data *data,
                                        const struct pointer_boost_config *cfg,
                                        struct input_event *event) {
    if (cfg->move_divisor <= 1) {
        return;
    }

    int32_t *residual =
        (event->code == INPUT_REL_X) ? &data->move_residual_x : &data->move_residual_y;

    *residual += event->value;
    int32_t scaled = *residual / (int32_t)cfg->move_divisor;
    *residual -= scaled * (int32_t)cfg->move_divisor;

    if (cfg->move_max_step > 0) {
        /* Drop the excess instead of banking it: hard pushes then move at a
         * constant, slow rate instead of bursting later. */
        scaled = CLAMP(scaled, -(int32_t)cfg->move_max_step, (int32_t)cfg->move_max_step);
    }

    event->value = (int16_t)scaled;
}

static int pointer_boost_handle_event(const struct device *dev, struct input_event *event,
                                      uint32_t param1, uint32_t param2,
                                      struct zmk_input_processor_state *state) {
    if (event->type != INPUT_EV_REL) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    bool is_wheel = (event->code == INPUT_REL_WHEEL || event->code == INPUT_REL_HWHEEL);
    bool is_move = (event->code == INPUT_REL_X || event->code == INPUT_REL_Y);
    if (!is_wheel && !is_move) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    if (!boost_allowed(dev)) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    const struct pointer_boost_config *cfg = dev->config;
    struct pointer_boost_data *data = (struct pointer_boost_data *)dev->data;

    if (is_wheel) {
        if (cfg->boost_factor <= 1) {
            return ZMK_INPUT_PROC_CONTINUE;
        }
        event->value =
            (int16_t)CLAMP((int32_t)event->value * cfg->boost_factor, INT16_MIN, INT16_MAX);
        LOG_DBG("Scroll boost x%d: %d", cfg->boost_factor, event->value);
    } else {
        apply_move_precision(data, cfg, event);
    }

    return ZMK_INPUT_PROC_CONTINUE;
}

#define POINTER_BOOST_POSITION_HANDLER(n)                                                          \
    {                                                                                              \
        const struct device *dev = DEVICE_DT_INST_GET(n);                                          \
        const struct pointer_boost_config *cfg = dev->config;                                      \
        struct pointer_boost_data *data = dev->data;                                               \
        if (is_boost_position(cfg, ev->position)) {                                                \
            if (ev->state) {                                                                       \
                atomic_inc(&data->boost_keys_down);                                                \
                LOG_DBG("Pointer boost key %d pressed (%ld down)", (int)ev->position,              \
                        (long)atomic_get(&data->boost_keys_down));                                 \
            } else if (atomic_get(&data->boost_keys_down) > 0) {                                   \
                atomic_dec(&data->boost_keys_down);                                                \
                LOG_DBG("Pointer boost key %d released (%ld down)", (int)ev->position,             \
                        (long)atomic_get(&data->boost_keys_down));                                 \
            }                                                                                      \
        }                                                                                          \
    }

static int pointer_boost_position_dispatcher(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    DT_INST_FOREACH_STATUS_OKAY(POINTER_BOOST_POSITION_HANDLER)

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(pointer_boost_position_listener, pointer_boost_position_dispatcher);
ZMK_SUBSCRIPTION(pointer_boost_position_listener, zmk_position_state_changed);

static const struct zmk_input_processor_driver_api pointer_boost_driver_api = {
    .handle_event = pointer_boost_handle_event,
};

#define POINTER_BOOST_INST(n)                                                                      \
    static struct pointer_boost_data pointer_boost_data_##n;                                       \
    static const uint16_t pointer_boost_positions_##n[] = DT_INST_PROP(n, boost_positions);        \
    static const struct pointer_boost_config pointer_boost_config_##n = {                          \
        .boost_factor = DT_INST_PROP(n, boost_factor),                                             \
        .move_divisor = DT_INST_PROP_OR(n, move_divisor, 1),                                       \
        .move_max_step = DT_INST_PROP_OR(n, move_max_step, 0),                                     \
        .active_layer = DT_INST_PROP_OR(n, active_layer, -1),                                      \
        .boost_positions = pointer_boost_positions_##n,                                            \
        .num_positions = DT_INST_PROP_LEN(n, boost_positions),                                     \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL, &pointer_boost_data_##n, &pointer_boost_config_##n,       \
                          POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                        \
                          &pointer_boost_driver_api);

DT_INST_FOREACH_STATUS_OKAY(POINTER_BOOST_INST)
