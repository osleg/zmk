/*
 * Copyright (c) 2022 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_smart_interrupt

#include <device.h>
#include <drivers/behavior.h>
#include <logging/log.h>
#include <zmk/behavior.h>
#include <zmk/keymap.h>
#include <zmk/matrix.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/hid.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define ZMK_BHV_MAX_ACTIVE_SMART_INTERRUPTS 10

struct behavior_smart_interrupt_config {
    int32_t shared_key_positions_len;
    int32_t shared_layers_len;
    struct zmk_behavior_binding *behaviors;
    int32_t shared_layers[32];
    int32_t shared_key_positions[];
};

struct active_smart_interrupt {
    bool is_active;
    bool is_pressed;
    bool first_press;
    uint32_t position;
    const struct behavior_smart_interrupt_config *config;
};

struct active_smart_interrupt active_smart_interruptes[ZMK_BHV_MAX_ACTIVE_SMART_INTERRUPTS] = {};

static struct active_smart_interrupt *find_smart_interrupt(uint32_t position) {
    for (int i = 0; i < ZMK_BHV_MAX_ACTIVE_SMART_INTERRUPTS; i++) {
        if (active_smart_interruptes[i].position == position &&
            active_smart_interruptes[i].is_active) {
            return &active_smart_interruptes[i];
        }
    }
    return NULL;
}

static int new_smart_interrupt(uint32_t position,
                               const struct behavior_smart_interrupt_config *config,
                               struct active_smart_interrupt **smart_interrupt) {
    for (int i = 0; i < ZMK_BHV_MAX_ACTIVE_SMART_INTERRUPTS; i++) {
        struct active_smart_interrupt *const ref_smart_interrupt = &active_smart_interruptes[i];
        if (!ref_smart_interrupt->is_active) {
            ref_smart_interrupt->position = position;
            ref_smart_interrupt->config = config;
            ref_smart_interrupt->is_active = true;
            ref_smart_interrupt->is_pressed = false;
            ref_smart_interrupt->first_press = true;
            *smart_interrupt = ref_smart_interrupt;
            return 0;
        }
    }
    return -ENOMEM;
}

static bool is_other_key_shared(struct active_smart_interrupt *smart_interrupt, int32_t position) {
    for (int i = 0; i < smart_interrupt->config->shared_key_positions_len; i++) {
        if (smart_interrupt->config->shared_key_positions[i] == position) {
            return true;
        }
    }
    return false;
}

static bool is_layer_shared(struct active_smart_interrupt *smart_interrupt, int32_t layer) {
    for (int i = 0; i < smart_interrupt->config->shared_layers_len; i++) {
        if (smart_interrupt->config->shared_layers[i] == layer) {
            return true;
        }
    }
    return false;
}

static int on_smart_interrupt_binding_pressed(struct zmk_behavior_binding *binding,
                                              struct zmk_behavior_binding_event event) {
    const struct device *dev = device_get_binding(binding->behavior_dev);
    const struct behavior_smart_interrupt_config *cfg = dev->config;
    struct active_smart_interrupt *smart_interrupt;
    smart_interrupt = find_smart_interrupt(event.position);
    if (smart_interrupt == NULL) {
        if (new_smart_interrupt(event.position, cfg, &smart_interrupt) == -ENOMEM) {
            LOG_ERR("Unable to create new smart_interrupt. Insufficient space in "
                    "active_smart_interruptes[].");
            return ZMK_BEHAVIOR_OPAQUE;
        }
        LOG_DBG("%d created new smart_interrupt", event.position);
    }
    LOG_DBG("%d smart_interrupt pressed", event.position);
    smart_interrupt->is_pressed = true;
    if (smart_interrupt->first_press) {
        behavior_keymap_binding_pressed(&cfg->behaviors[0], event);
        behavior_keymap_binding_released(&cfg->behaviors[0], event);
        smart_interrupt->first_press = false;
    }
    behavior_keymap_binding_pressed(&cfg->behaviors[1], event);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_smart_interrupt_binding_released(struct zmk_behavior_binding *binding,
                                               struct zmk_behavior_binding_event event) {
    const struct device *dev = device_get_binding(binding->behavior_dev);
    const struct behavior_smart_interrupt_config *cfg = dev->config;
    LOG_DBG("%d smart_interrupt keybind released", event.position);
    struct active_smart_interrupt *smart_interrupt = find_smart_interrupt(event.position);
    if (smart_interrupt == NULL) {
        return ZMK_BEHAVIOR_OPAQUE;
    }
    smart_interrupt->is_pressed = false;
    behavior_keymap_binding_released(&cfg->behaviors[1], event);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int smart_interrupt_init(const struct device *dev) { return 0; }

static const struct behavior_driver_api behavior_smart_interrupt_driver_api = {
    .binding_pressed = on_smart_interrupt_binding_pressed,
    .binding_released = on_smart_interrupt_binding_released,
};

static int smart_interrupt_position_state_changed_listener(const zmk_event_t *eh);
static int smart_interrupt_layer_state_changed_listener(const zmk_event_t *eh);

ZMK_LISTENER(behavior_smart_interrupt, smart_interrupt_position_state_changed_listener);
ZMK_SUBSCRIPTION(behavior_smart_interrupt, zmk_position_state_changed);

ZMK_LISTENER(behavior_smart_interrupt2, smart_interrupt_layer_state_changed_listener);
ZMK_SUBSCRIPTION(behavior_smart_interrupt2, zmk_layer_state_changed);

static int smart_interrupt_position_state_changed_listener(const zmk_event_t *eh) {
    struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    for (int i = 0; i < ZMK_BHV_MAX_ACTIVE_SMART_INTERRUPTS; i++) {
        struct active_smart_interrupt *smart_interrupt = &active_smart_interruptes[i];
        if (!smart_interrupt->is_active) {
            continue;
        }
        if (smart_interrupt->position == ev->position) {
            continue;
        }
        if (!is_other_key_shared(smart_interrupt, ev->position)) {
            LOG_DBG("Smart Interrupt interrupted, ending at %d %d", smart_interrupt->position,
                    ev->position);
            smart_interrupt->is_active = false;
            struct zmk_behavior_binding_event event = {.position = smart_interrupt->position,
                                                       .timestamp = k_uptime_get()};
            if (smart_interrupt->is_pressed) {
                behavior_keymap_binding_released(&smart_interrupt->config->behaviors[1], event);
            }
            behavior_keymap_binding_pressed(&smart_interrupt->config->behaviors[2], event);
            behavior_keymap_binding_released(&smart_interrupt->config->behaviors[2], event);
            return ZMK_EV_EVENT_BUBBLE;
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static int smart_interrupt_layer_state_changed_listener(const zmk_event_t *eh) {
    struct zmk_layer_state_changed *ev = as_zmk_layer_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (!ev->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    for (int i = 0; i < ZMK_BHV_MAX_ACTIVE_SMART_INTERRUPTS; i++) {
        struct active_smart_interrupt *smart_interrupt = &active_smart_interruptes[i];
        if (!smart_interrupt->is_active) {
            continue;
        }
        if (!is_layer_shared(smart_interrupt, ev->layer)) {
            LOG_DBG("Smart Interrupt layer changed, ending at %d %d", smart_interrupt->position,
                    ev->layer);
            smart_interrupt->is_active = false;
            struct zmk_behavior_binding_event event = {.position = smart_interrupt->position,
                                                       .timestamp = k_uptime_get()};
            if (smart_interrupt->is_pressed) {
                behavior_keymap_binding_released(&smart_interrupt->config->behaviors[1], event);
            }
            behavior_keymap_binding_pressed(&smart_interrupt->config->behaviors[2], event);
            behavior_keymap_binding_released(&smart_interrupt->config->behaviors[2], event);
            return ZMK_EV_EVENT_BUBBLE;
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}

#define _TRANSFORM_ENTRY(idx, node) ZMK_KEYMAP_EXTRACT_BINDING(idx, node),

#define TRANSFORMED_BINDINGS(node)                                                                 \
    { UTIL_LISTIFY(DT_INST_PROP_LEN(node, bindings), _TRANSFORM_ENTRY, DT_DRV_INST(node)) }

#define SMART_INTERRUPT_INST(n)                                                                    \
    static struct zmk_behavior_binding                                                             \
        behavior_smart_interrupt_config_##n##_bindings[DT_INST_PROP_LEN(n, bindings)] =            \
            TRANSFORMED_BINDINGS(n);                                                               \
    static struct behavior_smart_interrupt_config behavior_smart_interrupt_config_##n = {          \
        .shared_key_positions = DT_INST_PROP(n, shared_key_positions),                             \
        .shared_key_positions_len = DT_INST_PROP_LEN(n, shared_key_positions),                     \
        .shared_layers = DT_INST_PROP(n, shared_layers),                                           \
        .shared_layers_len = DT_INST_PROP_LEN(n, shared_layers),                                   \
        .behaviors = behavior_smart_interrupt_config_##n##_bindings};                              \
    DEVICE_DT_INST_DEFINE(                                                                         \
        n, smart_interrupt_init, NULL, NULL, &behavior_smart_interrupt_config_##n, APPLICATION,    \
        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_smart_interrupt_driver_api);

DT_INST_FOREACH_STATUS_OKAY(SMART_INTERRUPT_INST)
