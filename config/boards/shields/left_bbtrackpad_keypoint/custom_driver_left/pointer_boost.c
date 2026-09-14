/*
 * Pointer boost input processor.
 *
 * Two pointer behaviors, active while a key position listed in the
 * matching trigger list is held AND the `active-layer` is currently
 * active:
 *
 *  - Wheel: INPUT_REL_WHEEL / INPUT_REL_HWHEEL values are multiplied by
 *    `boost-factor` while a `boost-positions` key is held (scroll speed
 *    boost).
 *  - Movement: INPUT_REL_X / INPUT_REL_Y values are divided by
 *    `move-divisor` with a fractional remainder carried across events,
 *    then capped at +/- `move-max-step` pixels per event, while a
 *    `precision-positions` key is held. The cap turns hard pushes into a
 *    fixed, slow movement rate for precise positioning.
 *
 * Release mute: when the last precision key is released while the
 * pointer is still moving, movement output is muted for
 * `release-mute-ms`. This keeps the cursor from jumping away at full
 * speed when the precision key is released over the target (e.g. to
 * free a finger for clicking). Once the window expires, movement is
 * emitted again automatically if the pointing device is still being
 * pushed; pressing a precision key again cancels the mute immediately.
 *
 * Wheel and movement have separate trigger lists on purpose: a key can
 * slow the cursor without also boosting scroll speed.
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
    uint16_t release_mute_ms;
    int16_t active_layer;
    const uint16_t *boost_positions;
    size_t num_boost_positions;
    const uint16_t *precision_positions;
    size_t num_precision_positions;
};

struct pointer_boost_data {
    atomic_t boost_keys_down;
    atomic_t precision_keys_down;
    /* Release mute deadline (0 = not muted). Set when the last precision
     * key is released during movement; movement events are suppressed
     * until this timestamp passes. */
    atomic_t mute_until_ms;
    uint32_t last_move_ms;
    /* Fractional movement left over from the divisor, per axis. Only the
     * input thread touches these, so no locking is needed. */
    int32_t move_residual_x;
    int32_t move_residual_y;
};

static bool is_listed(const uint16_t *positions, size_t num_positions, uint32_t position) {
    for (size_t i = 0; i < num_positions; i++) {
        if (positions[i] == position) {
            return true;
        }
    }
    return false;
}

static bool layer_gate_open(const struct pointer_boost_config *cfg) {
    if (cfg->active_layer < 0) {
        return true;
    }
    return zmk_keymap_layer_active(
        zmk_keymap_layer_index_to_id((zmk_keymap_layer_index_t)cfg->active_layer));
}

static bool trigger_allowed(const struct pointer_boost_config *cfg, atomic_t *keys_down) {
    if (atomic_get(keys_down) <= 0) {
        return false;
    }
    return layer_gate_open(cfg);
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

    const struct pointer_boost_config *cfg = dev->config;
    struct pointer_boost_data *data = (struct pointer_boost_data *)dev->data;

    switch (event->code) {
    case INPUT_REL_WHEEL:
    case INPUT_REL_HWHEEL:
        if (cfg->boost_factor <= 1 || !trigger_allowed(cfg, &data->boost_keys_down)) {
            break;
        }
        event->value =
            (int16_t)CLAMP((int32_t)event->value * cfg->boost_factor, INT16_MIN, INT16_MAX);
        LOG_DBG("Scroll boost x%d: %d", cfg->boost_factor, event->value);
        break;
    case INPUT_REL_X:
    case INPUT_REL_Y: {
        uint32_t now = k_uptime_get_32();
        data->last_move_ms = now;

        uint32_t mute_until = (uint32_t)atomic_get(&data->mute_until_ms);
        if (mute_until != 0) {
            if ((int32_t)(now - mute_until) < 0) {
                /* Shortly after the precision key was released: freeze the
                 * pointer instead of letting it jump at full speed. */
                event->value = 0;
                break;
            }
            atomic_set(&data->mute_until_ms, 0);
            LOG_DBG("Release mute expired, movement resumes");
        }

        if (trigger_allowed(cfg, &data->precision_keys_down)) {
            apply_move_precision(data, cfg, event);
        }
        break;
    }
    default:
        break;
    }

    return ZMK_INPUT_PROC_CONTINUE;
}

#define POINTER_BOOST_POSITION_HANDLER(n)                                                          \
    {                                                                                              \
        const struct device *dev = DEVICE_DT_INST_GET(n);                                          \
        const struct pointer_boost_config *cfg = dev->config;                                      \
        struct pointer_boost_data *data = dev->data;                                               \
        if (is_listed(cfg->boost_positions, cfg->num_boost_positions, ev->position)) {             \
            if (ev->state) {                                                                       \
                atomic_inc(&data->boost_keys_down);                                                \
                LOG_DBG("Wheel boost key %d pressed (%ld down)", (int)ev->position,                \
                        (long)atomic_get(&data->boost_keys_down));                                 \
            } else if (atomic_get(&data->boost_keys_down) > 0) {                                   \
                atomic_dec(&data->boost_keys_down);                                                \
                LOG_DBG("Wheel boost key %d released (%ld down)", (int)ev->position,               \
                        (long)atomic_get(&data->boost_keys_down));                                 \
            }                                                                                      \
        }                                                                                          \
        if (is_listed(cfg->precision_positions, cfg->num_precision_positions, ev->position)) {     \
            if (ev->state) {                                                                       \
                atomic_inc(&data->precision_keys_down);                                            \
                atomic_set(&data->mute_until_ms, 0);                                               \
                LOG_DBG("Precision key %d pressed (%ld down)", (int)ev->position,                  \
                        (long)atomic_get(&data->precision_keys_down));                             \
            } else if (atomic_get(&data->precision_keys_down) > 0) {                               \
                atomic_dec(&data->precision_keys_down);                                            \
                LOG_DBG("Precision key %d released (%ld down)", (int)ev->position,                 \
                        (long)atomic_get(&data->precision_keys_down));                             \
                if (atomic_get(&data->precision_keys_down) == 0) {                                 \
                    uint32_t now = k_uptime_get_32();                                              \
                    if ((now - data->last_move_ms) < cfg->release_mute_ms &&                       \
                        layer_gate_open(cfg)) {                                                    \
                        atomic_set(&data->mute_until_ms,                                           \
                                   (atomic_val_t)(now + cfg->release_mute_ms));                    \
                        LOG_DBG("Movement muted for %u ms", (unsigned)cfg->release_mute_ms);       \
                    }                                                                              \
                }                                                                                  \
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
    static const uint16_t pointer_boost_wheel_positions_##n[] = DT_INST_PROP(n, boost_positions);  \
    static const uint16_t pointer_boost_precision_positions_##n[] =                                \
        DT_INST_PROP(n, precision_positions);                                                      \
    static const struct pointer_boost_config pointer_boost_config_##n = {                          \
        .boost_factor = DT_INST_PROP(n, boost_factor),                                             \
        .move_divisor = DT_INST_PROP_OR(n, move_divisor, 1),                                       \
        .move_max_step = DT_INST_PROP_OR(n, move_max_step, 0),                                     \
        .release_mute_ms = DT_INST_PROP_OR(n, release_mute_ms, 200),                                \
        .active_layer = DT_INST_PROP_OR(n, active_layer, -1),                                      \
        .boost_positions = pointer_boost_wheel_positions_##n,                                      \
        .num_boost_positions = DT_INST_PROP_LEN(n, boost_positions),                               \
        .precision_positions = pointer_boost_precision_positions_##n,                              \
        .num_precision_positions = DT_INST_PROP_LEN(n, precision_positions),                       \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL, &pointer_boost_data_##n, &pointer_boost_config_##n,       \
                          POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                        \
                          &pointer_boost_driver_api);

DT_INST_FOREACH_STATUS_OKAY(POINTER_BOOST_INST)
