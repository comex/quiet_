#pragma once
#include "types.h"
//#include "settings.h"
#include "uiback.h"
#include "containers.h"

BEGIN_LOCAL_DECLS

struct graphics_state;
struct GX2ColorBuffer;

struct qmod_ui;

struct ui_focus_info {
    uint32_t trigger;
    uint32_t held;
    uint32_t capture;
};

maybe<struct qmod_ui *> qmod_ui_alloc(struct heap *heap);
void qmod_ui_free(struct qmod_ui *ui);
void qmod_ui_update(struct qmod_ui *ui, struct ui_focus_info *ufi);
void qmod_ui_draw(struct qmod_ui *ui, struct graphics_state *gstate, struct GX2ColorBuffer *buffer);

END_LOCAL_DECLS
