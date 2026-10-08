/*
 * "Exit layer -> momentary layer" behavior.
 *
 * Usage: &mfl <exit_layer> <hold_layer>
 *   press:   deactivate exit_layer (e.g. the pointing-device temp mouse
 *            layer, which also stops its exit timer via the layer-state
 *            listener) and activate hold_layer momentarily
 *   release: deactivate hold_layer
 *
 * Used on the MOUSE layer's LOWER-thumb key so that pressing it while the
 * mouse layer is up instantly drops the mouse layer and enters the number
 * layer, instead of typing the mouse layer's letters.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_mfl

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>

#include <zmk/keymap.h>
#include <zmk/behavior.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static int mfl_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    LOG_DBG("position %d exit layer %d, hold layer %d", event.position, binding->param1,
            binding->param2);
    zmk_keymap_layer_deactivate(binding->param1);
    return zmk_keymap_layer_activate(binding->param2);
}

static int mfl_keymap_binding_released(struct zmk_behavior_binding *binding,
                                       struct zmk_behavior_binding_event event) {
    LOG_DBG("position %d hold layer %d up", event.position, binding->param2);
    return zmk_keymap_layer_deactivate(binding->param2);
}

static const struct behavior_driver_api behavior_mfl_driver_api = {
    .binding_pressed = mfl_keymap_binding_pressed,
    .binding_released = mfl_keymap_binding_released,
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_mfl_driver_api);
