#pragma once

enum setting_key {
    SK_BUTTON,
    SK_PLAY_PSWITCH,
    SK_PLAY_STAR,
    SK_PLAY_HURRY,
    // SK_PLAY_LAVA,
    NUM_SETTINGS
};

struct setting_values {
    int count;
    const char **menu_values;
    const char **serialized_values;
};
