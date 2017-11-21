#define WANT_CALLBACK 1
#include "uifront.h"
#include "uiback.h"
#include "decls.h"
#include "containers.h"
#include "logging.h"
#include "graphics.h"
#include "misc.h"
#include "font.h"

BEGIN_LOCAL_DECLS

#define un(x) unwrap_or(x, goto bad)

struct qmod_ui {
    struct heap *heap;
    struct ui_widget *root;
    struct ui_pages *pages;
};

static uint32_t
get_repeatable_action(uint32_t *repeat_timer_p, struct ui_focus_info *ufi, uint32_t bits) {
    uint32_t trigger = 0, held = 0;
    if (ufi) {
        ufi->capture |= bits;
        trigger = ufi->trigger & bits;
        held = ufi->held & bits;
    }
    if (trigger) {
        *repeat_timer_p = 21u;
        return trigger;
    } else if (held) {
        if (*repeat_timer_p != -1u && --*repeat_timer_p == 0u) {
            *repeat_timer_p = 10u;
            return held;
        } else {
            return 0;
        }
    } else {
        *repeat_timer_p = -1u;
        return 0;
    }
}

struct ui_rowpicker : public ui_widget {
    static maybe<struct ui_rowpicker *> make(struct ui_widget *parent);

    void update(struct ui_focus_info *ufi) override;
    maybe<struct ui_widget *> new_row();

    struct ui_widget *stack_;
    size_t cur_row_;
    uint32_t repeat_timer_;
    uint32_t switch_timer_;
};

void
ui_rowpicker::update(struct ui_focus_info *ufi) {
    uint32_t action = get_repeatable_action(&repeat_timer_, ufi, 0x300);
    size_t num_rows = stack_->kids_.count();
    if (!num_rows)
        return;
    size_t cur_row = cur_row_;
    if (cur_row >= num_rows)
        cur_row = num_rows - 1;
    if (action)
        log("action=%x\n", action);
    if (action & 0x100) {
        cur_row = (cur_row + 1) % num_rows;
        switch_timer_ = 0;
    } else if (action & 0x200) {
        cur_row = (cur_row + num_rows - 1) % num_rows;
        switch_timer_ = 0;
    }
    // 0x400 = right, 0x800 = left
    cur_row_ = cur_row;

    uint32_t switch_timer = switch_timer_++;

    coord_t top = 3, bottom = 3;
    size_t i = 0;
    for (struct ui_widget *wpad : stack_->kids_) {
        bool is_cur_row = i++ == cur_row;
        struct ui_bgcolor *bgcolor = downcast<ui_bgcolor>(wpad->kids_.only());
        uint8_t alpha = 0;
        struct ui_padding *pad = downcast<ui_padding>(wpad);
        pad->tl_.h = top;
        pad->br_.h = bottom;
        if (is_cur_row) {
            pad->tl_.h += 1;
            pad->br_.h -= 1;
            float val = fixed_sin((uint8_t)(int)(1.5f*switch_timer) + 0x40) / 127.f;
            //val = __builtin_copysignf(__builtin_sqrtf(__builtin_fabsf(val)), val);
            //val = __builtin_copysignf(val * val, val);
            float threshold = 0.9f;
            if (val < -threshold)
                val = -threshold;
            if (val > threshold)
                val = threshold;
            val *= (1.0f / threshold);
            val *= 10;
            uint32_t begin = 17;
            if (switch_timer < begin) {
                val += 50 * fixed_sin((uint8_t)(0x40 + switch_timer * 0x40 / begin)) / 127;
            }
            alpha = (uint8_t)(100.5 + val);
            //log("alpha=%df\n", alpha);
        }
        bgcolor->alpha_ = alpha;
        wpad->update(is_cur_row ? ufi : nullptr);
    }
}

maybe<struct ui_widget *>
ui_rowpicker::new_row() {
    struct ui_widget *pad = unwrap_or(ui_padding::make(stack_, (struct size){0}, (struct size){0}), return nothing);
    return ui_bgcolor::make(pad, COLOR_WHITE, /*alpha*/ 0);
}

maybe<struct ui_rowpicker *>
ui_rowpicker::make(struct ui_widget *parent) {
    {
        struct ui_rowpicker *self = un(ui_widget::make_a<ui_rowpicker>(parent));
        struct ui_widget *scroller = un(ui_scroller::make(self, UD_Y));
        struct ui_widget *stack = un(ui_stack::make(scroller, UD_Y, USL_COMPACT));

        self->stack_ = stack;
        self->switch_timer_ = 0xc0;

        return just(self);
    }
bad:
    return nothing;
}

struct ui_page : public ui_widget {
    static maybe<struct ui_page *> make(struct ui_widget *parent);

    void do_layout(const struct rect *restrict rect, const struct rect *restrict clip_rect, const struct size *restrict min_size) override;
    void do_draw(struct renderer *restrict r) override;

    struct rect calced_clip_rect_;
};

maybe<struct ui_page *>
ui_page::make(struct ui_widget *parent) {
    return ui_widget::make_a<ui_page>(parent);
}

void
ui_page::do_layout(const struct rect *restrict rect,
               const struct rect *restrict clip_rect,
                       const struct size *min_size) {
    //log("ui_page_layout(%p): x=%d w=%d\n", w, clip_rect->pos.x, clip_rect->size.w);
    calced_clip_rect_ = *clip_rect;
    ui_widget::do_layout(rect, clip_rect, min_size);
}

void
ui_page::do_draw(struct renderer *restrict r) {
    struct rect old = renderer_set_clip(r, calced_clip_rect_);
    ui_widget::do_draw(r);
    renderer_set_clip(r, old);
}

struct ui_pages : public ui_widget {
    static maybe<struct ui_pages *> make(struct ui_widget *parent);
    void push_page(struct ui_page *page);
    void pop_page(struct ui_page *page);

    void update(struct ui_focus_info *ufi) override;
    void calc_min_size() override;
    void do_layout(const struct rect *restrict rect, const struct rect *restrict clip_rect, const struct size *restrict min_size) override;
    //void do_draw(struct renderer *restrict r) override;
    size_t cur_page_idx_;
    struct transition slide_transition_;

    struct ui_bgcolor *bgcolor_;
    struct ui_widget *attach_;
};

void
ui_pages::update(struct ui_focus_info *ufi) {
    transition_update(&slide_transition_);
    if (attach_->kids_.count())
        attach_->kids_[cur_page_idx_]->update(ufi);
}

void
ui_pages::push_page(struct ui_page *page) {
    // 'page' must have already been allocated on attach_
    struct ui_widget *attach = attach_;
    size_t old_pages_count = attach->kids_.count() - 1;
    ensure(attach->kids_[old_pages_count] == page);
    if (old_pages_count == 0)
        return;
    while (cur_page_idx_ + 1 < old_pages_count) {
        //log("force-removing page %zu\n", old_pages_count - 1);
        delete attach->remove_kid(--old_pages_count);
    }
    struct ui_widget *cur_page = attach->kids_[cur_page_idx_];
    float new_target = slide_transition_.target + cur_page->calced_rect_.size.w;
    log("target: %f -> %f\n", slide_transition_.target, new_target);
    transition_start(&slide_transition_, TT_EASE_OUT, 15, new_target);
    cur_page_idx_++;
    page->update(nullptr);
}

void
ui_pages::pop_page(struct ui_page *page) {
    ensure(cur_page_idx_ < attach_->kids_.count() &&
           page == attach_->kids_[cur_page_idx_]);
    ensure(cur_page_idx_ > 0);
    cur_page_idx_--;
    struct ui_widget *prev_page = attach_->kids_[cur_page_idx_];
    float new_target = slide_transition_.target - prev_page->calced_rect_.size.w;
    transition_start(&slide_transition_, TT_EASE_OUT, 15, new_target);
}

void
ui_pages::calc_min_size() {
    if (attach_->kids_.count()) {
        struct ui_widget *cur_page = attach_->kids_[cur_page_idx_];
        cur_page->calc_min_size();
        calced_rect_.size = cur_page->calced_rect_.size;
    } else
        calced_rect_.size = (struct size){0, 0};
}

void
ui_pages::do_layout(const struct rect *restrict rect,
                 const struct rect *restrict clip_rect,
                       const struct size *min_size) {
    coord_t x_off = 0;
    coord_t slide_x = (coord_t)(slide_transition_.last + 0.5f);
    coord_t my_width = rect->size.w;
    attach_->layout_nokids(rect, clip_rect);
    uarray<struct ui_widget *> &kids = attach_->kids_;
    for (size_t i = 0, count = kids.count(); i < count; i++) {
        if (x_off >= slide_x + my_width &&
            i > cur_page_idx_) {
            while (count > i) {
                //log("removing page %zu due to being offscren\n", count - 1);
                delete attach_->remove_kid(--count);
            }
            break;
        }
        struct ui_widget *kid = kids[i];

        struct rect new_rect = kid->calced_rect_;
        new_rect.pos.x = rect->pos.x + x_off - slide_x;
        struct rect kid_clip = rect_intersect(rect, &new_rect);
        if (i != cur_page_idx_)
            kid->calc_min_size();
        kid->layout(&new_rect, &kid_clip);
        x_off += new_rect.size.w;
    }
}

maybe<struct ui_pages *>
ui_pages::make(struct ui_widget *parent) {
    {
        struct ui_pages *self = un(ui_widget::make_a<ui_pages>(parent));
        self->bgcolor_ = un(ui_bgcolor::make(self, COLOR_BLACK, /*alpha*/ 240));
        self->attach_ = self->bgcolor_;
        transition_reset(&self->slide_transition_, 0);
        return just(self);
    }
bad:
    return nothing;
}

struct ui_submenu_link : public ui_widget {
    using cb_t = callback<void(void)>;
    static maybe<struct ui_submenu_link *> make(struct ui_widget *parent, const char *text, bool free_text, cb_t callback);
    void update(struct ui_focus_info *ufi) override;

    struct ui_widget *label_, *arrow_;
    cb_t callback_;
};

maybe<struct ui_submenu_link *>
ui_submenu_link::make(struct ui_widget *parent, const char *text, bool free_text, cb_t callback) {
    if (0) { bad: return nothing; }
    struct ui_submenu_link *self = un(ui_widget::make_a<ui_submenu_link>(parent));
    self->callback_ = move(callback);

    struct size pad_size = {3, 1};
    struct ui_widget *padding = un(ui_padding::make(self, pad_size, pad_size));
    struct ui_widget *stack = un(ui_stack::make(padding, UD_X, USL_COMPACT));
    self->label_ = un(ui_text::make(stack, text, free_text, COLOR_WHITE, 0, COORD_MAX, UA_LEFT));
    self->label_->takes_slack_ = true;
    coord_t arrow_width = 15;
    self->arrow_ = un(ui_text::make(stack, STR_CONTROL_RIGHT, false, COLOR_WHITE, arrow_width, arrow_width, UA_RIGHT));

    return just(self);
}

void
ui_submenu_link::update(struct ui_focus_info *ufi) {
    if (ufi) {
        ufi->capture |= 0x400;
        if (ufi->trigger & 0x400) {
            callback_();
        }
    }
}

struct ui_test_row : public ui_widget {
    static maybe<struct ui_test_row *> make(struct ui_widget *parent, struct ui_test_page *test_page, size_t i);

    void update(struct ui_focus_info *ufi) override;

    struct ui_test_page *test_page_;
    struct ui_text *left_, *right_;
    int i_;
    int frame_;
    uint32_t repeat_timer_;
};


struct ui_test_page : public ui_widget {
    static maybe<struct ui_test_page *> make(struct ui_widget *parent);
    struct ui_page *page_;
    struct ui_rowpicker *rowpicker_;
};

void
ui_test_row::update(struct ui_focus_info *ufi) {
    struct ui_text *utext = left_;
    if (utext->free_text_)
        heap_free(heap_, (char *)utext->text_);
    char *text = (char *)heap_alloc(heap_, 64).unwrap();
    snprintf(text, 64, "row %u %u", i_, frame_ += i_);
    utext->text_ = text;
    utext->free_text_ = true;
    utext->color_ = ufi ? COLOR_YELLOW : COLOR_WHITE;
    if (ufi) {
        uint32_t action = get_repeatable_action(&repeat_timer_, ufi, 0xc00);
        struct ui_pages *pages = downcast<ui_pages>(test_page_->parent_->parent_->parent_);
        if (action & 0x400) {
            struct ui_test_page *new_page = unwrap_or(ui_test_page::make(pages->attach_), panic("?"));
            pages->push_page(new_page->page_);//, !!(action & 0x400));
            log("pushed page\n");
        } else if (action & 0x800) {
            pages->pop_page(downcast<ui_page>(test_page_->parent_));
            log("popped page\n");
        }
    }
}

maybe<struct ui_test_row *>
ui_test_row::make(struct ui_widget *parent, struct ui_test_page *test_page, size_t i) {
    {
        struct ui_test_row *self = un(ui_widget::make_a<ui_test_row>(parent));

        self->test_page_ = test_page;
        self->i_ = (int)i;
        struct ui_widget *hstack
            = un(ui_stack::make(self, UD_X, USL_BALANCE_WITH_SIBLINGS));
        struct ui_widget *pad = un(ui_padding::make(
            hstack, (struct size){(coord_t)(5 * i), 5}, (struct size){1, 1}));
        //char *buf = nullptr;
        //asprintf(&buf, "Hello %d", i);
        self->left_ = un(ui_text::make(pad, "init", false, COLOR_YELLOW, 0, 1000, UA_LEFT));
        self->right_ = un(ui_text::make(hstack, "This is some very long text", false,
                                          (i & 1) ? COLOR_GREEN : COLOR_BLUE, 0, 80, UA_CENTER));

        return just(self);
    }
bad:
    return nothing;
}

maybe<struct ui_test_page *>
ui_test_page::make(struct ui_widget *parent) {
    {
        struct ui_page *page = un(ui_page::make(parent));
        struct ui_test_page *self = un(ui_widget::make_a<ui_test_page>(page));
        self->page_ = page;
        size_t num_rows = 5;

        struct ui_widget *toppad
            = un(ui_padding::make(self, (struct size){5, 5}, (struct size){5, 5}));
        struct ui_rowpicker *rowp = un(ui_rowpicker::make(toppad));

        for (size_t i = 0; i < num_rows; i++) {
            un(ui_test_row::make(un(rowp->new_row()), self, i));
        }

        un(ui_submenu_link::make(un(rowp->new_row()), "Pon", false, []() {
            log("pon\n");
        }));

        self->update(nullptr);

        return just(self);
    }
bad:
    return nothing;
}

/*
void
qmod_ui_set_setting_vals(struct qmod_ui *ui, int (*setting_vals)[NUM_SETTINGS]) {
    ui->setting_vals = setting_vals;
}
*/

maybe<struct qmod_ui *>
qmod_ui_alloc(struct heap *heap) {
    struct qmod_ui *ui = (struct qmod_ui *)unwrap_or(heap_zalloc(heap, sizeof(*ui)), return nothing);
    {
        ui->heap = heap;
        ui->root = nullptr;

        struct ui_widget *root = (struct ui_widget *)un(heap_zalloc(heap, sizeof(*root)));
        new (root) ui_widget();
        root->parent_ = nullptr;
        root->heap_ = heap;

        ui->root = root;
        ui->pages = un(ui_pages::make(root));
        struct ui_test_page *first_page = un(ui_test_page::make(ui->pages->attach_));
        ui->pages->push_page(first_page->page_);

        return just(ui);
    }
bad:
    qmod_ui_free(ui);
    return nothing;
}

void
qmod_ui_free(struct qmod_ui *ui) {
    if (ui->root)
        delete ui->root;
}

void
qmod_ui_update(struct qmod_ui *ui, struct ui_focus_info *ufi) {
    struct ui_widget *root = ui->root;
    root->update(ufi);
}

void
qmod_ui_draw(struct qmod_ui *ui, struct graphics_state *gstate,
        struct GX2ColorBuffer *buffer) {
    struct ui_widget *root = ui->root;
    root->calc_min_size();
    struct rect big = {{0, 0}, root->calced_rect_.size};
    root->layout(&big, &big);
    struct renderer *r = renderer_start(gstate, ui->heap);
    renderer_set_clip(r, big);
    root->draw(r);
    renderer_finish(r, buffer);
    renderer_sync_and_free(r);
}

#if UITEST
#include <SDL.h>
static uint32_t
keycode_to_flag(SDL_Keycode kc) {
    switch (kc) {
    case SDLK_s:
        return 0x100;
    case SDLK_w:
        return 0x200;
    case SDLK_d:
        return 0x400;
    case SDLK_a:
        return 0x800;
    case SDLK_1:
        return 0x80;
    case SDLK_2:
        return 0x20;
    case SDLK_3:
        return 0x40;
    case SDLK_4:
        return 0x80;
    case SDLK_i:
        return 0x2000;
    case SDLK_j:
        return 0x1000;
    case SDLK_o:
        return 0x8000;
    case SDLK_k:
        return 0x4000;
    default:
        return 0;
    }
}

int
main() {
    struct graphics_state *graphics_state = graphics_onetime_init();
    uint32_t held = 0;
    struct timespec last_timespec;
    ensure(!clock_gettime(CLOCK_MONOTONIC, &last_timespec));
    struct heap heap;
    heap_init(&heap);
    struct qmod_ui *ui = unwrap_or(qmod_ui_alloc(&heap), panic("no ui"));
    uint32_t capture = 0;
    size_t num_frames = 0;
    while (1) {
        struct timespec timespec;
        ensure(!clock_gettime(CLOCK_MONOTONIC, &timespec));
        long diff_ns = (timespec.tv_sec - last_timespec.tv_sec) * 1000000000l + (timespec.tv_nsec - last_timespec.tv_nsec);
        long one_frame_ns = 1000000000l / 60 - 1000000l;
        ensure(diff_ns >= 0);
        if (diff_ns < one_frame_ns) {
            struct timespec wait = {.tv_nsec = one_frame_ns - diff_ns};
            //log("waiting %ld ns\n", wait.tv_nsec);
            nanosleep(&wait, nullptr);
            ensure(!clock_gettime(CLOCK_MONOTONIC, &timespec));
        }
        //usleep(200000);
        if (timespec.tv_sec != last_timespec.tv_sec) {
            log("%zu FPS\n", num_frames);
            num_frames = 0;
        }
        num_frames++;
        last_timespec = timespec;

        SDL_Event event;
        uint32_t trigger = 0;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                if (event.key.keysym.sym == SDLK_q) {
                    qmod_ui_free(ui);
                    return 0;
                }
                uint32_t flag = keycode_to_flag(event.key.keysym.sym);
                log("keydown %x <- %x\n", flag, event.key.keysym.sym);
                trigger |= flag;
                held |= flag;
            } else if (event.type == SDL_KEYUP && !event.key.repeat) {
                uint32_t flag = keycode_to_flag(event.key.keysym.sym);
                log("keyup %x\n", flag);
                held &= ~flag;
            }
        }
        held &= capture;
        trigger &= capture;
        if (trigger)
            log("trigger %x\n", trigger);
        struct ui_focus_info ufi = {.trigger = trigger, .held = held, .capture = 0};
        qmod_ui_update(ui, &ufi);
        qmod_ui_draw(ui, graphics_state, nullptr);
        capture = ufi.capture;
    }
}
#endif

END_LOCAL_DECLS
