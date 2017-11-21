// XXX check shader
// TODO:
// coin restores jump?
// luigi?

#include "logging.h"
#include "misc.h"
#include "types.h"
#include "loader.h"
#include "uiback.h"
#include "uifront.h"
#include "decls.h"
#include "containers.h"
#include "kern_garbage.h"
#include "graphics.h"
#include "mmhooks.h"
#include "qmod.h"
#include "settings.cpp"

BEGIN_LOCAL_DECLS

extern struct func_hook_info qmod_hooks_list[];

enum voice_special {
    VS_NONE,
    VS_PSWITCH,
    VS_STAR,
};

static struct state _the_state;

struct state *
get_state(void) {
    return in_right_process() ? &_the_state : NULL;
}


template<> SDKCALL int
FUNC_HOOK_TY(VPADRead)::hook(int controller, VPADStatus *buf, uint32_t length, int *err) {
    int ret = orig(controller, buf, length, err);
    struct state *state = get_state();
    if (!state || !state->ui)
        return ret;
    OSLockMutex(&state->mutex);
    uint32_t capture_bits = state->ui_input_cur_capture;
    uint32_t held = 0, trigger = 0;
    for (int i = 0; i < ret; i++) {
        VPADStatus *status = &buf[i];
        if (status->trigger)
            log("trigger[%d]=%x (held=%x capture=%x)\n", i, status->trigger, status->held,
                capture_bits);
        if (status->release)
            log("release[%d]=%x\n", i, status->release);
        held |= status->held & capture_bits;
        trigger |= status->trigger & capture_bits;
        status->held &= ~capture_bits;
        status->trigger &= ~capture_bits;
        status->release &= ~capture_bits;
    }
    state->ui_input_cur_held = held;
    state->ui_input_cur_trigger = trigger;
    //ui_handle_input(state->ui, trigger, held, &should_fake_release);
    if (0) {
        // fake release of all buttons
        for (int i = 0; i < ret; i++) {
            VPADStatus *status = &buf[i];
            status->release = status->held;
            status->held = status->trigger = 0;
        }
    }
    OSUnlockMutex(&state->mutex);
    return ret;
}

#if 0
template<> SDKCALL int
FUNC_HOOK_TY(FSOpenFile)::hook(struct fs_client *client, struct fs_cmd *cmd, const char *path,
                const char *mode, int *handle, uint32_t flags) {
    int ret = orig(client, cmd, path, mode, handle, flags);
    struct state *state = get_state();
    if (!state)
        return ret;
    log("FSOpenFile(%p, %s, %s) -> %d (%d)\n", client, path ?: "(null)", mode ?: "(null)",
        ret, handle ? *handle : -123);
    return ret;
}
#endif

template<> SDKCALL int
FUNC_HOOK_TY(FSCloseFile)::hook(struct fs_client *client, struct fs_cmd *cmd, int handle,
                 uint32_t flags) {
    int ret = orig(client, cmd, handle, flags);
    struct state *state = get_state();
    if (!state)
        return ret;
    log("FSCloseFile(%p, %d) -> %d\n", client, handle, ret);
    return ret;
    // struct state *state = get_state();
}

template<> SDKCALL void
FUNC_HOOK_TY(GX2CopyColorBufferToScanBuffer)::hook(struct GX2ColorBuffer *buffer, int target) {
    struct state *state = get_state();
    if (!state)
        return orig(buffer, target);
    if (0) {
        log("GX2CopyColorBufferToScanBuffer buffer=%p target=%d %ux%u size=%u ptr=%p "
            "format=0x%x\n",
            buffer, target, buffer->surface.width, buffer->surface.height,
            buffer->surface.imageSize, buffer->surface.image, buffer->surface.format);
        log("   tile=0x%x swizzle=0x%x align=0x%x pitch=0x%x\n", buffer->surface.tileMode,
            buffer->surface.swizzle, buffer->surface.alignment, buffer->surface.pitch);
    }
    OSLockMutex(&state->mutex);
    if (!state->initted_ui) {
        ensure(!state->graphics_state);
        ensure((state->graphics_state = graphics_onetime_init()));
        state->ui = unwrap_or(qmod_ui_alloc(&state->heap), panic("could not alloc ui"));
        state->initted_ui = true;
    }

    bool update = target == 1;
    if (update) {
        struct ui_focus_info ufi = {
            .trigger = state->ui_input_cur_trigger,
            .held = state->ui_input_cur_held,
        };
        qmod_ui_update(state->ui, &ufi);
        state->ui_input_cur_capture = ufi.capture;
    }
    qmod_ui_draw(state->ui, state->graphics_state, buffer);
    OSUnlockMutex(&state->mutex);
    orig(buffer, target);
}

static void
on_settings_loaded(struct state *state) {
    bool first_time = !state->settings_loaded;
    state->settings_loaded = true;
    if (first_time) {
        //ui_set_setting_vals(state->ui, state->setting_vals);
    }
}

static SDKCALL void save_callback(struct fs_client *client, struct fs_cmd *cmd, int status,
                          void *ctx);
static const struct async_callback save_callback_info = {save_callback, NULL, NULL};

static SDKCALL void
save_callback(struct fs_client *client, struct fs_cmd *cmd, int status, void *ctx) {
    struct state *state = get_state(); // shouldn't be null...
    OSLockMutex(&state->mutex);
    while (1) {
        int status2 = 0;
        switch (state->save_rw_state) {
        case SAVE_RW_READ_OPENING:
            if (status != 0) {
                log("failed to open _qmod.cfg for read: %d\n", status);
                on_settings_loaded(state);
                goto do_idle;
            }
            state->save_rw_state = SAVE_RW_READ_READING;
            status2
                = FSReadFileAsync(client, cmd, state->settings_txt_buf, 1,
                                  sizeof(state->settings_txt_buf) - 1,
                                  state->save_file_handle, 0, -1u, &save_callback_info);

            break;
        case SAVE_RW_READ_READING:
            if (status < 0) {
                log("failed to read _qmod.cfg: %d\n", status);
                on_settings_loaded(state);
                state->save_rw_state = SAVE_RW_READ_CLOSING;
                goto do_close;
            }
            state->settings_txt_buf_len = (size_t)status;
            state->settings_txt_buf[state->settings_txt_buf_len] = '\0';
            parse_settings(state->settings_txt_buf, state->settings_txt_buf_len,
                           state->setting_vals);
            on_settings_loaded(state);
            state->save_rw_state = SAVE_RW_READ_CLOSING;
            goto do_close;
        case SAVE_RW_READ_CLOSING:
            if (status != 0)
                log("failed to close file handle!?: %d\n", status);
            log("FS read all good\n");
            goto do_idle;
        case SAVE_RW_WRITE_OPENING:
            if (status != 0) {
                log("failed to open _qmod.cfg for WRITE: %d\n", status);
                goto do_idle;
            }
            state->save_rw_state = SAVE_RW_WRITE_WRITING;
            log("writing %u bytes to _qmod.cfg\n", (int)state->settings_txt_buf_len);
            status2 = FSWriteFileAsync(
                client, cmd, state->settings_txt_buf, 1, state->settings_txt_buf_len,
                state->save_file_handle, 0, -1u, &save_callback_info);
            break;
        case SAVE_RW_WRITE_WRITING:
            if (status != state->settings_txt_buf_len)
                log("failed to WRITE _qmod.cfg: %d\n", status);
            state->save_rw_state = SAVE_RW_WRITE_CLOSING;
            goto do_close;
        case SAVE_RW_WRITE_CLOSING:
            if (status != 0)
                log("failed to close file handle!?: %d\n", status);
            state->save_rw_state = SAVE_RW_WRITE_FLUSHING;
            status2 = SAVEFlushQuotaAsync(client, cmd, state->save_cur_slot, -1,
                                                 &save_callback_info);
            break;
        case SAVE_RW_WRITE_FLUSHING:
            if (status != 0)
                log("failed to flush quota: %d\n", status);
            else
                log("FS write all good\n");
            goto do_idle;
        do_close:
            status2 = FSCloseFileAsync(client, cmd, state->save_file_handle, -1u,
                                       &save_callback_info);
            break;
        case SAVE_RW_IDLE:
        do_idle:
            state->save_rw_state = SAVE_RW_IDLE;
            if (state->settings_need_write) {
                state->save_rw_state = SAVE_RW_WRITE_OPENING;
                state->settings_need_write = false;
                size_t len = serialize_settings(state->settings_txt_buf,
                                                sizeof(state->settings_txt_buf),
                                                state->setting_vals);
                state->settings_txt_buf_len = len;
                status2 = SAVEOpenFileAsync(
                    client, cmd, state->save_cur_slot, "_qmod.cfg", "w",
                    &state->save_file_handle, -1, &save_callback_info);
            }
            break;
        }
        if (status2) {
            status = status2;
            continue;
        }
        break;
    }
    OSUnlockMutex(&state->mutex);
}

static void
read_save(struct state *state, int slot) {
    OSLockMutex(&state->mutex);
    state->save_cur_slot = slot;
    if (state->save_rw_state != SAVE_RW_IDLE) {
        // *shrug*
        OSUnlockMutex(&state->mutex);
        return;
    }
    state->save_rw_state = SAVE_RW_READ_OPENING;
    int status = SAVEOpenFileAsync(
        &state->fs_client, &state->fs_cmd, state->save_cur_slot, "_qmod.cfg", "r",
        &state->save_file_handle, -1, &save_callback_info);
    if (status)
        save_callback(&state->fs_client, &state->fs_cmd, status, NULL);
    OSUnlockMutex(&state->mutex);
}

UNUSED static void
write_save(struct state *state) {
    OSLockMutex(&state->mutex);
    state->settings_need_write = true;
    if (state->save_rw_state == SAVE_RW_IDLE)
        save_callback(&state->fs_client, &state->fs_cmd, 0, NULL);
    OSUnlockMutex(&state->mutex);
}

template<> SDKCALL int
FUNC_HOOK_TY(SAVEInitSaveDir)::hook(int slot) {
    if (!get_state())
        return orig(slot);
    log("hook_SAVEInitSaveDir(%d)\n", slot);
    int ret = orig(slot);
    log("   ret=%d\n", ret);
    if (ret == 0) {
        struct state *state = get_state();
        read_save(state, slot);
    }
    return ret;
}

static SDKCALL void
shutdown_qmod(void) {
    uninstall_hooks(qmod_hooks_list);
}

void startup_qmod(void);
void
startup_qmod(void) {
    log("startup_qmod\n");
    log_flush();
    install_hooks(qmod_hooks_list);
    static struct at_exit my_at_exit = {shutdown_qmod};
    __ghs_at_exit(&my_at_exit);
    struct state *state = &_the_state;
    for (size_t i = 0; i < sizeof(*state); i++)
        ensure(((char *)state)[i] == 0);
    OSInitMutex(&state->mutex);
    splitter_heap_init(&state->heap);

    ensure(0 == FSAddClient(&state->fs_client, 0));
    FSInitCmdBlock(&state->fs_cmd);

    hook_mariomaker(state);
}

template<> SDKCALL void *
FUNC_HOOK_TY(MEMAllocFromExpHeapEx)::hook(void *exp_heap, size_t size, int align) {
    register uint32_t r20 asm("r20") = *(uint32_t *)((char *)orig + 8);
    register uint32_t r21 asm("r21") = 0xdeaddead;
    asm volatile("" :: "r"(r20), "r"(r21));
    void *ret = orig(exp_heap, size, align);
    if (ret == nullptr)
        panic("MEMAllocFromExpHeapEx(%p, %zu, %d) failed", exp_heap, size, align);
    return ret;
}

struct func_hook_info qmod_hooks_list[] = {
    FUNC_HOOK(MEMAllocFromExpHeapEx),
    {0}
};
static UNUSED struct func_hook_info qmod_hooks_list_disabled[] = {
    FUNC_HOOK(VPADRead),
    FUNC_HOOK(FSOpenFile),
    FUNC_HOOK(FSCloseFile),
    FUNC_HOOK(GX2CopyColorBufferToScanBuffer),
    FUNC_HOOK(SAVEInitSaveDir),
};

END_LOCAL_DECLS
