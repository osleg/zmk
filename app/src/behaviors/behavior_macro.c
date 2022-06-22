/*
 * Copyright (c) 2022 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <device.h>
#include <drivers/behavior.h>
#include <logging/log.h>
#include <zmk/behavior.h>
#include <zmk/behavior_queue.h>
#include <zmk/keymap.h>
#include <zmk/events/keycode_state_changed.h>
#include <dt-bindings/zmk/dynamic-macros.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(zmk_behavior_macro) ||                                               \
    DT_HAS_COMPAT_STATUS_OKAY(zmk_behavior_dynamic_macro)

enum behavior_macro_mode {
    MACRO_MODE_TAP,
    MACRO_MODE_PRESS,
    MACRO_MODE_RELEASE,
};

struct behavior_macro_trigger_state {
    uint32_t wait_ms;
    uint32_t tap_ms;
    enum behavior_macro_mode mode;
    uint16_t start_index;
    uint16_t count;
};

#define ZMK_BHV_DYNAMIC_MACRO_MAX_ACTIONS 80
#define HID_KEY_USAGE_PAGE 0x70000

struct behavior_macro_state {
    struct behavior_macro_trigger_state release_state;

    bool recording;
    uint64_t lastEventTime;
    uint32_t press_bindings_count;
    struct zmk_behavior_binding dynamic_bindings[ZMK_BHV_DYNAMIC_MACRO_MAX_ACTIONS];
};

struct behavior_macro_config {
    uint32_t default_wait_ms;
    uint32_t default_tap_ms;
    uint32_t count;
    bool dynamic;
    struct zmk_behavior_binding bindings[];
};

#define TAP_MODE DT_LABEL(DT_INST(0, zmk_macro_control_mode_tap))
#define PRESS_MODE DT_LABEL(DT_INST(0, zmk_macro_control_mode_press))
#define REL_MODE DT_LABEL(DT_INST(0, zmk_macro_control_mode_release))

#define TAP_TIME DT_LABEL(DT_INST(0, zmk_macro_control_tap_time))
#define WAIT_TIME DT_LABEL(DT_INST(0, zmk_macro_control_wait_time))
#define WAIT_REL DT_LABEL(DT_INST(0, zmk_macro_pause_for_release))

#define ZM_IS_NODE_MATCH(a, b) (strcmp(a, b) == 0)
#define IS_TAP_MODE(dev) ZM_IS_NODE_MATCH(dev, TAP_MODE)
#define IS_PRESS_MODE(dev) ZM_IS_NODE_MATCH(dev, PRESS_MODE)
#define IS_RELEASE_MODE(dev) ZM_IS_NODE_MATCH(dev, REL_MODE)

#define IS_TAP_TIME(dev) ZM_IS_NODE_MATCH(dev, TAP_TIME)
#define IS_WAIT_TIME(dev) ZM_IS_NODE_MATCH(dev, WAIT_TIME)
#define IS_PAUSE(dev) ZM_IS_NODE_MATCH(dev, WAIT_REL)

#define ZMK_BHV_RECORDING_MACRO_MAX 10

struct recording_macro {
    uint32_t count;
    uint32_t position;
    bool recording;
    const struct behavior_macro_config *config;
    struct behavior_macro_state *state;
};

struct recording_macro recording_macros[ZMK_BHV_RECORDING_MACRO_MAX] = {};

static struct recording_macro *find_recording_macro(uint32_t position) {
    for (int i = 0; i < ZMK_BHV_RECORDING_MACRO_MAX; i++) {
        if (recording_macros[i].position == position && recording_macros[i].recording) {
            return &recording_macros[i];
        }
    }
    return NULL;
}

static int new_recording_macro(uint32_t position, const struct behavior_macro_config *config,
                               struct behavior_macro_state *state, struct recording_macro **macro) {
    for (int i = 0; i < ZMK_BHV_RECORDING_MACRO_MAX; i++) {
        struct recording_macro *const ref_macro = &recording_macros[i];
        if (!ref_macro->recording) {
            ref_macro->recording = true;
            ref_macro->count = 0;
            ref_macro->position = position;
            ref_macro->config = config;
            ref_macro->state = state;
            *macro = ref_macro;
            return 0;
        }
    }
    return -ENOMEM;
}

static bool handle_control_binding(struct behavior_macro_trigger_state *state,
                                   const struct zmk_behavior_binding *binding) {
    if (IS_TAP_MODE(binding->behavior_dev)) {
        state->mode = MACRO_MODE_TAP;
        LOG_DBG("macro mode set: tap");
    } else if (IS_PRESS_MODE(binding->behavior_dev)) {
        state->mode = MACRO_MODE_PRESS;
        LOG_DBG("macro mode set: press");
    } else if (IS_RELEASE_MODE(binding->behavior_dev)) {
        state->mode = MACRO_MODE_RELEASE;
        LOG_DBG("macro mode set: release");
    } else if (IS_TAP_TIME(binding->behavior_dev)) {
        state->tap_ms = binding->param1;
        LOG_DBG("macro tap time set: %d", state->tap_ms);
    } else if (IS_WAIT_TIME(binding->behavior_dev)) {
        state->wait_ms = binding->param1;
        LOG_DBG("macro wait time set: %d", state->wait_ms);
    } else {
        return false;
    }

    return true;
}

static int behavior_macro_init(const struct device *dev) {
    const struct behavior_macro_config *cfg = dev->config;
    struct behavior_macro_state *state = dev->data;
    state->press_bindings_count = cfg->count;
    state->release_state.start_index = cfg->count;
    state->release_state.count = 0;
    state->lastEventTime = k_uptime_get();

    LOG_DBG("Precalculate initial release state:");
    for (int i = 0; i < cfg->count; i++) {
        if (handle_control_binding(&state->release_state, &cfg->bindings[i])) {
            // Updated state used for initial state on release.
        } else if (IS_PAUSE(cfg->bindings[i].behavior_dev)) {
            state->release_state.start_index = i + 1;
            state->release_state.count = cfg->count - state->release_state.start_index;
            state->press_bindings_count = i;
            LOG_DBG("Release will resume at %d", state->release_state.start_index);
            break;
        } else {
            // Ignore regular invokable bindings
        }
    }

    return 0;
};

static void queue_macro(uint32_t position, const struct zmk_behavior_binding bindings[],
                        struct behavior_macro_trigger_state state) {
    LOG_DBG("Iterating macro bindings - starting: %d, count: %d", state.start_index, state.count);
    for (int i = state.start_index; i < state.start_index + state.count; i++) {
        if (!handle_control_binding(&state, &bindings[i])) {
            switch (state.mode) {
            case MACRO_MODE_TAP:
                zmk_behavior_queue_add(position, bindings[i], true, state.tap_ms);
                zmk_behavior_queue_add(position, bindings[i], false, state.wait_ms);
                break;
            case MACRO_MODE_PRESS:
                zmk_behavior_queue_add(position, bindings[i], true, state.wait_ms);
                break;
            case MACRO_MODE_RELEASE:
                zmk_behavior_queue_add(position, bindings[i], false, state.wait_ms);
                break;
            default:
                LOG_ERR("Unknown macro mode: %d", state.mode);
                break;
            }
        }
    }
}

static int on_macro_binding_pressed(struct zmk_behavior_binding *binding,
                                    struct zmk_behavior_binding_event event) {
    const struct device *dev = device_get_binding(binding->behavior_dev);
    const struct behavior_macro_config *cfg = dev->config;
    struct behavior_macro_state *state = dev->data;

    struct behavior_macro_trigger_state trigger_state = {.mode = MACRO_MODE_TAP,
                                                         .tap_ms = cfg->default_tap_ms,
                                                         .wait_ms = cfg->default_wait_ms,
                                                         .start_index = 0,
                                                         .count = state->press_bindings_count};

    if (cfg->dynamic && binding->param1 == PLAY) {
        LOG_DBG("Playing Dynamic Macro");
        queue_macro(event.position, state->dynamic_bindings, trigger_state);
    } else if (cfg->dynamic && binding->param1 == RECORD) {
        state->recording = !state->recording;
        LOG_DBG("Recording Status: %d", state->recording);
        if (state->recording) {
            struct recording_macro *macro;
            macro = find_recording_macro(event.position);
            if (new_recording_macro(event.position, cfg, state, &macro) == -ENOMEM) {
                LOG_ERR("Unable to record new macro. Insufficient space in recording_macros[]");
                return ZMK_BEHAVIOR_OPAQUE;
            }
            LOG_DBG("Recording new macro: %d", event.position);
            macro->count = 0;
        } else {
            struct recording_macro *macro;
            macro = find_recording_macro(event.position);
            macro->recording = false;
            macro->state->press_bindings_count = macro->count;
        }
    } else {
        queue_macro(event.position, cfg->bindings, trigger_state);
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_macro_binding_released(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    const struct device *dev = device_get_binding(binding->behavior_dev);
    const struct behavior_macro_config *cfg = dev->config;
    struct behavior_macro_state *state = dev->data;

    if (cfg->dynamic && binding->param1 == PLAY) {
        queue_macro(event.position, state->dynamic_bindings, state->release_state);
    } else {
        queue_macro(event.position, cfg->bindings, state->release_state);
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_macro_driver_api = {
    .binding_pressed = on_macro_binding_pressed,
    .binding_released = on_macro_binding_released,
};

static int dynamic_macro_keycode_state_changed_listener(const zmk_event_t *eh);

ZMK_LISTENER(behavior_dynamic_macro, dynamic_macro_keycode_state_changed_listener);
ZMK_SUBSCRIPTION(behavior_dynamic_macro, zmk_keycode_state_changed);

static int dynamic_macro_keycode_state_changed_listener(const zmk_event_t *eh) {
    struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    for (int i = 0; i < ZMK_BHV_RECORDING_MACRO_MAX; i++) {
        struct recording_macro *macro = &recording_macros[i];
        if (macro->recording) {
            // uint64_t eventTime = k_uptime_get();
            // uint64_t elapsedTime = eventTime - macro->state->lastEventTime;
            // macro->state->lastEventTime = eventTime;
            // LOG_DBG("CURRENT TIME: %lld, LAST EVENT TIME: %lld, ELAPSED TIME: %lld", eventTime,
            //         macro->state->lastEventTime, elapsedTime);
            if (ev->state) {
                macro->state->dynamic_bindings[macro->count].behavior_dev = "MAC_PRESS";
            } else {
                macro->state->dynamic_bindings[macro->count].behavior_dev = "MAC_REL";
            }
            macro->state->dynamic_bindings[macro->count].param1 = 0;
            macro->state->dynamic_bindings[macro->count].param2 = 0;
            macro->state->dynamic_bindings[macro->count + 1].behavior_dev = "KEY_PRESS";
            macro->state->dynamic_bindings[macro->count + 1].param1 =
                HID_KEY_USAGE_PAGE + ev->keycode;
            macro->state->dynamic_bindings[macro->count + 1].param2 = 0;
            macro->count += 2;
            continue;
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}

#define BINDING_WITH_COMMA(idx, inst) ZMK_KEYMAP_EXTRACT_BINDING(idx, inst),

#define TRANSFORMED_BEHAVIORS(inst)                                                                \
    {UTIL_LISTIFY(DT_PROP_LEN(inst, bindings), BINDING_WITH_COMMA, inst)},

#define MACRO_INST(n)                                                                              \
    static struct behavior_macro_state behavior_macro_state_##n = {};                              \
    static struct behavior_macro_config behavior_macro_config_##n = {                              \
        .default_wait_ms = DT_PROP_OR(n, wait_ms, 100),                                            \
        .default_tap_ms = DT_PROP_OR(n, tap_ms, 100),                                              \
        .count = DT_PROP_LEN(n, bindings),                                                         \
        .dynamic = false,                                                                          \
        .bindings = TRANSFORMED_BEHAVIORS(n)};                                                     \
    DEVICE_DT_DEFINE(n, behavior_macro_init, NULL, &behavior_macro_state_##n,                      \
                     &behavior_macro_config_##n, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, \
                     &behavior_macro_driver_api);

#define DYNAMIC_MACRO_INST(n)                                                                      \
    static struct behavior_macro_state behavior_macro_state_##n = {.recording = false};            \
    static struct behavior_macro_config behavior_macro_config_##n = {                              \
        .default_wait_ms = DT_PROP_OR(n, wait_ms, 100),                                            \
        .default_tap_ms = DT_PROP_OR(n, tap_ms, 100),                                              \
        .count = 0,                                                                                \
        .dynamic = true};                                                                          \
    DEVICE_DT_DEFINE(n, behavior_macro_init, NULL, &behavior_macro_state_##n,                      \
                     &behavior_macro_config_##n, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, \
                     &behavior_macro_driver_api);

DT_FOREACH_STATUS_OKAY(zmk_behavior_macro, MACRO_INST)
DT_FOREACH_STATUS_OKAY(zmk_behavior_dynamic_macro, DYNAMIC_MACRO_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
