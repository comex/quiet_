#include "settings.h"
#include "decls.h"
#include "font.h"
#include "logging.h"
#include "types.h"
#include "misc.h"


static const struct setting_values play_dont_sv = {
    2,
    (const char *[]){"Don't Play", "Play"},
    (const char *[]){"dontplay", "play"},
};

static const uint32_t button_bits[]
    = {0x20, 0x80, 0x10, 0x40, 0x2000, 0x1000, 0x8000, 0x4000};
static const struct setting_values button_sv = {
    8,
    (const char *[]){STR_CONTROL_L, STR_CONTROL_Z STR_CONTROL_L, STR_CONTROL_R,
                     STR_CONTROL_Z STR_CONTROL_R, STR_CONTROL_X, STR_CONTROL_Y,
                     STR_CONTROL_A, STR_CONTROL_B},
    (const char *[]){"L", "ZL", "R", "ZR", "X", "Y", "A", "B"},
};

static struct setting_meta {
    const char *menu_name;
    const char *serialized_name;
    const struct setting_values *values;
} setting_meta[] = {
    {"Button To Use", "button", &button_sv},
    {"   P-Switch BGM", "play_pswitch", &play_dont_sv},
    {"   Star BGM", "play_star", &play_dont_sv},
    {"   Hurry Jingle", "play_hurry", &play_dont_sv},
    //{"   Lava Bubbling[??]", "play_lava", &play_dont_sv},
};

static_assert(countof(setting_meta) == NUM_SETTINGS,
              "num_settings mismatch");

static int
xisspace(int c) {
    switch (c) {
    case '\t':
    case '\n':
    case '\v':
    case '\f':
    case '\r':
    case ' ':
        return true;
    default:
        return false;
    }
}

static void
trim(const char **start_out, const char **end_out, const char *start, const char *end) {
    while (start < end && xisspace(*start))
        start++;
    while (start + 1 < end && xisspace(end[-1]))
        end--;
    *start_out = start;
    *end_out = end;
}

static char
xtolower(char c) {
    if (c >= 'A' && c <= 'Z')
        c |= 0x20;
    return c;
}

static bool
mem_eq_case_insensitive(const char *a, const char *b, size_t len) {
    while (len--)
        if (xtolower(*a++) != xtolower(*b++))
            return false;
    return true;
}

static bool
str_eq_mem(const char *left, const char *right_start, const char *right_end) {
    size_t len = (size_t)(right_end - right_start);
    return strlen(left) == len && mem_eq_case_insensitive(left, right_start, len);
}

static void
parse_settings(const char *start, size_t len, int setting_vals[NUM_SETTINGS]) {
    for (int i = 0; i < NUM_SETTINGS; i++)
        setting_vals[i] = 0;
    const char *end = start + len;
    const char *next_line_start;
    for (const char *line_start = start; line_start != end;
         line_start = next_line_start) {
        int val_idx;
        struct setting_meta *meta;
        const char *newline = (char *)memchr(line_start, '\n', (size_t)(end - line_start));
        next_line_start = newline ? (newline + 1) : end;
        const char *line_end = newline ? newline : end;
        const char *pound = (char *)memchr(line_start, '#', (size_t)(line_end - line_start));
        if (pound)
            line_end = pound;
        trim(&line_start, &line_end, line_start, line_end);
        if (line_start == line_end)
            continue;
        const char *equals = (char *)memchr(line_start, '=', (size_t)(line_end - line_start));
        if (!equals)
            goto bad_line;
        const char *key_start, *key_end;
        const char *val_start, *val_end;
        trim(&key_start, &key_end, line_start, equals);
        trim(&val_start, &val_end, equals + 1, line_end);
        size_t setting_idx;
        for (setting_idx = 0; setting_idx < NUM_SETTINGS; setting_idx++) {
            meta = &setting_meta[setting_idx];
            if (str_eq_mem(meta->serialized_name, key_start, key_end))
                goto found_key;
        }
        log("could not find setting named '%.*s', from line '%.*s'\n",
            (int)(key_end - key_start), key_start, (int)(line_end - line_start),
            line_start);
        continue;
    found_key:
        for (val_idx = 0; val_idx < meta->values->count; val_idx++) {
            if (str_eq_mem(meta->values->serialized_values[val_idx], val_start, val_end))
                goto found_val;
        }
        log("could not find value named '%.*s', from line '%.*s'\n",
            (int)(val_end - val_start), val_start, (int)(line_end - line_start),
            line_start);
        continue;
    found_val:
        setting_vals[setting_idx] = val_idx;
        log("parsed OK: '%.*s'\n", (int)(line_end - line_start), line_start);
        continue;
    bad_line:
        log("bad line: '%.*s'\n", (int)(line_end - line_start), line_start);
        continue;
    }
}

static size_t
serialize_settings(char *start, size_t cap, const int setting_vals[NUM_SETTINGS]) {
    char *end = start + cap;
    char *ptr = start;
    for (size_t setting_idx = 0; setting_idx < NUM_SETTINGS; setting_idx++) {
        int val = setting_vals[setting_idx];
        ensure(usprintf(&ptr, end, "%s = %s\n", setting_meta[setting_idx].serialized_name,
                        setting_meta[setting_idx].values->serialized_values[val]));
    }
    return (size_t)(ptr - start);
}

#if SETTINGSTEST
int
main(int argc, char **argv) {
    int setting_vals[NUM_SETTINGS] = {0};
    if (argc <= 1) {
        char tmp[4096];
        memset(tmp, 0xdd, sizeof(tmp));
        size_t len = fread(tmp, 1, sizeof(tmp) - 1, stdin);
        parse_settings(tmp, len, setting_vals);
        for (int i = 0; i < NUM_SETTINGS; i++)
            printf("setting_vals[%d] = %d\n", i, setting_vals[i]);
    } else {
        for (int i = 0; i < argc - 1; i++) {
            ensure(i < NUM_SETTINGS);
            int val = atoi(argv[i + 1]);
            ensure(val >= 0 && val < setting_meta[i].values->count);
            setting_vals[i] = val;
        }
    }
    char out[4096];
    size_t len = serialize_settings(out, sizeof(out), setting_vals);
    fwrite(out, 1, len, stdout);
    return 0;
}

#endif
