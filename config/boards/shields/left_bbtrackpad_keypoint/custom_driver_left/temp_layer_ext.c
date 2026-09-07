/*
 * Extended temporary layer input processor.
 *
 * Based on ZMK's zmk,input-processor-temp-layer (v0.3.0). The extension is an
 * "arm-only" mode: such instances never activate the layer themselves, but
 * while the layer is already active every event they see pushes the layer
 * exit deadline out again. It is attached to the &mkp input listener so that
 * keyboard mouse buttons (&mkp LCLK / RCLK / MCLK, including inside tap
 * dances and hold-taps) keep refreshing the mouse layer exit timer.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_input_processor_temp_layer_ext

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <drivers/input_processor.h>
#include <zephyr/logging/log.h>
#include <zmk/keymap.h>
#include <zmk/behavior.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/layer_state_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* The stock temp layer driver is no longer instantiated by this config, so
 * its Kconfig may be absent; keep the same default queue depth. */
#ifndef CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_MAX_ACTION_EVENTS
#define CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_MAX_ACTION_EVENTS 4
#endif

/* Constants and Types */
#define MAX_LAYERS ZMK_KEYMAP_LAYERS_LEN

struct temp_layer_config {
    int16_t require_prior_idle_ms;
    bool arm_only;
    const uint16_t *excluded_positions;
    size_t num_positions;
};

struct temp_layer_state {
    uint8_t toggle_layer;
    bool is_active;
    int64_t last_tapped_timestamp;
};

struct temp_layer_data {
    const struct device *dev;
    struct k_mutex lock;
    struct temp_layer_state state;
};

/* Static Work Queue Items. Keyed by layer id and shared by every instance of
 * this compatible, so an arm-only refresh reschedules the very work item the
 * activation instance armed. */
static struct k_work_delayable layer_disable_works[MAX_LAYERS];

/* Mouse buttons currently held (INPUT_BTN_0..4 bitmap) and the last timeout
 * used per layer. While a button is held there are no input events, so the
 * exit timer would otherwise fire mid-drag; layer_disable_callback re-arms
 * instead of deactivating until every button is released. */
static atomic_t held_buttons;
static atomic_t layer_timeouts[MAX_LAYERS];

static void track_mouse_button(const struct input_event *event) {
    if (event->type != INPUT_EV_KEY || event->code < INPUT_BTN_0 ||
        event->code > INPUT_BTN_4) {
        return;
    }
    uint8_t bit = (uint8_t)(event->code - INPUT_BTN_0);
    if (event->value > 0) {
        atomic_set_bit(&held_buttons, bit);
    } else {
        atomic_clear_bit(&held_buttons, bit);
    }
}

/* Position Search */
static bool position_is_excluded(const struct temp_layer_config *config, uint32_t position) {
    if (!config->excluded_positions || !config->num_positions) {
        return false;
    }

    const uint16_t *end = config->excluded_positions + config->num_positions;
    for (const uint16_t *pos = config->excluded_positions; pos < end; pos++) {
        if (*pos == position) {
            return true;
        }
    }
    return false;
}

/* Timing Check */
static bool should_quick_tap(const struct temp_layer_config *config, int64_t last_tapped,
                             int64_t current_time) {
    return (last_tapped + config->require_prior_idle_ms) > current_time;
}

/* Layer State Management */
static void update_layer_state(struct temp_layer_state *state, bool activate) {
    if (state->is_active == activate) {
        return;
    }

    state->is_active = activate;
    if (activate) {
        zmk_keymap_layer_activate(state->toggle_layer);
        LOG_DBG("Layer %d activated", state->toggle_layer);
    } else {
        zmk_keymap_layer_deactivate(state->toggle_layer);
        LOG_DBG("Layer %d deactivated", state->toggle_layer);
    }
}

struct layer_state_action {
    uint8_t layer;
    bool activate;
};

K_MSGQ_DEFINE(temp_layer_action_msgq, sizeof(struct layer_state_action),
              CONFIG_ZMK_INPUT_PROCESSOR_TEMP_LAYER_MAX_ACTION_EVENTS, 4);

static void layer_action_work_cb(struct k_work *work) {

    const struct device *dev = DEVICE_DT_INST_GET(0);
    struct temp_layer_data *data = (struct temp_layer_data *)dev->data;

    int ret = k_mutex_lock(&data->lock, K_FOREVER);
    if (ret < 0) {
        LOG_ERR("Error locking for updating %d", ret);
        return;
    }

    struct layer_state_action action;

    while (k_msgq_get(&temp_layer_action_msgq, &action, K_MSEC(10)) >= 0) {
        if (!action.activate) {
            if (zmk_keymap_layer_active(action.layer)) {
                update_layer_state(&data->state, false);
            }
        } else {
            update_layer_state(&data->state, true);
        }
    }

    k_mutex_unlock(&data->lock);
}

static K_WORK_DEFINE(layer_action_work, layer_action_work_cb);

/* Work Queue Callback */
static void layer_disable_callback(struct k_work *work) {
    struct k_work_delayable *d_work = k_work_delayable_from_work(work);
    int layer_index = ARRAY_INDEX(layer_disable_works, d_work);

    if (atomic_get(&held_buttons) != 0) {
        /* A mouse button is still held (e.g. dragging): no events are coming
         * to refresh the timer, so extend the deadline instead of exiting. */
        int timeout = (int)atomic_get(&layer_timeouts[layer_index]);
        if (timeout <= 0) {
            timeout = 600;
        }
        k_work_reschedule(d_work, K_MSEC(timeout));
        return;
    }

    struct layer_state_action action = {.layer = layer_index, .activate = false};

    int ret = k_msgq_put(&temp_layer_action_msgq, &action, K_MSEC(10));
    k_work_submit(&layer_action_work);
}

/* Event Handlers */
static int handle_layer_state_changed(const struct device *dev, const zmk_event_t *eh) {
    struct temp_layer_data *data = (struct temp_layer_data *)dev->data;
    int ret = k_mutex_lock(&data->lock, K_FOREVER);
    if (ret < 0) {
        return ret;
    }
    if (!zmk_keymap_layer_active(zmk_keymap_layer_index_to_id(data->state.toggle_layer))) {
        LOG_DBG("Deactivating layer that was activated by this processor");
        data->state.is_active = false;
        k_work_cancel_delayable(&layer_disable_works[data->state.toggle_layer]);
    }
    ret = k_mutex_unlock(&data->lock);
    if (ret < 0) {
        return ret;
    }

    return ZMK_EV_EVENT_BUBBLE;
}

static int handle_position_state_changed(const struct device *dev, const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (!ev->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    struct temp_layer_data *data = (struct temp_layer_data *)dev->data;
    int ret = k_mutex_lock(&data->lock, K_FOREVER);
    if (ret < 0) {
        return ret;
    }

    const struct temp_layer_config *cfg = dev->config;

    if (data->state.is_active && cfg->excluded_positions && cfg->num_positions > 0) {
        if (!position_is_excluded(cfg, ev->position)) {
            LOG_DBG("Position not excluded, deactivating layer");
            update_layer_state(&data->state, false);
        }
    }
    LOG_DBG("Position excluded, continuing");

    k_mutex_unlock(&data->lock);

    return ZMK_EV_EVENT_BUBBLE;
}

static int handle_keycode_state_changed(const struct device *dev, const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (!ev->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    struct temp_layer_data *data = (struct temp_layer_data *)dev->data;

    int ret = k_mutex_lock(&data->lock, K_FOREVER);
    if (ret < 0) {
        return ret;
    }

    LOG_DBG("Setting last_tapped_timestamp to: %d", ev->timestamp);
    data->state.last_tapped_timestamp = ev->timestamp;

    ret = k_mutex_unlock(&data->lock);
    if (ret < 0) {
        return ret;
    }

    return ZMK_EV_EVENT_BUBBLE;
}

static int handle_state_changed_dispatcher(const struct device *dev, const zmk_event_t *eh) {
    if (as_zmk_layer_state_changed(eh) != NULL) {
        LOG_DBG("Dispatching handle_layer_state_changed");
        return handle_layer_state_changed(dev, eh);
    } else if (as_zmk_position_state_changed(eh) != NULL) {
        LOG_DBG("Dispatching handle_position_state_changed");
        return handle_position_state_changed(dev, eh);
    } else if (as_zmk_keycode_state_changed(eh) != NULL) {
        LOG_DBG("Dispatching handle_keycode_state_changed");
        return handle_keycode_state_changed(dev, eh);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

#define DISPATCH_EVENT(inst)                                                                       \
    {                                                                                              \
        int err = handle_state_changed_dispatcher(DEVICE_DT_INST_GET(inst), eh);                   \
        if (err < 0) {                                                                             \
            return err;                                                                            \
        }                                                                                          \
    }

static int handle_event_dispatcher(const zmk_event_t *eh) {
    DT_INST_FOREACH_STATUS_OKAY(DISPATCH_EVENT)

    return 0;
}

/* Driver Implementation */
static int temp_layer_handle_event(const struct device *dev, struct input_event *event,
                                   uint32_t param1, uint32_t param2,
                                   struct zmk_input_processor_state *state) {
    if (param1 >= MAX_LAYERS) {
        LOG_ERR("Invalid layer index: %d", param1);
        return -EINVAL;
    }

    struct temp_layer_data *data = (struct temp_layer_data *)dev->data;

    int ret = k_mutex_lock(&data->lock, K_FOREVER);
    if (ret < 0) {
        return ret;
    }

    const struct temp_layer_config *cfg = dev->config;

    data->state.toggle_layer = param1;

    track_mouse_button(event);
    if (param2 > 0) {
        atomic_set(&layer_timeouts[param1], (atomic_val_t)param2);
    }

    if (cfg->arm_only) {
        /* Arm-only instances never turn the layer on by themselves; they only
         * keep extending the exit countdown while the layer is active, e.g.
         * keyboard mouse buttons keeping a pointing device temp layer alive. */
        if (param2 > 0 && zmk_keymap_layer_active(param1)) {
            LOG_DBG("arm refresh: layer %d held=0x%02x", param1,
                    (unsigned)atomic_get(&held_buttons));
            k_work_reschedule(&layer_disable_works[param1], K_MSEC(param2));
        }
    } else {
        if (!data->state.is_active &&
            !should_quick_tap(cfg, data->state.last_tapped_timestamp, k_uptime_get())) {
            struct layer_state_action action = {.layer = param1, .activate = true};

            int ret = k_msgq_put(&temp_layer_action_msgq, &action, K_MSEC(10));
            k_work_submit(&layer_action_work);
        }

        if (param2 > 0) {
            k_work_reschedule(&layer_disable_works[param1], K_MSEC(param2));
        }
    }

    k_mutex_unlock(&data->lock);

    return ZMK_INPUT_PROC_CONTINUE;
}

static int temp_layer_init(const struct device *dev) {
    struct temp_layer_data *data = (struct temp_layer_data *)dev->data;
    k_mutex_init(&data->lock);

    for (int i = 0; i < MAX_LAYERS; i++) {
        k_work_init_delayable(&layer_disable_works[i], layer_disable_callback);
    }

    return 0;
}

/* Driver API */
static const struct zmk_input_processor_driver_api temp_layer_driver_api = {
    .handle_event = temp_layer_handle_event,
};

/* Event Listeners Conditions. The leading '+' makes the generated
 * "fn(0, sep) fn(1, sep)" expansion valid in #if for multiple instances
 * (expands to e.g. "+0 +0" instead of the invalid "0 0"). */
#define NEEDS_POSITION_HANDLERS(n, ...) +DT_INST_PROP_HAS_IDX(n, excluded_positions, 0)
#define NEEDS_KEYCODE_HANDLERS(n, ...) +(DT_INST_PROP_OR(n, require_prior_idle_ms, 0) > 0)

/* Event Handlers Registration */
ZMK_LISTENER(processor_temp_layer_ext, handle_event_dispatcher);
ZMK_SUBSCRIPTION(processor_temp_layer_ext, zmk_layer_state_changed);

/* Individual Subscriptions */
#if DT_INST_FOREACH_STATUS_OKAY_VARGS(NEEDS_POSITION_HANDLERS, ||)
ZMK_SUBSCRIPTION(processor_temp_layer_ext, zmk_position_state_changed);
#endif

#if DT_INST_FOREACH_STATUS_OKAY_VARGS(NEEDS_KEYCODE_HANDLERS, ||)
ZMK_SUBSCRIPTION(processor_temp_layer_ext, zmk_keycode_state_changed);
#endif

/* Device Instantiation */
#define TEMP_LAYER_INST(n)                                                                         \
    static struct temp_layer_data processor_temp_layer_ext_data_##n = {};                          \
    static const uint16_t excluded_positions_##n[] = DT_INST_PROP(n, excluded_positions);          \
    static const struct temp_layer_config processor_temp_layer_ext_config_##n = {                  \
        .require_prior_idle_ms = DT_INST_PROP_OR(n, require_prior_idle_ms, 0),                     \
        .arm_only = DT_INST_PROP(n, arm_only),                                                     \
        .excluded_positions = excluded_positions_##n,                                              \
        .num_positions = DT_INST_PROP_LEN(n, excluded_positions),                                  \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, temp_layer_init, NULL, &processor_temp_layer_ext_data_##n,            \
                          &processor_temp_layer_ext_config_##n, POST_KERNEL,                       \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &temp_layer_driver_api);

DT_INST_FOREACH_STATUS_OKAY(TEMP_LAYER_INST)
