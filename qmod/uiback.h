#pragma once
#include "containers.h"
#include "graphics.h"
#include "misc.h"
#include "types.h"

BEGIN_LOCAL_DECLS

struct rect rect_intersect(const struct rect *a, const struct rect *b);

enum transition_type {
    TT_LINEAR = 0,
    TT_EASE_IN = 1,
    TT_EASE_OUT = 2,
    //TT_BUMP,
};

struct transition {
    uint8_t transition_flags;
    uint8_t frame_count_left;
    uint8_t ease_in_frames_left, ease_out_frames_left;
    float velocity;
    float target_velocity;
    float target, last;
};

void transition_reset(struct transition *t, int val);
void transition_start(struct transition *t, int transition_flags,
                      uint8_t frame_count, float target);
static inline bool
transition_active(const struct transition *t) {
    return t->frame_count_left != 0;
}

float transition_update(struct transition *t);

enum ui_dimension {
    UD_X = 0,
    UD_Y = 1,
};

static const coord_t ARROWBAR_LENGTH = 14;

enum ui_stack_layout {
    USL_COMPACT,
    USL_PROPORTIONAL,
    USL_EQUAL,
    USL_BALANCE_WITH_SIBLINGS,
};

enum ui_align {
    UA_LEFT,
    UA_CENTER,
    UA_RIGHT,
};

struct ui_focus_info;

struct ui_widget {
    virtual void update(struct ui_focus_info *ufi);
    virtual void calc_min_size();
    virtual void do_layout(const struct rect *restrict rect, const struct rect *restrict clip_rect, const struct size *restrict min_size);
    virtual void do_draw(struct renderer *restrict r);

    void layout(const struct rect *restrict rect, const struct rect *restrict clip_rect);
    void layout_nokids(const struct rect *restrict rect, const struct rect *restrict clip_rect);
    void draw(struct renderer *restrict r);

    struct ui_widget *remove_kid(size_t i);

    // TODO: SDKCALL is only necessary because this is dumb 
    virtual ~ui_widget();
    void operator delete(void *ptr);

    template <typename ui_foo>
    static inline maybe<ui_foo *>
    make_a(struct ui_widget *parent) {
        struct heap *heap = parent->heap_;
        ui_foo *self = (ui_foo *)unwrap_or(heap_alloc(heap, sizeof(ui_foo)), return nothing);
        new (self) ui_foo();
        self->heap_ = heap;
        if (!parent->kids_.append(self, parent->heap_)) {
            delete self;
            return nothing;
        }
        self->parent_ = parent;
        return just(self);
    }

    struct ui_widget *parent_ = nullptr;
    struct heap *heap_;
    uarray<struct ui_widget *> kids_;
    bool takes_slack_ = false;
    bool calced_visible_;

    struct rect calced_rect_;
    struct ui_widget *calced_first_balancewithsiblings_kid_;
};

struct ui_text : public ui_widget {
    void calc_min_size() override;
    void do_layout(const struct rect *restrict rect, const struct rect *restrict clip_rect, const struct size *restrict min_size) override;
    void do_draw(struct renderer *restrict r) override;

    ~ui_text() override;

    static maybe<ui_text *>
    make(struct ui_widget *parent, const char *text, bool free_text, enum color color, coord_t min_width, coord_t max_width, enum ui_align align) {
        ui_text *self = unwrap_or(ui_widget::make_a<ui_text>(parent), return nothing);
        self->text_ = text;
        self->free_text_ = free_text;
        self->color_ = color;
        self->min_width_ = min_width;
        self->max_width_ = max_width;
        self->align_ = align;
        return just(self);
    }

    const char *text_;
    bool free_text_;
    enum color color_;
    coord_t min_width_, max_width_;
    enum ui_align align_;
    coord_t calced_width_;
};

struct ui_bgcolor : public ui_widget {
    static maybe<ui_bgcolor *>
    make(struct ui_widget *parent, enum color color, uint8_t alpha) {
        ui_bgcolor *self = unwrap_or(ui_widget::make_a<ui_bgcolor>(parent), return nothing);
        self->color_ = color;
        self->alpha_ = alpha;
        return just(self);
    }

    void do_draw(struct renderer *restrict r) override;

    enum color color_;
    uint8_t alpha_;
};

struct ui_padding : public ui_widget {
    static maybe<ui_padding *>
    make(struct ui_widget *parent, struct size tl, struct size br) {
        ui_padding *self = unwrap_or(ui_widget::make_a<ui_padding>(parent), return nothing);
        self->tl_ = tl;
        self->br_ = br;
        return just(self);
    }

    void calc_min_size() override;
    void do_layout(const struct rect *restrict rect, const struct rect *restrict clip_rect, const struct size *restrict min_size) override;

    struct size tl_, br_;
};

struct ui_overlay : public ui_widget {
    static maybe<ui_overlay *>
    make(struct ui_widget *parent) {
        return ui_widget::make_a<ui_overlay>(parent);
    }

    void calc_min_size() override;
    void do_layout(const struct rect *restrict rect, const struct rect *restrict clip_rect, const struct size *restrict min_size) override;
};

struct ui_stack : public ui_widget {
    static maybe<ui_stack *>
    make(struct ui_widget *parent, enum ui_dimension dimension, enum ui_stack_layout layout) {
        ui_stack *self = unwrap_or(ui_widget::make_a<ui_stack>(parent), return nothing);
        self->dimension_ = dimension;
        self->layout_ = layout;
        return just(self);
    }

    void calc_min_size() override;
    void do_layout(const struct rect *restrict rect, const struct rect *restrict clip_rect, const struct size *restrict min_size) override;

    enum ui_dimension dimension_;
    enum ui_stack_layout layout_;
};

struct ui_scroller : public ui_widget {
    static maybe<ui_scroller *>
    make(struct ui_widget *parent, enum ui_dimension dimension) {
        ui_scroller *self = unwrap_or(ui_widget::make_a<ui_scroller>(parent), return nothing);
        self->dimension_ = dimension;
        self->scroll_pos_ = 0;
        return just(self);
    }

    void calc_min_size() override;
    void do_layout(const struct rect *restrict rect, const struct rect *restrict clip_rect, const struct size *restrict min_size) override;
    void do_draw(struct renderer *restrict r) override;

    enum ui_dimension dimension_;
    coord_t scroll_pos_;
    coord_t calced_max_scroll_pos_;
    struct rect calced_clip_rect_;
};

END_LOCAL_DECLS
