#pragma once
#include "containers.h"
#include "types.h"

#define RGB_MAX 0xff
#define A_MAX 0xff
#define RGBA(r, g, b, a)                                                                 \
    ((uint32_t)(r) << 24 | (uint32_t)(g) << 16 | (uint32_t)(b) << 8 | (uint32_t)(a))
static inline uint32_t
get_r(uint32_t c) {
    return c >> 24;
}
static inline uint32_t
get_g(uint32_t c) {
    return c >> 16;
}
static inline uint32_t
get_b(uint32_t c) {
    return c >> 8;
}
static inline uint32_t
get_a(uint32_t c) {
    return c >> 0;
}

typedef int16_t coord_t;
static const coord_t COORD_MAX = 0x7fff, COORD_MIN = -0x8000;

struct pos {
    union {
        struct {
            coord_t x, y;
        };
        coord_t xy[2];
    };
};

struct size {
    union {
        struct {
            coord_t w, h;
        };
        coord_t wh[2];
    };
};

struct rect {
    struct pos pos;
    struct size size;
};

enum color {
    COLOR_BLACK,
    COLOR_WHITE,
    COLOR_YELLOW,
    COLOR_BLUE,
    COLOR_RED,
    COLOR_GREEN,
    COLOR_MENUBG,
    NUM_COLORS,
};
extern uint32_t colors[NUM_COLORS];

struct graphics_state;
struct GX2ColorBuffer;
struct renderer;

struct renderer *renderer_start(struct graphics_state *gstate,
                                struct heap *heap);
void renderer_draw_char(struct renderer *r, struct pos pos, uint8_t font_data_idx,
                        enum color color);
void renderer_draw_rect(struct renderer *r, struct rect rect, enum color color,
                        uint8_t alpha);
struct rect renderer_set_clip(struct renderer *r, struct rect rect);
void renderer_finish(struct renderer *r, struct GX2ColorBuffer *cbuf);
void renderer_sync_and_free(struct renderer *r);

struct graphics_state *graphics_onetime_init(void);
