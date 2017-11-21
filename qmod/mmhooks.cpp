#define WANT_UHASH 1
#include "decls.h"
#include "logging.h"
#include "containers.h"
#include "maker.h"
#include "qmod.h"
#include "misc.h"
#include "ssscanf.h"
#include "kern_garbage.h"
#include "dynstruct.h"
#include "mmhooks.h"
#include "loader.h"

union mm_ProtocolCallContext;

#define MM_DEBUG_HOOKS 0
#define MM_GXLOG_DEBUG_HOOK 1
#define MM_TIMES10_HOOK 0
#define MM_DOWN_HOOK 1

struct AnyPtr {
    explicit AnyPtr(uintptr_t p) : p(p) {}
    template <typename T>
    inline operator T *() const { return (T *)p; }

    uintptr_t p;
};

struct mm_addrs {
    mm_addrs(uint32_t text_slide, uint32_t data_slide) : text_slide(text_slide), data_slide(data_slide) {}
    uint32_t text_slide, data_slide;
    AnyPtr slide_text(uint32_t base) { return AnyPtr(base + text_slide); }
    AnyPtr slide_data(uint32_t base) { return AnyPtr(base + data_slide); }

    void (*statemgr_set_state)(mm_statemgr *self, int state) = slide_text(0x02834280);
    bool (*Scene_should_block_fade_for_bgm)(mm_Scene *) = slide_text(0x0272D15C);
    int (*buffer_append_ex)(void *, const void *, size_t, int) = slide_text(0x02A9F838);
    mm_ProtocolCallContext *(*ctor_ProtocolCallContext)(mm_ProtocolCallContext *, int) = slide_text(0x02B19414);
    void (*dtor_ProtocolCallContext)(mm_ProtocolCallContext *, int) = slide_text(0x02B194BC);
    void (*ajit_push)(mm_ajit *self, float dt) = slide_text(0x0288FEE8);
    void (*WorldPlayKeeper_subtracts_life_from_courseloader)(mm_WorldPlayKeeper *) = slide_text(0x02778710);
    int (*sets_popular_settings)(void *, void *, int, int, int) = slide_text(0x0276B468);
    void *sets_popular_settings_retaddr = slide_text(0x0277588C);
    mm_WorldPlayKeeper **the_WorldPlayKeeper = slide_data(0x1019D124);

    int (*killer_downtimer0)(mm_killer *self) = slide_text(0x0238D4C0);
    int (*killer_downtimer1)(mm_killer *self) = slide_text(0x02389BE8);
    int (*killer_downtimer2)(mm_killer *self) = slide_text(0x0237CD6C);
    int (*killer_downtimer3)(mm_killer *self) = slide_text(0x0238EE58);
};

BEGIN_LOCAL_DECLS

static manually_construct<struct mm_addrs> g_mm_addrs;

#define MM_ADDR_HOOK(name) \
    make_fptrptr_hook(#name##_typestring, #name, (decltype(+g_mm_addrs->name) *)(g_mm_addrs.storage + offsetof(struct mm_addrs, name)))

#define MM_ADDR_HOOK_TY(name) \
    func_hook<decltype(#name##_typestring), typename remove_reference<decltype(*(g_mm_addrs->name))>::type>

#if MM_DEBUG_HOOKS
static OSMutex g_pcc_construction_time_mtx;
static uhash<mm_ProtocolCallContext *, uint64_t> g_pcc_construction_time_hash;
#endif

extern struct func_hook_info mariomaker_hooks_list[];

template<> SDKCALL int
MM_ADDR_HOOK_TY(sets_popular_settings)::hook(void *self, void *r4, int should_use_CourseIn, int flag6, int should_load) {
    //log("sps: %d %p %p\n", should_use_CourseIn, __builtin_return_address(0), g_mm_addrs->sets_popular_settings_retaddr);
    if (__builtin_return_address(0) == g_mm_addrs->sets_popular_settings_retaddr) {
        mm_WorldPlayKeeper *wpk = *g_mm_addrs->the_WorldPlayKeeper;
        //log("WPK=%p\n", wpk);
        bool ok = should_use_CourseIn && wpk;
        if (!ok) {
            log("sets_popular_settings::hook: something is wrong\n");
        } else {
            // OK
            g_mm_addrs->WorldPlayKeeper_subtracts_life_from_courseloader(wpk);
            should_use_CourseIn = false;
        }
    }
    return orig(self, r4, should_use_CourseIn, flag6, should_load);
}

static int
downtimer_hook(mm_killer *self, int (*SDKCALL orig)(mm_killer *self)) {
    if (self->timer > 1)
        self->timer = 1;
    return orig(self);
}

// clang-format off
template<> SDKCALL int MM_ADDR_HOOK_TY(killer_downtimer0)::hook(mm_killer *self) { return downtimer_hook(self, orig); }
template<> SDKCALL int MM_ADDR_HOOK_TY(killer_downtimer1)::hook(mm_killer *self) { return downtimer_hook(self, orig); }
template<> SDKCALL int MM_ADDR_HOOK_TY(killer_downtimer2)::hook(mm_killer *self) { return downtimer_hook(self, orig); }
template<> SDKCALL int MM_ADDR_HOOK_TY(killer_downtimer3)::hook(mm_killer *self) { return downtimer_hook(self, orig); }
// clang-format on

static const char *
statemgr_get_state_name(mm_statemgr *sm, int state) {
    if (state < 0 || state >= sm->state_list().count)
        return "??state_id??";
    return sm->state_list().list[(size_t)state].name;
}

static uint32_t
get_statemgr_callback_to_print(mm_count_ptr<mm_state_callback> *list, int state) {
    size_t text_slide = get_state()->mariomaker_rpl_info.text_slide;
    if (state < 0 || state >= list->count)
        return 0xeeeeeeee;
    uint32_t func = list->list[(size_t)state].func;
    return func - text_slide;
}

template<> SDKCALL void
MM_ADDR_HOOK_TY(statemgr_set_state)::hook(mm_statemgr *self, int state) {
    int old_state = self->state;
    size_t data_slide = get_state()->mariomaker_rpl_info.data_slide;
    log("statemgr_set_state: %p(%p) : %d(%s) -> %d(%s) cbs=%#x, %#x, %#x <- %p\n",
        self,
        (char *)self->vtable() - data_slide,
        old_state,
        statemgr_get_state_name(self, old_state),
        state,
        statemgr_get_state_name(self, state),
        get_statemgr_callback_to_print(&self->cb_entry_list, state),
        get_statemgr_callback_to_print(&self->cb_frame_list, state),
        get_statemgr_callback_to_print(&self->cb_exit_list, state),
        __builtin_return_address(0));
    return orig(self, state);
}


template<> SDKCALL void
MM_ADDR_HOOK_TY(ajit_push)::hook(mm_ajit *self, float dt) {
    orig(self, dt * 10);
}

template<> SDKCALL bool
MM_ADDR_HOOK_TY(Scene_should_block_fade_for_bgm)::hook(mm_Scene *self) {
    return false;
}

template<> SDKCALL int
FUNC_HOOK_TY(curl_easy_setopt)::hook(CURL *curl, int option, ...) {
    va_list ap;
    va_start(ap, option);
    if (option == CURLOPT_URL) {
        char *url = va_arg(ap, char *);
        log("curl_easy_setopt(%p, CURLOPT_URL, %s)\n", curl, url);
        return orig(curl, option, url);
    } else if (option >= CURLOPTTYPE_OFF_T) {
        uint32_t arg = va_arg(ap, uint32_t);
        log("curl_easy_setopt(%p, %d, %#x)\n", curl, option, arg);
        return orig(curl, option, arg);
    } else {
        uint64_t long_arg = va_arg(ap, uint64_t);
        log("curl_easy_setopt(%p, %d, %#llx)\n", curl, option, long_arg);
        return orig(curl, option, long_arg);
    }
    va_end(ap);
}

template<> SDKCALL int
FUNC_HOOK_TY(curl_easy_perform)::hook(CURL *curl) {
    uint64_t start = cur_time_us();
    log("curl_easy_perform(%p) start\n", curl);
    int ret = orig(curl);
    uint64_t end = cur_time_us();
    log("curl_easy_perform(%p) -> %d, took %llums\n", curl, ret, (end - start + 500) / 1000);
    return ret;
}

template<> SDKCALL int
FUNC_HOOK_TY(boss_Task_Initialize)::hook(boss_Task *self, const char *name, unsigned int x) {
    log("Task::Initialize(%p, %s, %#x)\n", self, name, x);
    return orig(self, name, x);
}

template<> SDKCALL void
FUNC_HOOK_TY(boss_Task_Finalize)::hook(boss_Task *self) {
    log("Task::Finalize(%.8s, %p)\n", self->name, self);
    return orig(self);
}
#if 0
/*

DEFINE_ORIG(orig_dotty2, int(void *this, void *second, void *third));
int
hook_dotty2_real(void *this, void *second, void *third) {
    int ret = orig_dotty2(this, second, third);
    /*
    log("dotty2(%p,%p,%p / %p:%s) <- %p <- %p <- %p => %p\n",
        this, second, third,
        *(char **)third,
        *(char **)third ?: "(null)",
        __builtin_return_address(2),
        __builtin_return_address(3),
        __builtin_return_address(4),
        ret);
    */
    if (ret) {
        const char *filename = *(char **)third;
        if (filename
            && (!strcmp(filename, "WU_SE_SYS_HURRY_UP")
                || str_endswith(filename, "_HurryUpFanfare"))) {
            struct state *state = get_state();
            OSLockMutex(&state->mutex);
            bool mute = state->should_be_quiet && !state->setting_vals[SK_PLAY_HURRY];
            OSUnlockMutex(&state->mutex);
            if (mute)
                return 0;
        }
    }
    return ret;
}
DEFINE_PILLOW(hook_dotty2, hook_dotty2_real, 0);
#endif

#if MM_DEBUG_HOOKS
template<> SDKCALL int
MM_ADDR_HOOK_TY(buffer_append_ex)::hook(void *buffer, const void *in, size_t len, int x) {
    log("buffer_append_ex(%p, %p, %zu, %d) <- %p <- %p <- %p <- %p\n", buffer, in, len, x, __builtin_return_address(0), __builtin_return_address(1), __builtin_return_address(2), __builtin_return_address(3));
    char tmp[257];
    size_t hexlen = min(len, (sizeof(tmp) - 1)/2);
    s_hex_encode(tmp, (char *)in, hexlen);
    tmp[2*hexlen] = 0;
    log(" -> appended %s\n", tmp);
    return orig(buffer, in, len, x);
}

template<> SDKCALL mm_ProtocolCallContext *
MM_ADDR_HOOK_TY(ctor_ProtocolCallContext)::hook(mm_ProtocolCallContext *self, int arg) {
    mm_ProtocolCallContext *ret = orig(self, arg);
    uint64_t now = cur_time_us();
    log("ctor_ProtocolCallContext(%p) <- %p <- %p <- %p\n", ret,  __builtin_return_address(1), __builtin_return_address(2), __builtin_return_address(3));
    OSLockMutex(&g_pcc_construction_time_mtx);
    g_pcc_construction_time_hash.set(ret, now);
    OSUnlockMutex(&g_pcc_construction_time_mtx);
    return ret;
}
template<> SDKCALL void
MM_ADDR_HOOK_TY(dtor_ProtocolCallContext)::hook(mm_ProtocolCallContext *self, int arg) {
    OSLockMutex(&g_pcc_construction_time_mtx);
    maybe<uint64_t *> val = g_pcc_construction_time_hash.get(self);
    if (val) {
        uint64_t *valp = val.unwrap();
        uint64_t now = cur_time_us();
        log("dtor_ProtocolCallContext(%p) time=%llu\n", self, now - *valp);
        g_pcc_construction_time_hash.erase(valp);
    } else {
        log("?! dtor_ProtocolCallContext(%p) didn't find start time\n", self);
    }
    OSUnlockMutex(&g_pcc_construction_time_mtx);
    orig(self, arg);
}

template<> SDKCALL int
FUNC_HOOK_TY(FSOpenFile)::hook(struct fs_client *client, struct fs_cmd *cmd, const char *path,
                const char *mode, int *handle, uint32_t flags) {
    int ret = orig(client, cmd, path, mode, handle, flags);
    if (get_state())
        log("FSOpenFile(client=%p, path=%s, mode=%s) -> %d, handle=%d\n", client, path, mode, ret, handle ? *handle : -123);
    return ret;
}
template<> SDKCALL int
FUNC_HOOK_TY(FSReadFile)::hook(struct fs_client *client, struct fs_cmd *cmd, void *ptr,
                size_t size, size_t nitems, int handle, uint32_t flags1,
                uint32_t flags2) {
    struct state *state = get_state();
    uint64_t start_time = cur_time_us();
    if (state)
        log("FSReadFile(cmd=%p, client=%p, handle=%d, ptr=%p, %zu*%zu)\n", cmd, client, handle, ptr, size, nitems);
    int ret = orig(client, cmd, ptr, size, nitems, handle, flags1, flags2);
    uint64_t end_time = cur_time_us();
    if (state)
        log("FSReadFile(cmd=%p) -> %d, time=%llu\n", cmd, ret, end_time - start_time);
    return ret;
}
template<> SDKCALL int
FUNC_HOOK_TY(FSReadFileWithPos)::hook(struct fs_client *client, struct fs_cmd *cmd, void *ptr,
                size_t size, size_t nitems, uint32_t pos, int handle, uint32_t flags1,
                uint32_t flags2) {
    struct state *state = get_state();
    uint64_t start_time = cur_time_us();
    if (state)
        log("FSReadFileWithPos(cmd=%p, client=%p, handle=%d, pos=%u, ptr=%p, %zu*%zu)\n", cmd, client, handle, pos, ptr, size, nitems);
    int ret = orig(client, cmd, ptr, size, nitems, pos, handle, flags1, flags2);
    uint64_t end_time = cur_time_us();
    if (state)
        log("FSReadFileWithPos(cmd=%p) -> %d, time=%llu\n", cmd, ret, end_time - start_time);
    return ret;
}
template<> SDKCALL int
FUNC_HOOK_TY(FSWriteFile)::hook(struct fs_client *client, struct fs_cmd *cmd, const void *ptr,
                size_t size, size_t nitems, int handle, uint32_t flags1,
                uint32_t flags2) {
    struct state *state = get_state();
    uint64_t start_time = cur_time_us();
    if (state)
        log("FSWriteFile(cmd=%p, client=%p, handle=%d, ptr=%p, %zu*%zu)\n", cmd, client, handle, ptr, size, nitems);
    int ret = orig(client, cmd, ptr, size, nitems, handle, flags1, flags2);
    uint64_t end_time = cur_time_us();
    if (state)
        log("FSWriteFile(cmd=%p) -> %d, time=%llu\n", cmd, ret, end_time - start_time);
    return ret;
}
#endif
#if MM_GXLOG_DEBUG_HOOK

static struct {
    uint32_t width, height;
    void *addr;
    uint32_t loc;
    void *caller;
} g_last_tex, g_last_cbuf;
static struct {
    void *pending_addr;
    struct GX2Surface tmp;
    struct GX2Surface capture;
} g_capture;

static void
check_draw(uint32_t n) {
    if (!in_right_process())
        return;
    if (1) {
    /*g_last_cbuf.width > 200 &&
        g_last_tex.height > 200) {*/
        log("draw(n=%u, caller=%p)  cbuf={%ux%u %p loc=%u caller=%p}  tex={%ux%u %p loc=%u caller=%p}\n",
            n, __builtin_return_address(0),
            g_last_cbuf.width, g_last_cbuf.height,
            g_last_cbuf.addr,
            g_last_cbuf.loc, g_last_cbuf.caller,
            g_last_tex.width, g_last_tex.height,
            g_last_tex.addr,
            g_last_tex.loc, g_last_tex.caller);
    }

}
template<> SDKCALL void
FUNC_HOOK_TY(GX2SetColorBuffer)::hook(struct GX2ColorBuffer *cb, int t) {
    if (in_right_process()) {
        g_last_cbuf.width = cb->surface.width;
        g_last_cbuf.height = cb->surface.height;
        g_last_cbuf.addr = cb->surface.image;
        g_last_cbuf.loc = (uint32_t)t;
        g_last_cbuf.caller = __builtin_return_address(0);
    }
    orig(cb, t);
}
template<> SDKCALL void
FUNC_HOOK_TY(GX2SetPixelTexture)::hook(struct GX2Texture *tex, uint32_t loc) {
    orig(tex, loc);
    if (in_right_process()) {
        g_last_tex.width = tex->surface.width;
        g_last_tex.height = tex->surface.height;
        g_last_tex.addr = tex->surface.image;
        g_last_tex.loc = loc;
        g_last_tex.caller = __builtin_return_address(0);
        if (tex->surface.image == g_capture.pending_addr) {
            log("* will capture %p...\n", g_capture.pending_addr);
            g_capture.pending_addr = NULL;
            if (!FUNC_HOOK_TY(GX2DrawDone)::orig()) {
                log("...but GX2DrawDone failed\n");
                return;
            }
            void *src = GX2RLockSurfaceEx(&tex->surface, 0, 1 << 22);
            if (!src) {
                log("* failed to lock tex->surface\n");
                return;
            }
            g_capture.tmp = (struct GX2Surface){
                .dim = 1,
                .width = tex->surface.width,
                .height = tex->surface.height,
                .depth = 1,
                .mipLevels = 1,
                .format = tex->surface.format,
                .resourceFlags = GX2RResourceFlags_UsageCpuReadWrite | GX2RResourceFlags_UsageGpuReadWrite,
                .tileMode = tex->surface.tileMode,
            };
            if (!GX2RCreateSurface(&g_capture.tmp, g_capture.capture.resourceFlags)) {
                log("* failed to create tmp surface (%ux%u)\n", g_capture.tmp.width, g_capture.tmp.height);
            } else {
                void *dst = GX2RLockSurfaceEx(&g_capture.tmp, 0, 0);
                ensure(dst);
                memcpy(dst, src, g_capture.tmp.imageSize);
                log("* copied to tmp surface\n");
                GX2RUnlockSurfaceEx(&g_capture.tmp, 0, 0);
            }
            GX2RUnlockSurfaceEx(&tex->surface, 0, 1 << 22);
        }
    }
}

template<> SDKCALL void
FUNC_HOOK_TY(GX2DrawEx)::hook(uint32_t m, uint32_t c, uint32_t t, uint32_t n) {
    check_draw(n);
    return orig(m, c, t, n);
}
template<> SDKCALL void
FUNC_HOOK_TY(GX2DrawIndexedEx)::hook(uint32_t m, uint32_t c, uint32_t t, void *i, uint32_t o, uint32_t n) {
    check_draw(n);
    return orig(m, c, t, i, o, n);
}
template<> SDKCALL void
FUNC_HOOK_TY(GX2DrawIndexedEx2)::hook(uint32_t m, uint32_t c, uint32_t t, void *i, uint32_t o, uint32_t n, uint32_t b) {
    check_draw(n);
    return orig(m, c, t, i, o, n, b);
}

template<> SDKCALL void
FUNC_HOOK_TY(GX2BeginDisplayListEx)::hook(void *dl, uint32_t size, uint32_t x) {
    orig(dl, size, x);
    if (in_right_process())
        log("GX2BeginDisplayListEx(%p, %u, %u)\n", dl, size, x);
}
template<> SDKCALL uint32_t
FUNC_HOOK_TY(GX2EndDisplayList)::hook(void *dl) {
    uint32_t ret = orig(dl);
    if (in_right_process())
        log("GX2EndDisplayList(%p) -> %u\n", dl, ret);
    return ret;
}
template<> SDKCALL void
FUNC_HOOK_TY(GX2CallDisplayList)::hook(void *dl, uint32_t size) {
    orig(dl, size);
    if (in_right_process())
        log("GX2CallDisplayList(%p)\n", dl);
}
template<> SDKCALL void
FUNC_HOOK_TY(GX2DirectCallDisplayList)::hook(void *dl, uint32_t size) {
    orig(dl, size);
    if (in_right_process())
        log("GX2DirectCallDisplayList(%p)\n", dl);
}
template<> SDKCALL void
FUNC_HOOK_TY(GX2CopyDisplayList)::hook(void *dl, uint32_t size) {
    orig(dl, size);
    if (in_right_process())
        log("GX2CopyDisplayList(%p)\n", dl);
}


template<> SDKCALL bool
FUNC_HOOK_TY(GX2DrawDone)::hook() {
    bool ret = orig();
    if (in_right_process()) {
        log("\n\nGX2DrawDone (&pending_addr = %p)\n\n", &g_capture.pending_addr);

        if (g_capture.tmp.image) {
            //log("* tex %p image %p
            if (g_capture.capture.image)
                GX2RDestroySurfaceEx(&g_capture.capture, 0);
            g_capture.capture = (struct GX2Surface){
                .dim = 1,
                .width = g_capture.tmp.width,
                .height = g_capture.tmp.height,
                .depth = 1,
                .mipLevels = 1,
                .format = GX2SurfaceFormat_UNORM_R8_G8_B8_A8,
                .resourceFlags = GX2RResourceFlags_UsageCpuReadWrite | GX2RResourceFlags_UsageGpuWrite,
                .tileMode = GX2TileMode_LinearAligned,
            };
            if (!GX2RCreateSurface(&g_capture.capture, g_capture.capture.resourceFlags)) {
                log("* failed to create capture surface (%ux%u)\n", g_capture.capture.width, g_capture.capture.height);
                g_capture.capture.image = NULL;
            } else {
                GX2CopySurface(&g_capture.tmp, 0, 0, &g_capture.capture, 0, 0);
                void *buf = GX2RLockSurfaceEx(&g_capture.capture, 0, 0);
                log("* capture @ %p %ux%u pitch=%u size=%u\n", buf, g_capture.capture.width, g_capture.capture.height, g_capture.capture.pitch, g_capture.capture.imageSize);
                GX2RUnlockSurfaceEx(&g_capture.capture, 0, 0);
            }
            g_capture.tmp.image = NULL;
        }
    }
    return ret;
}
#endif

static SDKCALL void
unhook_mariomaker(void) {
    uninstall_hooks(mariomaker_hooks_list);
}

void
hook_mariomaker(struct state *state) {
    uint64_t tid = OSGetTitleID();
    int version = __OSGetTitleVersion();
    if (tid != 0x000500001018DC00 || version != 272) {
        panic("Sorry, no support for this version of Mario Maker yet\nTitle ID: %llx  "
              "Version: %u",
              tid, version);
    }
    // ensure(OSGetTitleVersion() == 0x000500001018DC00);
    struct rpl_info *info = &state->mariomaker_rpl_info;
    ensure(find_rpl_info(info, "\\Block.rpx"));

#if MM_DEBUG_HOOKS
    OSInitMutex(&g_pcc_construction_time_mtx);
    g_pcc_construction_time_hash.init(&state->heap);
#endif

    // XXX do a search
    //late_hook((void *)(0x02919B74 + info.text_slide), hook_dotty2, orig_dotty2);

    new (&g_mm_addrs) mm_addrs(info->text_slide, info->data_slide);

    install_hooks(mariomaker_hooks_list);
    static struct at_exit my_at_exit = {unhook_mariomaker};
    __ghs_at_exit(&my_at_exit);

}

struct func_hook_info mariomaker_hooks_list[] = {
#if MM_DEBUG_HOOKS
    FUNC_HOOK(curl_easy_setopt),
    FUNC_HOOK(curl_easy_perform),
    FUNC_HOOK(boss_Task_Initialize),
    FUNC_HOOK(boss_Task_Finalize),
    FUNC_HOOK(FSOpenFile),
    FUNC_HOOK(FSReadFile),
    FUNC_HOOK(FSReadFileWithPos),
    FUNC_HOOK(FSWriteFile),

    MM_ADDR_HOOK(statemgr_set_state),
    MM_ADDR_HOOK(buffer_append_ex),
    MM_ADDR_HOOK(ctor_ProtocolCallContext),
    MM_ADDR_HOOK(dtor_ProtocolCallContext),
#endif
#if MM_TIMES10_HOOK
    MM_ADDR_HOOK(ajit_push),
#endif
#if MM_DOWN_HOOK
    MM_ADDR_HOOK(Scene_should_block_fade_for_bgm),
    MM_ADDR_HOOK(sets_popular_settings),
    MM_ADDR_HOOK(killer_downtimer0),
    MM_ADDR_HOOK(killer_downtimer1),
    MM_ADDR_HOOK(killer_downtimer2),
    MM_ADDR_HOOK(killer_downtimer3),
#endif
#if MM_GXLOG_DEBUG_HOOK
    FUNC_HOOK(GX2SetPixelTexture),
    FUNC_HOOK(GX2SetColorBuffer),
    FUNC_HOOK(GX2DrawEx),
    FUNC_HOOK(GX2DrawIndexedEx),
    FUNC_HOOK(GX2DrawIndexedEx2),
    FUNC_HOOK(GX2DrawDone),
    FUNC_HOOK(GX2BeginDisplayListEx),
    FUNC_HOOK(GX2EndDisplayList),
    FUNC_HOOK(GX2CallDisplayList),
    FUNC_HOOK(GX2DirectCallDisplayList),
    FUNC_HOOK(GX2CopyDisplayList),
#endif
    {0}
};

END_LOCAL_DECLS
