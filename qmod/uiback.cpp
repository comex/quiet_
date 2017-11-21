#include "uiback.h"
#include "decls.h"
#include "font.h"
#include "logging.h"
#include "graphics.h"
#include "misc.h"
#include "containers.h"

#if DUMMY
#include <SDL.h>
#include <typeinfo>
#endif

BEGIN_LOCAL_DECLS

#define UI_WIDGET_DEBUG 0
#if UI_WIDGET_DEBUG
static int g_debug_depth;
static void
debug_depth_inc(void) {
    g_debug_depth++;
}
static void
debug_depth_dec(void) {
    g_debug_depth--;
}
static const char *
debug_depth_indent(void) {
    static char buf[128];
    size_t depth = min((size_t)g_debug_depth, sizeof(buf) - 1);
    memset(buf, ' ', depth);
    buf[depth] = '\0';
    return buf;
}
static const char *
debug_class_name(ui_widget *w) {
    return typeid(*w).name();
}
#else
static void
debug_depth_inc(void) {}
static void
debug_depth_dec(void) {}
extern const char *debug_depth_indent(void);
extern const char *debug_class_name(ui_widget *w);
#endif

struct rect
rect_intersect(const struct rect *a, const struct rect *b) {
    struct pos tl = {max(a->pos.x, b->pos.x), max(a->pos.y, b->pos.y)};
    struct pos br = {(coord_t)min(a->pos.x + a->size.w, b->pos.x + b->size.w),
                     (coord_t)min(a->pos.y + a->size.h, b->pos.y + b->size.h)};
    return (struct rect){tl.x, tl.y, (coord_t)max(br.x - tl.x, 0),
                         (coord_t)max(br.y - tl.y, 0)};
}

void
transition_reset(struct transition *t, int val) {
    t->target = t->last = val;
    t->frame_count_left = 0;
}

void
transition_start(struct transition *t, int transition_flags, uint8_t frame_count,
                 float target) {
    log("transition_start: target=%f (last=%f)\n", target, t->last);
    t->transition_flags = (uint8_t)transition_flags;
    t->target = target;
    float base = t->last;
    uint8_t frames_left = frame_count;
    if ((t->velocity < 0) != (target < base))
        t->velocity = 0;
    uint8_t ease_in_frames = 0, ease_out_frames = 0;
    if (transition_flags & TT_EASE_IN) {
        ease_in_frames = min(frames_left, (uint8_t)5);
        frames_left -= ease_in_frames;
    }
    if (transition_flags & TT_EASE_OUT) {
        ease_out_frames = min(frames_left, (uint8_t)5);
        frames_left -= ease_out_frames;
    }
    // ease_in_frames * (cur_velo + target_velo)/2 +
    // ease_out_frames * (target_velo + 0)/2 +
    // frames_left * target_velo
    // = target - base
    float dividend = 2 * (target - base) - (ease_in_frames * t->velocity);
    float divisor = ease_in_frames + ease_out_frames + 2 * frames_left;
    t->target_velocity = (divisor != 0.0f) ? (dividend / divisor) : 0.0f;
    t->frame_count_left = frame_count;
    t->ease_in_frames_left = ease_in_frames;
    t->ease_out_frames_left = ease_out_frames;
}

float
transition_update(struct transition *t) {
    float newval;
    if (t->frame_count_left == 0) {
        newval = t->target;
    } else {
        if (t->ease_in_frames_left) {
            t->velocity += (t->target_velocity - t->velocity) / t->ease_in_frames_left--;
        } else if (t->ease_out_frames_left && t->frame_count_left <= t->ease_out_frames_left) {
            t->velocity += (0.0f - t->velocity) / t->ease_out_frames_left--;
        } else {
            t->velocity = t->target_velocity;
        }
        newval = t->last + t->velocity;
        if ((newval > t->target) != (t->last > t->target))
            newval = t->target;
        t->last = newval;
        t->frame_count_left--;
        //log("transition_update: velo=%f(->%f) newval=%f\n", t->velocity, t->target_velocity, newval);
    }
    t->last = newval;
    return newval;
}

static struct size
draw_text(const char *text, enum color color, struct pos base_pos, coord_t max_width,
          struct renderer *r) {
    const char *textp = text ?: "";
    coord_t calced_width = 0;
    coord_t yoff = 0;
    while (*textp != '\0') { // line loop
        coord_t xoff = 0;
        while (*textp != '\0' && *textp != '\n') { // word loop
            if (xoff > 0) {
                const char *lookahead_textp = textp;
                coord_t lookahead_xoff = xoff;
                // check if it fits
                while (1) { // word loop (lookahead)
                    char c = *lookahead_textp++;
                    if (c == '\n' || c == ' ' || c == '\0')
                        break;
                    uint8_t idx = c >= sizeof(font_mapping) ? 0 : font_mapping[c];
                    coord_t width = font_data[idx].width;
                    lookahead_xoff += width;
                    if (lookahead_xoff > max_width)
                        goto end_line;
                }
            }

            while (*textp != '\0') {
                char c = *textp++;
                if (c == '\n')
                    break;
                uint8_t idx = c >= sizeof(font_mapping) ? 0 : font_mapping[c];
                coord_t width = font_data[idx].width;
                if (r)
                    renderer_draw_char(r,
                                       (struct pos){(coord_t)(base_pos.x + xoff),
                                                    (coord_t)(base_pos.y + yoff)},
                                       idx, color);
                xoff += width;
                if (c == ' ')
                    break;
            }
        }
    end_line:
        calced_width = max(calced_width, xoff);
        yoff += FONT_HEIGHT;
        while (*textp == ' ')
            textp++;
    }
    return (struct size){calced_width, yoff};
}

void
ui_widget::calc_min_size() {
    if (kids_.count()) {
        struct ui_widget *kid = kids_.only();
        kid->calc_min_size();
        calced_rect_.size = kid->calced_rect_.size;
    } else
        calced_rect_.size = (struct size){0, 0};
}

void
ui_text::calc_min_size() {
    struct size calced
        = draw_text(text_, color_, (struct pos){0, 0}, max_width_, nullptr);
    calced_width_ = calced.w;
    calced.w = max(calced.w, min_width_);
    calced_rect_.size = calced;
}

void
ui_padding::calc_min_size() {
    struct ui_widget *kid = kids_.only();
    kid->calc_min_size();
    struct size ret = kid->calced_rect_.size;
    ret.w += tl_.w + br_.w;
    ret.h += tl_.h + br_.h;
    calced_rect_.size = ret;
}

void
ui_overlay::calc_min_size() {
    struct size ret = {0, 0};
    for (struct ui_widget *kid : kids_) {
        kid->calc_min_size();
        struct size kid_min = kid->calced_rect_.size;
        ret.w = max(ret.w, kid_min.w);
        ret.h = max(ret.h, kid_min.h);
    }
    calced_rect_.size = ret;
}

void
ui_stack::calc_min_size() {
    enum ui_dimension d = dimension_;
    struct size ret = {0, 0};
    for (struct ui_widget *kid : kids_) {
        kid->calc_min_size();
        struct size theirs = kid->calced_rect_.size;
        ret.wh[d] += theirs.wh[d];
        ret.wh[1 - d] = max(ret.wh[1 - d], theirs.wh[1 - d]);
    }
    calced_rect_.size = ret;
}

void
ui_scroller::calc_min_size() {
    struct ui_widget *kid = kids_.only();
    kid->calc_min_size();
    struct size ret = kid->calced_rect_.size;
    //ret.wh[dimension_] = 1;
    calced_rect_.size = ret;
}

void
ui_widget::layout_nokids(const struct rect *restrict rect,
                         const struct rect *restrict clip_rect) {
    debug_depth_inc();
    calced_rect_ = *rect;
    calced_visible_ = clip_rect->size.w != 0;
    if (UI_WIDGET_DEBUG)
        log("%s%p(%s)::lnk: rect=%d,%d,%d,%d clip_rect=%d,%d,%d,%d calced_visible=%d\n",
            debug_depth_indent(),
            this, //
            debug_class_name(this),
            rect->pos.x, rect->pos.y, rect->size.w, rect->size.h, //
            clip_rect->pos.x, clip_rect->pos.y, clip_rect->size.w, clip_rect->size.h, //
            calced_visible_);
    calced_first_balancewithsiblings_kid_ = nullptr;
    debug_depth_dec();
}

void
ui_widget::layout(const struct rect *restrict rect,
                  const struct rect *restrict clip_rect) {
    struct size min_size = calced_rect_.size;
    layout_nokids(rect, clip_rect);
    debug_depth_inc();
    if (calced_visible_)
        do_layout(rect, clip_rect, &min_size);
    debug_depth_dec();
}

void
ui_widget::do_layout(const struct rect *restrict rect,
                     const struct rect *restrict clip_rect,
                     const struct size *restrict min_size) {
    if (kids_.count())
        kids_.only()->layout(rect, clip_rect);
}

void
ui_text::do_layout(const struct rect *restrict rect,
                const struct rect *restrict clip_rect,
                const struct size *restrict min_size) {
    ui_widget::do_layout(rect, clip_rect, min_size);
    switch (align_) {
    case UA_RIGHT:
        calced_rect_.pos.x += rect->size.w - calced_width_;
        break;
    case UA_CENTER:
        calced_rect_.pos.x += (rect->size.w - calced_width_) / 2;
        break;
    case UA_LEFT:
        break;
    }
    calced_rect_.size.w = calced_width_;
}

void
ui_padding::do_layout(const struct rect *restrict rect,
                   const struct rect *restrict clip_rect,
                   const struct size *restrict min_size) {
    struct ui_widget *kid = kids_.only();
    struct rect sub_rect = {
        {(coord_t)(rect->pos.x + tl_.w), (coord_t)(rect->pos.y + tl_.h)},
        {(coord_t)(rect->size.w - tl_.w - br_.w),
         (coord_t)(rect->size.h - tl_.h - br_.h)}
    };
    kid->layout(&sub_rect, clip_rect);
}

void
ui_overlay::do_layout(const struct rect *restrict rect,
                   const struct rect *restrict clip_rect,
                   const struct size *restrict min_size) {
    for (struct ui_widget *kid : kids_)
        kid->layout(rect, clip_rect);
}

void
ui_stack::do_layout(const struct rect *restrict rect,
                const struct rect *restrict clip_rect,
                   const struct size *restrict min_size) {
    enum ui_dimension d = dimension_;
    coord_t loc = rect->pos.xy[d];
    coord_t avail_length = rect->size.wh[d];
    coord_t total_min_length = min_size->wh[d];
    struct ui_widget *first_sibling = nullptr;
    if (layout_ == USL_BALANCE_WITH_SIBLINGS) {
        first_sibling = parent_->calced_first_balancewithsiblings_kid_;
        if (first_sibling == nullptr) {
            parent_->calced_first_balancewithsiblings_kid_ = this;
            for (struct ui_widget *sibling : parent_->kids_) {
                size_t i = 0;
                for (struct ui_widget *skid : sibling->kids_) {
                    struct ui_widget *mykid = kids_[i++];
                    coord_t *mine = &mykid->calced_rect_.size.wh[d];
                    *mine = max(*mine, skid->calced_rect_.size.wh[d]);
                }
            }
            total_min_length = 0;
            for(struct ui_widget *kid : kids_)
                total_min_length += kid->calced_rect_.size.wh[d];
        }
    }

    size_t nkids = kids_.count();
    size_t i = 0;
    for (struct ui_widget *kid : kids_) {
        coord_t length;
        coord_t min_length = kid->calced_rect_.size.wh[d];
        switch (layout_) {
        case USL_BALANCE_WITH_SIBLINGS:
            if (first_sibling) {
                struct ui_widget *mykid = first_sibling->kids_[i];
                length = mykid->calced_rect_.size.wh[d];
                break;
            }
            FALLTHROUGH;
        case USL_COMPACT:
            if (kid->takes_slack_)
                length = avail_length - total_min_length + min_length;
            else
                length = min_length;
            break;
        case USL_PROPORTIONAL:
            length = avail_length * min_length / total_min_length;
            break;
        case USL_EQUAL:
            length = avail_length / (coord_t)nkids;
            break;
        }
        struct rect kid_rect;
        kid_rect.pos.xy[d] = loc;
        kid_rect.pos.xy[1 - d] = rect->pos.xy[1 - d];
        kid_rect.size.wh[d] = length;
        kid_rect.size.wh[1 - d] = rect->size.wh[1 - d];
        kid->layout(&kid_rect, clip_rect);
        loc += length;
        i++;
    }
}

void
ui_scroller::do_layout(const struct rect *restrict rect,
                       const struct rect *restrict clip_rect,
                       const struct size *min_size) {
    struct ui_widget *kid = kids_.only();
    enum ui_dimension d = dimension_;
    calced_max_scroll_pos_
        = max((coord_t)(rect->size.wh[d] - kid->calced_rect_.size.wh[d]), (coord_t)0);
    struct rect content_rect = *rect;
    if (scroll_pos_ > 0) {
        // up arrow
        content_rect.pos.xy[d] += ARROWBAR_LENGTH;
        content_rect.size.wh[d] -= ARROWBAR_LENGTH;
    }
    if (scroll_pos_ < calced_max_scroll_pos_) {
        // down arrow
        content_rect.size.wh[d] -= ARROWBAR_LENGTH;
    }
    content_rect.size.wh[d] = max(content_rect.size.wh[d], (coord_t)0);
    struct rect my_clip_rect = rect_intersect(&content_rect, clip_rect);
    struct rect inner_rect = content_rect;
    inner_rect.pos.xy[d] -= scroll_pos_;
    inner_rect.size.wh[d] = kid->calced_rect_.size.wh[d];
    calced_clip_rect_ = my_clip_rect;
    return kid->layout(&inner_rect, &my_clip_rect);
}

void
ui_widget::update(struct ui_focus_info *ufi) {
    for (struct ui_widget *kid : kids_)
        kid->update(ufi);
}

void
ui_widget::draw(struct renderer *restrict r) {
    if (UI_WIDGET_DEBUG)
        log("%s%p(%s)::draw: calced_visible=%d\n", debug_depth_indent(), this, debug_class_name(this), calced_visible_);
    debug_depth_inc();
    if (calced_visible_)
        do_draw(r);
    debug_depth_dec();
}

void
ui_widget::do_draw(struct renderer *restrict r) {
    for (struct ui_widget *kid : kids_)
        kid->draw(r);
}

void
ui_text::do_draw(struct renderer *restrict r) {
    draw_text(text_, color_, calced_rect_.pos, calced_rect_.size.w, r);
    ui_widget::do_draw(r);
}

void
ui_bgcolor::do_draw(struct renderer *restrict r) {
    if (alpha_)
        renderer_draw_rect(r, calced_rect_, color_, alpha_);
    ui_widget::do_draw(r);
}

void
ui_scroller::do_draw(struct renderer *restrict r) {
    struct rect clip_rect = renderer_set_clip(r, calced_clip_rect_);
    ui_widget::do_draw(r);
    renderer_set_clip(r, clip_rect);
}

ui_widget::~ui_widget() {
    ensure(parent_ == nullptr);
    while (size_t count = kids_.count())
        delete remove_kid(count - 1);
}
void ui_widget::operator delete(void *ptr) {
    heap_free(((ui_widget *)ptr)->heap_, ptr);
}

ui_text::~ui_text() {
    if (free_text_)
        heap_free(heap_, (char *)text_);
}

struct ui_widget *
ui_widget::remove_kid(size_t i) {
    struct ui_widget *kid = kids_.remove(i);
    kid->parent_ = nullptr;
    return kid;
}

END_LOCAL_DECLS
