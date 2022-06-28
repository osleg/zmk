---
title: Smart Interrupt Behavior
sidebar_label: Smart Interrupt
---

## Summary

Smart Interrupts are a way to have something persist while other behaviors occur.

The smart interrupt key will fire the 'start' behavior when the key is pressed for the first time. Subsequent presses of the same key will output the second, 'continue' behavior, and any key position or layer state change that is not specified (see below) will trigger the 'interrupt behavior'.

### Basic Usage

The following is a basic definition of a smart interrupt:

```
/ {
    behaviors {
        smart-int: smart-interrupt {
            compatible = "zmk,behavior-smart-interrupt";
            label = "SMART-INTERRUPT";
            #binding-cells = <0>;
            bindings = <&kp A>, <&kp B>, <&kt C>;
        };
    };

    keymap {
        compatible = "zmk,keymap";
        label ="Default keymap";

        default_layer {
            bindings = <
                &smart-int  &kp D
                &kp E       &kp F>;
        };
    };
};
```

Pressing `smart-int` will fire the first behavior, and output `A`, as well as the second behavior, outputting `B`. Subsequent presses of `smart-int` will output `B`. When another key is pressed or a layer change occurs, the third, 'interrupt' behavior will fire.

### Advanced Configuration

#### `shared-key-positions`

- Including `shared-key-positions` in your smart interrupt definition will let the key positions specified NOT trigger the interrupt behavior when a smart interrupt is active.
- Pressing any key **NOT** listed in `shared-key-positions` will cause the interrupt behavior to fire.
- Note that `shared-key-positions` is an array of key position indexes. Key positions are numbered according to your keymap, starting with 0. So if the first key in your keymap is Q, this key is in position 0. The next key (probably W) will be in position 1, et cetera.
- See the following example, which is an implementation of the popular [Swapper](https://github.com/callum-oakley/qmk_firmware/tree/master/users/callum) from Callum Oakley:

```
/ {
    behaviors {
        swap: swapper {
            compatible = "zmk,behavior-smart-interrupt";
            label = "SWAPPER";
            #binding-cells = <0>;
            bindings = <&kt LALT>, <&kp TAB>, <&kt LALT>;
            shared-key-positions = <1>;
        };
    };

    keymap {
        compatible = "zmk,keymap";
        label ="Default keymap";

        default_layer {
            bindings = <
                &swap    &kp LS(TAB)
                &kp B    &kp C>;
        };
    };
};
```

- The sequence `(swap, swap, LS(TAB))` produces `(LA(TAB), LA(TAB), LA(LS(TAB)))`. The LS(TAB) behavior does not fire the interrupt behavior, because it is included in `shared-key-positions`.
- The sequence `(swap, swap, B)` produces `(LA(TAB), LA(TAB), B)`. The B behavior **does** fire the interrupt behavior, because it is **not** included in `shared-key-positions`.

#### `shared-layers`

- Including `shared-layers` in your smart interrupt definition will let the specified layers NOT trigger the active smart interrupt behavior when they become active (include the layer the behavior is on to accommodate for layer toggling).
- Activating any layer **NOT** listed in `shared-layers` will cause the interrupt behavior to fire.
- Note that `shared-layers` is an array of layer indexes. Layers are numbered according to your keymap, starting with 0. The first layer in your keymap is layer 0. The next layer will be layer 1, et cetera.
- Looking back at the swapper implementation, we can see how `shared-layers` can affect things

```
/ {
    behaviors {
        swap: swapper {
            compatible = "zmk,behavior-smart-interrupt";
            label = "SWAPPER";
            #binding-cells = <0>;
            bindings = <&kt LALT>, <&kp TAB>, <&kt LALT>;
            shared-key-positions = <1 2 3>;
            shared-layers = <1>;
        };
    };

    keymap {
        compatible = "zmk,keymap";
        label ="Default keymap";

        default_layer {
            bindings = <
                &swap    &kp LS(TAB)
                &kp B    &tog 1>;
        };

        layer2 {
            bindings = <
                &kp DOWN    &kp B
                &tog 2    &trans>;
        };

        layer3 {
            bindings = <
                &kp LEFT  &kp N2
                &trans    &kp N3>;
        };
    };
};
```

- The sequence `(swap, tog 1, DOWN)` produces `(LA(TAB), LA(DOWN))`. The change to layer 1 does not fire the interrupt behavior, because it is included in `shared-layers`, and DOWN is in the same position as the smart interrupt, also not firing the interrupt behavior.
- The sequence `(swap, tog 1, tog 2, LEFT)` produces `(LA(TAB), LEFT`. The change to layer 2 **does** fire the interrupt behavior, because it is not included in `shared-layers`.
