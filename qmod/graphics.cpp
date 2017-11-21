#include "containers.h"
#include "graphics.h"
#include "decls.h"
#include "font.h"
#include "misc.h"
#include "logging.h"

uint32_t colors[NUM_COLORS] = {
    [COLOR_BLACK] = RGBA(0, 0, 0, A_MAX),
    [COLOR_WHITE] = RGBA(RGB_MAX, RGB_MAX, RGB_MAX, A_MAX),
    [COLOR_YELLOW] = RGBA(RGB_MAX, RGB_MAX, 0, A_MAX),
    [COLOR_BLUE] = RGBA(0, 0, RGB_MAX, A_MAX),
    [COLOR_RED] = RGBA(RGB_MAX, 0, 0, A_MAX),
    [COLOR_GREEN] = RGBA(0, RGB_MAX, 0, A_MAX),
    [COLOR_MENUBG] = 0, // filled in later
};

#define NUM_FONT_DATA countof(font_data)

#if !DUMMY

#include "shader1.h"
#include "shader2.h"

#define HANG_DEBUG 0
#define CBUF_SCREENSHOT 0

struct vtx_buffer_1_elm {
    float x, y;
    uint32_t color;
};

struct vtx_buffer_2_elm {
    float x, y;
    float u, v;
    uint32_t color;
};

struct draw {
    size_t vertices_start_idx;
    size_t vertices_count;
    struct rect clip_rect;
};

struct renderer {
    struct heap *heap;

    float scale;

    struct vtx_buffer_2_elm *vertices;
    struct GX2RBuffer vertices_rb;
    size_t vertices_count, vertices_cap;

    uarray<struct draw> draws;
    struct draw *cur_draw;

    bool oom;

    struct GX2ColorBuffer *cbuf;
    uint32_t old_num_slices;

    struct GX2ContextState context_state;
};

struct graphics_state {
    struct renderer r;
    struct GX2FetchShader shader1_fs;
    struct GX2FetchShader shader2_fs;
    struct GX2Sampler sampler;
    struct GX2Texture texture;

#if CBUF_SCREENSHOT
    struct GX2Surface screenshot;
#endif
};

struct graphics_state *
graphics_onetime_init(void) {
    log("graphics_onetime_init\n");
    struct graphics_state *gstate = (struct graphics_state *)MEMAllocFromDefaultHeapEx(
        sizeof(struct graphics_state), __alignof(struct graphics_state));
    ensure(gstate);
    memset(gstate, 0, sizeof(*gstate));
    struct GX2AttribStream shader1_as[3] = {
        {
            // color
            .location = 0,
            .buffer = 0,
            .offset = offsetof(struct vtx_buffer_1_elm, color),
            .format = GX2AttribFormat_UNORM_8_8_8_8,
            .mask = 0x00010203, // RGBA
            .endianSwap = GX2EndianSwapMode_Default,
        },
        {
            // pos
            .location = 1,
            .buffer = 0,
            .offset = offsetof(struct vtx_buffer_1_elm, x),
            .format = GX2AttribFormat_FLOAT_32_32,
            .mask = 0x00010405, // RG01
            .endianSwap = GX2EndianSwapMode_Default,
        },
    };
    struct GX2AttribStream shader2_as[3] = {
        {
            // color
            .location = 0,
            .buffer = 0,
            .offset = offsetof(struct vtx_buffer_2_elm, color),
            .format = GX2AttribFormat_UNORM_8_8_8_8,
            .mask = 0x00010203, // RGBA
            .endianSwap = GX2EndianSwapMode_Default,
        },
        {
            // pos
            .location = 1,
            .buffer = 0,
            .offset = offsetof(struct vtx_buffer_2_elm, x),
            .format = GX2AttribFormat_FLOAT_32_32,
            .mask = 0x00010405, // RG01
            .endianSwap = GX2EndianSwapMode_Default,
        },
        {
            // uv
            .location = 2,
            .buffer = 0,
            .offset = offsetof(struct vtx_buffer_2_elm, u),
            .format = GX2AttribFormat_FLOAT_32_32,
            .mask = 0x00010405, // RG01
            .endianSwap = GX2EndianSwapMode_Default,
        },
    };
    struct {
         struct GX2AttribStream *as;
         size_t as_count;
         struct GX2FetchShader *fs_out;
    } as_info[] = {
        {shader1_as, sizeof(shader1_as)/sizeof(*shader1_as), &gstate->shader1_fs},
        {shader2_as, sizeof(shader2_as)/sizeof(*shader2_as), &gstate->shader2_fs},
    };
    for (auto info : as_info) {
        size_t fetch_shader_size
            = GX2CalcFetchShaderSizeEx(info.as_count, GX2FetchShaderType_NoTessellation, 0);
        void *fetch_shader_buf = MEMAllocFromDefaultHeapEx(fetch_shader_size, 0x100);
        ensure(fetch_shader_buf);
        GX2InitFetchShaderEx(info.fs_out, fetch_shader_buf, info.as_count, info.as,
                             GX2FetchShaderType_NoTessellation, 0);
    }

    uint32_t num_cells = NUM_FONT_DATA + 1;
    struct GX2Surface temp_surface = {
        .dim = 1,
        .width = 16 * ((num_cells + 7) / 8),
        .height = 8 * 8,
        .depth = 1,
        .mipLevels = 1,
        .format = GX2SurfaceFormat_UNORM_R8,
        .resourceFlags = GX2RResourceFlags_UsageCpuReadWrite | GX2RResourceFlags_UsageGpuRead,
        .tileMode = 1,
    };
    log("create surface\n");
    // limits
    ensure(temp_surface.width < 8192);
    ensure(temp_surface.height < 8192);
    ensure(GX2RCreateSurface(&temp_surface, temp_surface.resourceFlags));
    log("IMAGE SIZE (temp) = %d %dx%d pitch=%d\n", (int)temp_surface.imageSize,
        (int)temp_surface.width, (int)temp_surface.height, (int)temp_surface.pitch);
    {
        uint8_t *buf = (uint8_t *)GX2RLockSurfaceEx(&temp_surface, 0, 0);
        log("lock surface => %p\n", buf);
        ensure(buf);
        size_t width = temp_surface.width;
        size_t height = temp_surface.height;
        size_t pitch = temp_surface.pitch;
        for (size_t i = 0; i < num_cells; i++) {
            for (size_t xoff = 0; xoff < 8; xoff++) {
                for (size_t yoff = 0; yoff < 16; yoff++) {
                    // wow, this loop is inefficient
                    bool in;
                    if (i == NUM_FONT_DATA)
                        in = true;
                    else
                        in = (font_data[i].pixel_data[yoff] >> (7 - xoff)) & 1;
                    size_t x = 16 * (i / 8) + yoff;
                    size_t y = 8 * (i % 8) + xoff;
                    ensure(x < width);
                    ensure(y < height);
                    buf[y * pitch + x] = in ? 255 : 0;
                }
            }
        }

        log("unlock surface\n");
        GX2RUnlockSurfaceEx(&temp_surface, 0, 0);
    }
    gstate->texture = (struct GX2Texture){
        .surface = {
            .dim = 1,
            .width = temp_surface.width,
            .height = temp_surface.height,
            .depth = 1,
            .mipLevels = 1,
            .format = GX2SurfaceFormat_UNORM_R8,
            .resourceFlags = GX2RResourceFlags_BindTexture | GX2RResourceFlags_UsageCpuReadWrite | GX2RResourceFlags_UsageGpuRead,
        },
        .viewFirstMip = 0,
        .viewNumMips = 1,
        .viewFirstSlice = 0,
        .viewNumSlices = 1,
        .compMap = 0x05050500, // 111R
        //.compMap = 0x00000005, // RRR1
    };

    ensure(GX2RCreateSurface(&gstate->texture.surface,
                             gstate->texture.surface.resourceFlags));
    GX2CopySurface(&temp_surface, 0, 0, &gstate->texture.surface, 0, 0);
    ensure(GX2DrawDone());
    GX2InitTextureRegs(&gstate->texture);
    GX2RDestroySurfaceEx(&temp_surface, 0);

    GX2Invalidate(GX2InvalidateMode_CPU | GX2InvalidateMode_Shader,
                  shader2_vsh.shader_ptr, shader2_vsh.shader_size);
    GX2Invalidate(GX2InvalidateMode_CPU | GX2InvalidateMode_Shader,
                  shader2_psh.shader_ptr, shader2_psh.shader_size);

#if CBUF_SCREENSHOT
    gstate->screenshot = (struct GX2Surface){
        .dim = 1,
        .width = 1280,
        .height = 720,
        .depth = 1,
        .mipLevels = 1,
        .format = GX2SurfaceFormat_UNORM_R8_G8_B8_A8,
        .resourceFlags = GX2RResourceFlags_UsageCpuReadWrite | GX2RResourceFlags_UsageGpuWrite,
        .tileMode = GX2TileMode_LinearAligned,
    };
    ensure(GX2RCreateSurface(&gstate->screenshot,
                             gstate->screenshot.resourceFlags));
#endif


    return gstate;
}

static maybe<struct vtx_buffer_2_elm *>
renderer_alloc_vertices(struct renderer *r, size_t count) {
    size_t i = r->vertices_count, cap = r->vertices_cap;
    if (count > cap - i) {
        if (r->oom)
            return nothing;
        size_t new_cap = cap ? sat_mul(cap, 2) : 512;
        struct GX2RBuffer new_rb = {
            .flags = GX2RResourceFlags_BindVertexBuffer | GX2RResourceFlags_UsageCpuReadWrite
                | GX2RResourceFlags_UsageGpuRead,
            .elem_size = sizeof(struct vtx_buffer_2_elm),
            .elem_count = new_cap,
        };
        bool ok = GX2RCreateBuffer(&new_rb);
        struct vtx_buffer_2_elm *new_vertices;
        if (ok) {
            ensure((new_vertices = (struct vtx_buffer_2_elm *)GX2RLockBufferEx(&new_rb, 0)));
            memcpy(new_vertices, r->vertices, i * sizeof(struct vtx_buffer_2_elm));
        } else {
            new_vertices = nullptr;
            new_cap = 0;
            r->vertices_count = 0;
            r->oom = true;
        }
        if (cap) {
            GX2RUnlockBufferEx(&r->vertices_rb, 0);
            GX2RDestroyBufferEx(&r->vertices_rb, 0);
        }
        r->vertices_rb = new_rb;
        r->vertices = new_vertices;
        r->vertices_cap = new_cap;
        if (!ok)
            return nothing;
    }
    r->vertices_count = i + count;
    return just(&r->vertices[i]);
}

static void
renderer_finish_draw(struct renderer *r) {
    if (r->cur_draw) {
        r->cur_draw->vertices_count = r->vertices_count - r->cur_draw->vertices_start_idx;
        r->cur_draw = nullptr;
    }
}
static void
renderer_start_draw(struct renderer *r) {
    renderer_finish_draw(r);
    if (r->oom)
        return;
    struct draw *draw = unwrap_or(r->draws.appendn(1, r->heap), {
        r->oom = true;
        return;
    });
    draw->vertices_start_idx = r->vertices_count;
    draw->clip_rect = (struct rect){{0, 0}, {10000, 10000}};
    draw->vertices_count = 0;
    r->cur_draw = draw;
}


struct renderer *
renderer_start(struct graphics_state *gstate, struct heap *heap) {
    struct renderer *r = &gstate->r;
    ensure(r->heap == nullptr);
    ensure(heap);
    r->heap = heap;
    r->scale = 1.0f;
    if (r->vertices_cap)
        ensure((r->vertices = (struct vtx_buffer_2_elm *)GX2RLockBufferEx(&r->vertices_rb, 0)));
    else
        r->vertices = nullptr;
    r->vertices_count = 0;
    r->oom = false;
    r->draws.shrink(0);
    r->cur_draw = nullptr;
    renderer_start_draw(r);

    return r;
}

void
renderer_draw_char(struct renderer *r, struct pos pos, uint8_t font_data_idx,
                   enum color color) {
    float xlo = pos.x, ylo = pos.y;
    float xhi = xlo + 8, yhi = ylo + 16;
    float slo = 16 * (font_data_idx / 8), tlo = 8 * (font_data_idx % 8);
    float shi = slo + 16, thi = tlo + 8;
    uint32_t c = colors[color];
    // log("color=%x\n", color);
    // note: this puts the texture sideways on purpose
    float scale = r->scale;
    struct vtx_buffer_2_elm *elms = unwrap_or(renderer_alloc_vertices(r, 3), return);
    elms[0] = (struct vtx_buffer_2_elm){xlo * scale, yhi * scale, shi, tlo, c};
    elms[1] = (struct vtx_buffer_2_elm){xhi * scale, yhi * scale, shi, thi, c};
    elms[2] = (struct vtx_buffer_2_elm){xhi * scale, ylo * scale, slo, thi, c};
}

void
renderer_draw_rect(struct renderer *r, struct rect rect, enum color color,
                   uint8_t alpha) {
    float xlo = rect.pos.x, ylo = rect.pos.y;
    float xhi = xlo + rect.size.w, yhi = ylo + rect.size.h;
    float s = 16 * (NUM_FONT_DATA / 8), t = 8 * (NUM_FONT_DATA % 8);
    uint32_t c = colors[color];
    c = RGBA(get_r(c), get_g(c), get_b(c), alpha);
    float scale = r->scale;
    struct vtx_buffer_2_elm *elms = unwrap_or(renderer_alloc_vertices(r, 3), return);
    elms[0] = (struct vtx_buffer_2_elm){xlo * scale, yhi * scale, s, t, c};
    elms[1] = (struct vtx_buffer_2_elm){xhi * scale, yhi * scale, s, t, c};
    elms[2] = (struct vtx_buffer_2_elm){xhi * scale, ylo * scale, s, t, c};
}

struct rect
renderer_set_clip(struct renderer *r, struct rect rect) {
    renderer_start_draw(r);
    struct draw *draw = r->cur_draw;
    if (draw) {
        struct rect ret = draw->clip_rect;
        draw->clip_rect = rect;
        return ret;
    } else {
        return (struct rect){0};
    }
}

void
renderer_finish(struct renderer *r, struct GX2ColorBuffer *cbuf) {
    renderer_finish_draw(r);
    if (r->vertices)
        GX2RUnlockBufferEx(&r->vertices_rb, 0);
    // log("firstSlice=%d numSlices=%d viewMip=%d numMips=%d depth=%d aa=%d\n",
    // cbuf->viewFirstSlice, cbuf->viewNumSlices, cbuf->viewMip, cbuf->surface.mipLevels,
    // cbuf->surface.depth, cbuf->surface.aa);
    log("renderer_finish cbuf=%p size=%ux%u\n", cbuf, cbuf->surface.width, cbuf->surface.height);
    log("format: %#x; AA mode: %d; aaPtr: %p; aaSize: %u\n", cbuf->surface.format, cbuf->surface.aa, cbuf->aaBuffer, cbuf->aaSize);

    r->cbuf = cbuf;
    r->old_num_slices = cbuf->viewNumSlices;
    cbuf->viewNumSlices = 1;
    GX2InitColorBufferRegs(cbuf);
    if (0)
        GX2ClearColor(cbuf, 1, 0, 0, 1);
    GX2SetupContextStateEx(&r->context_state, 1);
    GX2Invalidate(0x10, cbuf->surface.image, cbuf->surface.imageSize);

    GX2SetColorBuffer(cbuf, 0);
    struct GX2ViewportReg viewport_reg = {
        .pa_cl_vport_xscale = 1,
        .pa_cl_vport_xoffset = 0,
        .pa_cl_vport_yscale = 1,
        .pa_cl_vport_yoffset = 0,
        .pa_cl_vport_zscale = 1,
        .pa_cl_vport_zoffset = 0,
        .pa_cl_gb_vert_clip_adj = 1,
        .pa_cl_gb_vert_disc_adj = 1,
        .pa_cl_gb_horz_clip_adj = 1,
        .pa_cl_gb_horz_disc_adj = 1,
        .pa_sc_vport_zmin = 0,
        .pa_sc_vport_zmax = 1,
    };
    GX2SetViewportReg(&viewport_reg);
    //GX2SetDRCScale(cbuf->surface.width, cbuf->surface.height);

    struct graphics_state *gstate = (struct graphics_state *)r;

    GX2SetFetchShader(&gstate->shader2_fs);
    GX2SetVertexShader(&shader2_vsh);
    GX2SetPixelShader(&shader2_psh);
    GX2SetPixelTexture(&gstate->texture, 0);
    GX2SetColorControl(GX2LogicOp_Copy, 1 << 0, false, true);
    GX2SetTargetChannelMasks(0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf);
    GX2SetDepthOnlyControl(false, false, 0);
    GX2SetCullOnlyControl(0, false, false);
    GX2SetAlphaTest(false, 0, 0);
    GX2InitSampler(&gstate->sampler, GX2TexClampMode_ClampBorder,
                   GX2TexXYFilterMode_Point);
    GX2InitSamplerBorderType(&gstate->sampler, GX2TexBorderType_White);
    GX2InitSamplerLOD(&gstate->sampler, 0, 0, 0);
    GX2SetPixelSampler(&gstate->sampler, 0);

    GX2SetBlendControl(0, //
                       GX2BlendMode_SrcAlpha, GX2BlendMode_InvSrcAlpha, GX2BlendCombineMode_Add, //
                       true, //
                       GX2BlendMode_One, GX2BlendMode_InvSrcAlpha, GX2BlendCombineMode_Add);
    GX2SetRasterizerClipControl(true, false);
    GX2RSetAttributeBuffer(&r->vertices_rb, 0, r->vertices_rb.elem_size, 0);
    ensure(false == GX2GetCurrentDisplayList(nullptr, nullptr));

    for (struct draw &draw : r->draws) {
        if (!draw.vertices_count)
            continue;
        log("draw.vertices_count=%zu draw.vertices_start_idx=%zu\n", draw.vertices_count, draw.vertices_start_idx);
        GX2SetScissor((uint32_t)(r->scale * draw.clip_rect.pos.x),
                      (uint32_t)(r->scale * draw.clip_rect.pos.y),
                      (uint32_t)(r->scale * draw.clip_rect.size.w),
                      (uint32_t)(r->scale * draw.clip_rect.size.h));
        GX2DrawEx(GX2PrimitiveMode_Rects, draw.vertices_count, draw.vertices_start_idx, 1);
    }
}

void
renderer_sync_and_free(struct renderer *r) {
    log("GX2DrawDone:\n");
    int ok = GX2DrawDone();
    log("...ok=%d\n", ok);
    if (!ok)
        log("GX2DrawDone timeout :(\n");

#if HANG_DEBUG
    GX2PrintGPUStatus();
#endif
    GX2SetContextState(nullptr);

#if CBUF_SCREENSHOT
    if (r->cbuf->surface.height == 720) {
        struct GX2Surface *screenshot = &((struct graphics_state *) r)->screenshot;
        GX2CopySurface(&r->cbuf->surface, 0, 0, screenshot, 0, 0);
        void *buf = GX2RLockSurfaceEx(screenshot, 0, 0);
        ensure(buf);
        log("screenshot @ %p size=%u\n", buf, screenshot->imageSize);
        GX2RUnlockSurfaceEx(screenshot, 0, 0);
    }
#endif

    r->cbuf->viewNumSlices = r->old_num_slices;
    r->cbuf = nullptr;
    r->heap = nullptr;
}

#if HANG_DEBUG
#include "misc.h"
extern void TCLOpenDebugFile(int *fsahp, int *handlep);
extern void TCLCloseDebugFile(void);
HOOK(TCLOpenDebugFile, hook_TCLOpenDebugFile, orig_TCLOpenDebugFile);
HOOK(TCLCloseDebugFile, hook_TCLCloseDebugFile, orig_TCLCloseDebugFile);
HOOK(FSAWriteFile, hook_FSAWriteFile, orig_FSAWriteFile);

void
hook_TCLOpenDebugFile(int *fsahp, int *handlep) {
    *fsahp = *handlep = 0xdead;
}
void
hook_TCLCloseDebugFile() {}

int
hook_FSAWriteFile(int fsah, const void *ptr, size_t size, size_t nitems, int handle,
                  uint32_t flags) {
    if (fsah == 0xdead) {
        log_buf(ptr, size * nitems);
        return nitems;
    }
    return orig_FSAWriteFile(fsah, ptr, size, nitems, handle, flags);
}
#endif

#else // DUMMY

#include <SDL.h>
#define sdl_ensure(x)                                                                    \
    do {                                                                                 \
        if (!(x)) {                                                                      \
            panic("sdl_ensure failed (%s:%d): %s\nSDL_GetError(): %s", __FILE__,         \
                  __LINE__, #x, SDL_GetError());                                         \
        }                                                                                \
    } while (0)

struct renderer {
    struct heap *heap;
    void *dummy;
    double scale;
    SDL_Renderer *renderer;
    SDL_Texture *textures[NUM_FONT_DATA];
    struct rect clip_rect;
};

struct graphics_state {
    SDL_Window *window;
    struct renderer r;
};

struct graphics_state *
graphics_onetime_init(void) {
    struct graphics_state *gstate = (struct graphics_state *)calloc(1, sizeof(*gstate));
    sdl_ensure(!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS));
    sdl_ensure((gstate->window = SDL_CreateWindow(
                    "test", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 400, 400,
                    SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI)));
    sdl_ensure(
        (gstate->r.renderer = SDL_CreateRenderer(
             gstate->window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC)));
    for (size_t i = 0; i < NUM_FONT_DATA; i++) {
        SDL_Texture *tex
            = SDL_CreateTexture(gstate->r.renderer, SDL_PIXELFORMAT_RGBA8888,
                                SDL_TEXTUREACCESS_STREAMING, FONT_WIDTH, FONT_HEIGHT);
        ensure(tex);
        void *pixels;
        int pitch;
        sdl_ensure(!SDL_LockTexture(tex, nullptr, &pixels, &pitch));
        for (size_t y = 0; y < FONT_HEIGHT; y++) {
            for (size_t x = 0; x < FONT_WIDTH; x++) {
                uint32_t *p = (uint32_t *)((char *)pixels + (size_t)pitch * y + 4 * x);
                bool on = (font_data[i].pixel_data[y] >> (FONT_WIDTH - 1 - x)) & 1;
                *p = on ? 0xffffffff : 0x00000000;
            }
        }
        SDL_UnlockTexture(tex);
        gstate->r.textures[i] = tex;
    }
    return gstate;
}

void
renderer_sync_and_free(struct renderer *r) {
    SDL_RenderPresent(r->renderer);
    r->heap = nullptr;
    free(r->dummy);
    r->dummy = nullptr;
}

struct renderer *
renderer_start(struct graphics_state *gstate,
               struct heap *heap) {
    {
        // Hack to work around SDL bug on macOS (missing [context update] call)
        int w, h;
        SDL_GetWindowSize(gstate->window, &w, &h);
        SDL_SetWindowSize(gstate->window, w, h);
    }

    //colors[COLOR_BLUE] ^= RGBA(255, 255, 255, 0); // epilepsy mode
    struct renderer *r = &gstate->r;
    r->clip_rect = (struct rect){0};
    SDL_Renderer *renderer = r->renderer;

    ensure(heap);
    ensure(!r->heap);
    r->heap = heap;
    r->dummy = unwrap_or(heap_alloc(r->heap, 0x1234), panic("dummy alloc failed"));
    memset(r->dummy, 0xee, 0x1234);

    sdl_ensure(!SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND));
    sdl_ensure(!SDL_SetRenderDrawColor(renderer, 0xdd, 0xff, 0xff, 0xff));
    sdl_ensure(!SDL_RenderClear(renderer));

    int ww, wh, ow, oh;
    SDL_GetWindowSize(gstate->window, &ww, &wh);
    sdl_ensure(!SDL_GetRendererOutputSize(renderer, &ow, &oh));
    r->scale = ww ? ((double)ow / ww) : 1;

    return r;
}

void
renderer_draw_char(struct renderer *r, struct pos pos, uint8_t font_data_idx,
                   enum color color) {
    ensure(0 <= font_data_idx && font_data_idx < NUM_FONT_DATA);
    SDL_Texture *tex = r->textures[font_data_idx];

    uint32_t c = colors[color];
    sdl_ensure(!SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND));
    sdl_ensure(!SDL_SetTextureColorMod(tex, (uint8_t)get_r(c), (uint8_t)get_g(c),
                                       (uint8_t)get_b(c)));
    SDL_Rect srect = {
        (int)(pos.x * r->scale),
        (int)(pos.y * r->scale),
        (int)(FONT_WIDTH * r->scale),
        (int)(FONT_HEIGHT * r->scale),
    };
    sdl_ensure(!SDL_RenderCopy(r->renderer, tex, nullptr, &srect));
}

void
renderer_draw_rect(struct renderer *r, struct rect rect, enum color color,
                   uint8_t alpha) {
    uint32_t c = colors[color];
    SDL_SetRenderDrawColor(r->renderer, (uint8_t)get_r(c), (uint8_t)get_g(c),
                           (uint8_t)get_b(c), alpha);
    //(uint8_t)((1.-0.125)*0xff));

    SDL_Rect srect = {
        (int)(rect.pos.x * r->scale),
        (int)(rect.pos.y * r->scale),
        (int)(rect.size.w * r->scale),
        (int)(rect.size.h * r->scale),
    };
    sdl_ensure(!SDL_RenderFillRect(r->renderer, &srect));
}

struct rect
renderer_set_clip(struct renderer *r, struct rect rect) {
    SDL_Rect srect = {
        (int)(rect.pos.x * r->scale),
        (int)(rect.pos.y * r->scale),
        (int)(rect.size.w * r->scale),
        (int)(rect.size.h * r->scale),
    };
    sdl_ensure(!SDL_RenderSetClipRect(r->renderer, &srect));
    struct rect ret = r->clip_rect;
    r->clip_rect = rect;
    return ret;
}

void
renderer_finish(struct renderer *r, struct GX2ColorBuffer *cbuf) {}

#endif
