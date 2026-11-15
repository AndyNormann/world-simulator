#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "game.h"

void game_logger(const char *tag, uint32_t level, uint32_t item,
                      const char *msg, uint32_t line, const char *file, void *ud) {
    (void)ud;
    /* The palette is one uniform block shared by the terrain and sky shaders,
       and each uses only part of it: the GL compiler removes the members a
       stage never reads, so sokol cannot find them when it walks the list.
       Benign by construction, and it fires seven times per launch. */
    if (item == SG_LOGITEM_GL_UNIFORMBLOCK_NAME_NOT_FOUND_IN_SHADER) return;
    fprintf(stderr, "%s%s%s(%u): %s\n", tag, level ? " warning: " : " PANIC: ",
            file ? file : "sokol", line, msg ? msg : "(no message)");
    /* sokol would abort() here by default. Swallowing that turns a broken
       pipeline or attachment setup into a plausible-looking frame, which is the
       worst possible outcome for an agent: it reports success and ships a wrong
       image. Fail the process instead. */
    if (!level) {
        fprintf(stderr, "fatal: sokol validation failed, not rendering a possibly wrong frame\n");
        exit(1);
    }
}


void lerp3(float *o, const float *a, const float *b, float t) {
    o[0] = a[0] + (b[0]-a[0])*t; o[1] = a[1] + (b[1]-a[1])*t; o[2] = a[2] + (b[2]-a[2])*t;
}
float ramp01(float a, float b, float x) {
    float t = (x - a) / (b - a);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

/* Rayleigh extinction per airmass. The ratios are the physical ones -- blue is
   scattered out of the beam fastest -- and the scale is set so that a high sun
   is white-ish. Everything else the palette needs (sun colour, sky gradient,
   ambient, fill) is then derived from this one number. */

/* the pale sky the art direction asks for: the model tints toward this rather
   than toward a physical deep blue, and the reference sky is this colour */

/* scattered light that is no longer the sun's own colour: what fills a shadow */

/* twilight zenith: what is left overhead once the warm light is all near
   the horizon. Hand-set because it is a look, not a transmittance. */


/* Night is not sunlight, so the last palette keeps its own hand-set values:
   there is no atmosphere model for a moon. */


/* Fills pal[] with the 12 vec4s the shaders expect. Sun and moon share one
   code path: at night the "sun" is simply the body on the other side of the
   sky, which keeps one shadow direction and one disc. */
/* Where the light is at a given hour. Night reuses the same body on the other
   side of the sky, so there is one light direction, one disc and one shadow
   term to reason about. */
/* Signed elevation, one source of truth: build_palette needs the sign (to know
   whether the sun is up) and sun_angles wants the magnitude. */
float sun_elevation(float hours) {
    return sinf((hours - 6.0f) / 12.0f * 3.14159265f) * 1.05f;   /* 0 at sunrise */
}

void sun_angles(float hours, float *az, float *elev) {
    float a = (hours - 6.0f) / 12.0f * 3.14159265f;
    float e = sun_elevation(hours);
    float le = e < 0.0f ? -e : e;
    float laz = (e < 0.0f ? a + 3.14159265f + 2.85f : a + 2.85f);
    laz = fmodf(laz, 6.28318531f);
    if (laz < 0.0f) laz += 6.28318531f;
    *az = laz;
    *elev = le;
}

void build_palette(float hours, float *pal) {
    float elev = sun_elevation(hours);
    float laz, le;
    sun_angles(hours, &laz, &le);
    /* night only takes over well below the horizon: ramping it at the
       horizon made twilight darker than midnight, because the moon is
       still low then and nothing was lit */
    float nf = 1.0f - ramp01(-0.28f, -0.05f, elev);   /* night amount   */
    float df = ramp01(-0.02f, 0.30f, elev);           /* daylight amount */

    float zen[3], hor[3], sc[3], as[3], ag[3], fl[3], tmp[3];

    /* One over the sine of the elevation: how much atmosphere the light
       crosses. Clamped so it stays finite at and below the horizon. This is
       the only physical input -- sunset red, twilight blue and the dimmer sky
       all fall out of it. */
    float se = sinf(elev);
    float am = 1.0f / (se > 0.055f ? se : 0.055f);
    float T[3], lum = 0.0f;
    for (int i = 0; i < 3; i++) T[i] = expf(-BETA_R[i] * am);
    lum = 0.30f*T[0] + 0.55f*T[1] + 0.15f*T[2];
    if (lum < 1e-3f) lum = 1e-3f;
    /* the sun keeps its apparent brightness and loses only its colour, which
       is what an eye does -- transmittance alone would fade the scene to black
       long before the light actually left */
    float hm = 0.0f;
    for (int i = 0; i < 3; i++) {          /* T and sc are RGB: this read past
                                              the end for years, writing into
                                              as[] and inflating hm */
        sc[i] = T[i] / lum * 0.95f;
        if (sc[i] > hm) hm = sc[i];              /* hue: the light, normalised */
    }

    /* Desaturate the light a little on its way in: the model is right that a
       5-degree sun is almost pure red, but a scene lit by it alone turns
       monochrome, and the reference keeps its greens at dawn. */
    float sl = 0.30f*sc[0] + 0.55f*sc[1] + 0.15f*sc[2];
    for (int i = 0; i < 3; i++) sc[i] += (sl - sc[i]) * 0.40f;
    hm = 0.0f;
    for (int i = 0; i < 3; i++) { tmp[i] = sc[i]; if (tmp[i] > hm) hm = tmp[i]; }
    /* the same softening for the sky's tint, or the horizon goes as red as the
       light itself */
    for (int i = 0; i < 3; i++) tmp[i] /= hm;
    float tl = 0.30f*tmp[0] + 0.55f*tmp[1] + 0.15f*tmp[2];
    for (int i = 0; i < 3; i++) tmp[i] += (tl - tmp[i]) * 0.35f;

    float cdf = df > 0.55f ? df : 0.55f;         /* ambient never fully dies */
    float brz = 0.45f + 0.55f * sqrtf(df);       /* the zenith fades first... */
    float brh = 0.86f + 0.14f * df;              /* ...the horizon holds on */
    for (int i = 0; i < 3; i++) {
        /* The overhead sky does NOT take the sun's colour -- at dusk the warm
           light is all down at the horizon and the zenith is the clean, deep
           remainder, which is why twilight is violet and not orange. The
           horizon does take it, because that is where the long path ends. */
        float z = (SKY_PALE_Z[i] + (tmp[i] - SKY_PALE_Z[i]) * 0.03f) * brz;
        zen[i] = z + (SKY_TWILIGHT[i] - z) * (1.0f - df);
        hor[i] = (SKY_PALE_H[i] + (tmp[i] - SKY_PALE_H[i]) * 0.65f) * brh;
        /* ambient and fill are the sky's own light, so they cannot go green
           while the sky goes orange */
        as[i]  = 0.26f * sqrtf(sc[i]) * cdf + SKY_LIFT[i];
        ag[i]  = as[i] * (i == 0 ? 0.80f : i == 1 ? 0.74f : 0.36f);
        fl[i]  = as[i] * 0.33f;
    }

    lerp3(tmp, zen, NIGHT_ZEN, nf); memcpy(zen, tmp, sizeof(zen));
    lerp3(tmp, hor, NIGHT_HOR, nf); memcpy(hor, tmp, sizeof(hor));
    lerp3(tmp, sc,  NIGHT_SUN, nf); memcpy(sc,  tmp, sizeof(sc));
    lerp3(tmp, as,  NIGHT_AMBS, nf); memcpy(as, tmp, sizeof(as));
    lerp3(tmp, ag,  NIGHT_AMBG, nf); memcpy(ag, tmp, sizeof(ag));
    lerp3(tmp, fl,  NIGHT_FILL, nf); memcpy(fl, tmp, sizeof(fl));

    float disc = (0.80f + (0.55f - 0.80f) * df) * (1.0f - nf) + 0.25f * nf;
    float halo = (0.22f + (0.10f - 0.22f) * df) * (1.0f - nf) + 0.05f * nf;

    float wind_strength = 0.85f + 0.30f * sinf(anim_time * 0.37f);
    float sets[PAL_VECS][4] = {
        { cosf(laz)*cosf(le), sinf(le), sinf(laz)*cosf(le), le },
        { laz, nf, anim_time, wind_strength },
        { sc[0], sc[1], sc[2], 0.0f },
        { as[0], as[1], as[2], 0.0f },
        { ag[0], ag[1], ag[2], 0.0f },
        { fl[0], fl[1], fl[2], 0.0f },
        { zen[0], zen[1], zen[2], 0.0f },
        { hor[0], hor[1], hor[2], 0.0f },
        { disc, halo, 0.0f, 0.0f },
        { 0.0f, 0.0f, 0.0f, 0.0f },      /* camera basis: filled in frame() */
        { 0.0f, 0.0f, 0.0f, 0.0f },
        { 0.0f, 0.0f, 0.0f, 0.0f },
        { 0.0f, 0.0f, 0.0f, 0.0f },      /* eye position: filled in frame() */
        { FOG_START, FOG_FAR, FAR_PLANE, 0.0f },
    };
    for (int i = 0; i < PAL_VECS * 4; i++) pal[i] = sets[i / 4][i % 4];
}

/* --- graphics backend ------------------------------------------------------
   Two ways in: sokol_app owns a window and its Metal drawable (the game), or
   this file owns a Metal device and renders into an offscreen texture (shot /
   replay). Nothing between here and main() calls sapp_*, so the second path
   never touches a window or the window server. */


/* --- render targets --------------------------------------------------------
   Since the TAA pass needs the depth buffer as a texture, there is no
   multisampling any more: MSAA's resolve target cannot be sampled, and
   accumulating eight jittered frames beats four coverage samples anyway. The
   pipeline is scene (1x) -> TAA (history ping-pong) -> present or readback. */
static sg_image hd_color_img, hd_lin_img, hd_depth_img, hist_img[2];
static sg_view hd_color_view, hd_lin_view, hd_depth_view, k_color_tex, k_lin_tex;
static sg_view hist_view[2], hist_tex[2];
static sg_sampler lin_smp, near_smp;
static sg_pipeline taa_pip, present_pip;
static float prev_vp[16];
static float prev_vp_valid = 0.0f;
/* TAA state. The accumulator must NOT be reset when the camera moves -- that is
   precisely when it earns its keep, and resetting on motion is what made the
   first version do nothing at all while walking. Only a teleport and the
   loading->loaded transition invalidate the history; everything else is
   handled by reprojection plus the neighbourhood clamp. */
static int taa_phase = 0;
static float taa_last_x = 0.0f, taa_last_y = 0.0f, taa_last_z = 0.0f;
static int hist_cur = 0, hist_drawn = 0;
static int tgt_w = 0, tgt_h = 0;
static id<MTLTexture> mtl_hist[2];
static id<MTLTexture> mtl_color, mtl_depth;

#define TAA_BLEND 0.90f

static void targets_free(void) {
    if (tgt_w == 0) return;
    for (int i = 0; i < 2; i++) { sg_destroy_view(hist_view[i]); sg_destroy_view(hist_tex[i]);
                                  sg_destroy_image(hist_img[i]); }
    sg_destroy_view(hd_color_view); sg_destroy_view(k_color_tex);
    sg_destroy_view(hd_lin_view); sg_destroy_view(k_lin_tex);
    sg_destroy_view(hd_depth_view);
    sg_destroy_image(hd_color_img); sg_destroy_image(hd_lin_img); sg_destroy_image(hd_depth_img);
    tgt_w = tgt_h = 0;
}

/* `inject` is the headless path: we make the Metal textures ourselves so the
   readback can blit them without asking sokol for a pointer it does not expose. */
static void targets_make(int w, int h, int inject) {
    tgt_w = w; tgt_h = h;
    id<MTLTexture> c = nil, l = nil, d = nil;
    if (inject) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
            width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        c = [mtl_device newTextureWithDescriptor:td];

        MTLTextureDescriptor *ld = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float
            width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
        ld.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        ld.storageMode = MTLStorageModePrivate;
        l = [mtl_device newTextureWithDescriptor:ld];

        MTLTextureDescriptor *dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
            width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
        dd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        dd.storageMode = MTLStorageModePrivate;
        d = [mtl_device newTextureWithDescriptor:dd];
        mtl_color = c; mtl_depth = d;
        for (int i = 0; i < 2; i++) {
            MTLTextureDescriptor *hd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
            hd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            hd.storageMode = MTLStorageModePrivate;
            mtl_hist[i] = [mtl_device newTextureWithDescriptor:hd];
        }
    }
    hd_color_img = sg_make_image(&(sg_image_desc){ .width = w, .height = h,
                    .pixel_format = SG_PIXELFORMAT_RGBA8,
                    .usage.color_attachment = true,
                    .mtl_texture = (const void*)c, .label = "scene-color" });
    hd_lin_img = sg_make_image(&(sg_image_desc){ .width = w, .height = h,
                    .pixel_format = SG_PIXELFORMAT_R32F,
                    .usage.color_attachment = true,
                    .mtl_texture = (const void*)l, .label = "scene-lin" });
    hd_depth_img = sg_make_image(&(sg_image_desc){ .width = w, .height = h,
                    .pixel_format = SG_PIXELFORMAT_DEPTH,
                    .usage.depth_stencil_attachment = true,
                    .mtl_texture = (const void*)d, .label = "scene-depth" });
    for (int i = 0; i < 2; i++) {
        hist_img[i] = sg_make_image(&(sg_image_desc){ .width = w, .height = h,
                    .pixel_format = SG_PIXELFORMAT_RGBA8,
                    .usage.color_attachment = true,
                    .mtl_texture = (const void*)(inject ? mtl_hist[i] : nil),
                    .label = i ? "taa-hist-1" : "taa-hist-0" });
    }
    hd_color_view = sg_make_view(&(sg_view_desc){ .color_attachment = { .image = hd_color_img } });
    hd_lin_view = sg_make_view(&(sg_view_desc){ .color_attachment = { .image = hd_lin_img } });
    hd_depth_view = sg_make_view(&(sg_view_desc){ .depth_stencil_attachment = { .image = hd_depth_img } });
    k_color_tex   = sg_make_view(&(sg_view_desc){ .texture = { .image = hd_color_img } });
    k_lin_tex     = sg_make_view(&(sg_view_desc){ .texture = { .image = hd_lin_img } });
    for (int i = 0; i < 2; i++) {
        hist_view[i] = sg_make_view(&(sg_view_desc){ .color_attachment = { .image = hist_img[i] } });
        hist_tex[i]  = sg_make_view(&(sg_view_desc){ .texture = { .image = hist_img[i] } });
    }
    hist_cur = 0;
    prev_vp_valid = 0.0f;
}

void targets_ensure(int w, int h) {
    if (w == tgt_w && h == tgt_h) return;
    targets_free();
    targets_make(w, h, headless);
}

void headless_gfx_setup(void) {
    mtl_device = MTLCreateSystemDefaultDevice();
    if (!mtl_device) { fprintf(stderr, "headless: no Metal device\n"); exit(1); }

    sg_setup(&(sg_desc){
        .environment = {
            .defaults = { .color_format = SG_PIXELFORMAT_RGBA8,
                          .depth_format = SG_PIXELFORMAT_DEPTH,
                          .sample_count = 1 },
            .metal = { .device = (const void*)mtl_device },
        },
        .logger = { .func = game_logger },
        /* one buffer per chunk, and the chunk count grows with the world:
           17600 / (24 cells * 14.9) is a 50x50 grid = 2500 chunks */
        .buffer_pool_size = 4096,
    });
    /* The readback blit MUST go on sokol's own queue: command buffers on two
       different queues have no ordering, so the blit could copy the texture
       before the frame that filled it had run, and the shot would be one frame
       stale at random. Same queue = same order, no fence needed. */
    mtl_queue = (id<MTLCommandQueue>)(void*)sg_mtl_command_queue();
    fb_w = gfx_w * gfx_ssaa;
    fb_h = gfx_h * gfx_ssaa;
    targets_make(fb_w, fb_h, 1);
}

/* Read the final (TAA-accumulated) frame into `dst` (RGBA8, top-left origin).
   The headless path owns its textures outright; in the window they belong to
   sokol, and asking sokol for the raw Metal handle is what makes a screenshot
   in the running game the same code as a `game shot`. */
void headless_readback(unsigned char *dst) {
    id<MTLTexture> src = mtl_hist[hist_drawn];
    if (!src) {
        sg_mtl_image_info mi = sg_mtl_query_image_info(hist_img[hist_drawn]);
        src = (id<MTLTexture>)mi.tex[mi.active_slot];
    }
    if (!mtl_device) mtl_device = (id<MTLDevice>)(void*)sg_mtl_device();
    if (!mtl_queue) mtl_queue = (id<MTLCommandQueue>)(void*)sg_mtl_command_queue();
    NSUInteger row = (NSUInteger)fb_w * 4;
    id<MTLBuffer> buf = [mtl_device newBufferWithLength:row * (NSUInteger)fb_h
                                                options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cmdbuf = [mtl_queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cmdbuf blitCommandEncoder];
    [blit copyFromTexture:src sourceSlice:0 sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake((NSUInteger)fb_w, (NSUInteger)fb_h, 1)
                 toBuffer:buf destinationOffset:0
        destinationBytesPerRow:row destinationBytesPerImage:row * (NSUInteger)fb_h];
    [blit endEncoding];
    [cmdbuf commit];
    [cmdbuf waitUntilCompleted];
    memcpy(dst, [buf contents], (size_t)row * (size_t)fb_h);
    [buf release];
}

/* Device and targets. Separate from init() because a rebuild has to drop them
   and make them again while the window keeps running. */
static void gfx_device_setup(void) {
    if (headless) {
        headless_gfx_setup();
    } else {
    /* Without a logger sokol PANICs by calling abort() with no output at all,
       which is indistinguishable from a crash in our own code. Every validation
       failure now names itself and the line it came from. */
    sg_setup(&(sg_desc){
        .environment = sglue_environment(),
        .logger = { .func = game_logger },
        /* sokol's default pool is 128 buffers. One buffer per chunk means a
           world with more than 128 chunks silently fails to create them and
           then aborts inside sg_apply_bindings on the invalid handle. */
        /* one buffer per chunk, and the chunk count grows with the world:
           17600 / (24 cells * 14.9) is a 50x50 grid = 2500 chunks */
        .buffer_pool_size = 4096,
    });
    /* A window is never supersampled: temporal accumulation is the AA there and
       a 2x framebuffer would cost 4x the fill for a marginal gain. --ssaa N
       can still raise it for a benchmark. */
    if (!ssaa_given) gfx_ssaa = 1;
    if (gfx_ssaa < 1) gfx_ssaa = 1;
    fb_w = sapp_width() * gfx_ssaa;
    fb_h = sapp_height() * gfx_ssaa;
    targets_make(fb_w, fb_h, 0);
    }
}

/* Everything that belongs to *this world*: the terrain and its chunks, the
   fixed decor buffer and the pipelines that draw it. */
static void world_gfx_setup(unsigned seed) {
    terrain_check_resolution(CELL);
    draw_distance_check();
    uint64_t t0 = stm_now();
    build_terrain(seed);
    build_ms = (float)(stm_sec(stm_now() - t0) * 1000.0);



    vbuf = sg_make_buffer(&(sg_buffer_desc){
        .data = { .ptr = verts, .size = (size_t)decor_words * sizeof(float) } });
    sg_shader shd = sg_make_shader(terrain_shader_desc(sg_query_backend()));
    pip = sg_make_pipeline(&(sg_pipeline_desc){
        .shader = shd,
        .index_type = SG_INDEXTYPE_NONE,   /* flat shading: vertices are not shared */
        .layout.buffers[0].stride = VERT_FLOATS * (int)sizeof(float),
        .layout.attrs[ATTR_terrain_pos ].format = SG_VERTEXFORMAT_FLOAT3,
        .layout.attrs[ATTR_terrain_nrm ].format = SG_VERTEXFORMAT_FLOAT3,
        .layout.attrs[ATTR_terrain_col ].format = SG_VERTEXFORMAT_FLOAT3,
        .layout.attrs[ATTR_terrain_anim].format = SG_VERTEXFORMAT_FLOAT2,
        .layout.attrs[ATTR_terrain_flow].format = SG_VERTEXFORMAT_FLOAT2,
        .layout.attrs[ATTR_terrain_sh0 ].format = SG_VERTEXFORMAT_UBYTE4N,
        .layout.attrs[ATTR_terrain_sh1 ].format = SG_VERTEXFORMAT_UBYTE4N,
        .layout.attrs[ATTR_terrain_b0  ].format = SG_VERTEXFORMAT_UBYTE4N,
        .layout.attrs[ATTR_terrain_b1  ].format = SG_VERTEXFORMAT_UBYTE4N,
        .layout.attrs[ATTR_terrain_b2  ].format = SG_VERTEXFORMAT_UBYTE4N,
        .layout.attrs[ATTR_terrain_b3  ].format = SG_VERTEXFORMAT_UBYTE4N,
        .layout.attrs[ATTR_terrain_b4  ].format = SG_VERTEXFORMAT_UBYTE4N,
        .layout.attrs[ATTR_terrain_b5  ].format = SG_VERTEXFORMAT_UBYTE4N,
        /* depth unset defaults to ALWAYS/no-write, which paints over itself */
        .depth.compare = SG_COMPAREFUNC_GREATER_EQUAL,   /* reversed-Z */
        .depth.write_enabled = true,
        /* second target: linear view depth, what the temporal pass reprojects
           with. One extra float per pixel, and no depth-buffer sampling. */
        .color_count = 2,
        /* the offscreen scene target is RGBA8 in both paths; without this the
           default follows the window's swapchain (BGRA8) and the pipeline is
           rejected against our own attachment */
        .colors[0].pixel_format = SG_PIXELFORMAT_RGBA8,
        .colors[1].pixel_format = SG_PIXELFORMAT_R32F,
    });
    bindings.vertex_buffers[0] = vbuf;

    sg_shader sky_shd = sg_make_shader(sky_shader_desc(sg_query_backend()));
    sky_pip = sg_make_pipeline(&(sg_pipeline_desc){
        .shader = sky_shd,
        .layout.buffers[0].stride = 0,
        .index_type = SG_INDEXTYPE_NONE,
        .depth.compare = SG_COMPAREFUNC_ALWAYS,
        .depth.write_enabled = false,
        .color_count = 2,
        .colors[0].pixel_format = SG_PIXELFORMAT_RGBA8,
        .colors[1].pixel_format = SG_PIXELFORMAT_R32F,
    });
    lin_smp = sg_make_sampler(&(sg_sampler_desc){
        .min_filter = SG_FILTER_LINEAR, .mag_filter = SG_FILTER_LINEAR,
        .wrap_u = SG_WRAP_CLAMP_TO_EDGE, .wrap_v = SG_WRAP_CLAMP_TO_EDGE,
        .label = "linear-clamp" });
    /* depth32float cannot be filtered, so the depth read is nearest */
    near_smp = sg_make_sampler(&(sg_sampler_desc){
        .min_filter = SG_FILTER_NEAREST, .mag_filter = SG_FILTER_NEAREST,
        .wrap_u = SG_WRAP_CLAMP_TO_EDGE, .wrap_v = SG_WRAP_CLAMP_TO_EDGE,
        .label = "nearest-clamp" });
    sg_shader taa_shd = sg_make_shader(quad_shader_desc(sg_query_backend()));
    taa_pip = sg_make_pipeline(&(sg_pipeline_desc){
        .shader = taa_shd,
        .layout.buffers[0].stride = 0,
        .index_type = SG_INDEXTYPE_NONE,
        .depth.pixel_format = SG_PIXELFORMAT_NONE,   /* no depth attachment */
        .colors[0].pixel_format = SG_PIXELFORMAT_RGBA8,
        .depth.compare = SG_COMPAREFUNC_ALWAYS,
        .depth.write_enabled = false,
    });
    sg_shader pre_shd = sg_make_shader(present_shader_desc(sg_query_backend()));
    present_pip = sg_make_pipeline(&(sg_pipeline_desc){
        .shader = pre_shd,
        .layout.buffers[0].stride = 0,
        .index_type = SG_INDEXTYPE_NONE,
        /* offscreen passes want NONE, but a swapchain pass requires the
           pipeline's formats to equal the swapchain's, even when nothing is
           tested or written. Created for the window only. */
        .depth.pixel_format = headless ? SG_PIXELFORMAT_NONE : SG_PIXELFORMAT_DEPTH,
        .colors[0].pixel_format = headless ? SG_PIXELFORMAT_RGBA8 : SG_PIXELFORMAT_BGRA8,
        .depth.compare = SG_COMPAREFUNC_ALWAYS,
        .depth.write_enabled = false,
    });
    pass_action = (sg_pass_action){ .colors[0] = { .load_action = SG_LOADACTION_CLEAR, .clear_value = {SKY_R, SKY_G, SKY_B, 1.0f} },
                                     .depth = { .load_action = SG_LOADACTION_CLEAR, .clear_value = 0.0f } };
    /* mouse lock waits for focus: locking while the window is in the
       background would hijack the pointer the user is still using */
}

void init(void) {
    stm_setup();
    gfx_device_setup();
    world_gfx_setup(terrain_seed);
    ui_setup();
}

/* A new world in the same window. Everything that describes the world is either
   rebuilt by build_terrain() or destroyed here, and the gate is brutal: a shot
   after an in-window rebuild must equal a shot from a fresh process at the same
   (seed, world, camera, hour). A world-static that this misses shows up as a
   pixel difference, not as a mystery six months later. */
void world_rebuild(unsigned seed) {
    stop_streamer();
    ui_shutdown();
    sg_shutdown();                 /* chunk buffers, decor vbuf, targets, shaders */
    /* counters that a fresh process would have at zero */
    frame_count = 0;
    sim_frames = 0;
    anim_time = anim_fixed >= 0.0f ? anim_fixed : 0.0f;
    stream_rebuilds = 0;
    gfx_device_setup();
    terrain_seed = seed;
    world_gfx_setup(seed);
    ui_setup();
}

/* One fixed timestep of simulation. No GPU calls and no wall clock: the same
   scripted input produces the same camera path on any machine, which is what
   makes a replay a test rather than a different program. */

/* Sub-pixel offsets are a base-2 / base-3 Halton sequence: eight frames land
   on eight evenly spread positions inside the pixel, which is what turns the
   temporal blend into real supersampling instead of a blur. */
static float halton(int i, int base) {
    float f = 1.0f, r = 0.0f;
    while (i > 0) { f /= (float)base; r += f * (float)(i % base); i /= base; }
    return r;
}

static void taa_pass(int rw, int rh, float jx, float jy, float wgt, float fx, float fy, float fz) {
    const float tan_half = 0.54630249f;      /* tan(0.5 * fovy), fovy = 1.0 */
    const float aspect = (float)rw / (float)rh;
    float right[3] = { -cosf(yaw), 0.0f, sinf(yaw) };
    float up[3] = { -sinf(yaw) * sinf(pitch), cosf(pitch), -cosf(yaw) * sinf(pitch) };
    float tp[36];
    tp[0] = fx; tp[1] = fy; tp[2] = fz; tp[3] = 0.0f;
    for (int i = 0; i < 3; i++) tp[4+i] = right[i] * tan_half * aspect;
    tp[7] = 1.0f / (float)rw;
    for (int i = 0; i < 3; i++) tp[8+i] = up[i] * tan_half;
    tp[11] = 1.0f / (float)rh;
    tp[12] = cam_x; tp[13] = cam_y; tp[14] = cam_z; tp[15] = wgt;
    memcpy(&tp[16], prev_vp, sizeof(prev_vp));
    tp[32] = 0.1f / (FAR_PLANE - 0.1f);                 /* refit with any near plane */
    tp[33] = (0.1f * FAR_PLANE) / (FAR_PLANE - 0.1f);
    tp[34] = jx; tp[35] = jy;

    sg_bindings tb = {0};
    tb.views[0] = k_color_tex;
    tb.views[1] = hist_tex[hist_cur ^ 1];
    tb.views[2] = k_lin_tex;
    tb.samplers[0] = lin_smp;
    tb.samplers[1] = lin_smp;
    tb.samplers[2] = lin_smp;

    sg_begin_pass(&(sg_pass){ .action = { .colors[0] = { .load_action = SG_LOADACTION_DONTCARE } },
                              .attachments = { .colors[0] = hist_view[hist_cur] } });
    sg_apply_pipeline(taa_pip);
    sg_apply_bindings(&tb);
    sg_apply_uniforms(0, &(sg_range){ .ptr = tp, .size = sizeof(tp) });
    sg_draw(0, 3, 1);
    sg_end_pass();
    hist_drawn = hist_cur;
    hist_cur ^= 1;
}

static void present_pass(void) {
    if (headless) return;                    /* the shot blits hist_drawn instead */
    sg_bindings pb = {0};
    pb.views[0] = hist_tex[hist_drawn];
    pb.samplers[0] = lin_smp;
    sg_begin_pass(&(sg_pass){ .action = { .colors[0] = { .load_action = SG_LOADACTION_DONTCARE } },
                              .swapchain = sglue_swapchain() });
    sg_apply_pipeline(present_pip);
    sg_apply_bindings(&pb);
    sg_draw(0, 3, 1);
    /* The overlay shares this pass. A second swapchain pass in one frame is two
       drawables, and which one ends up on screen is the backend's business --
       so the panel is the last thing drawn in the pass that presents. */
    ui_draw_pass();
    sg_end_pass();
}

void render(int w, int h) {
    float fx = sinf(yaw) * cosf(pitch);
    float fy = sinf(pitch);
    float fz = cosf(yaw) * cosf(pitch);
    float rx = -cosf(yaw), rz = sinf(yaw);

    int rw = w * gfx_ssaa, rh = h * gfx_ssaa;
    targets_ensure(rw, rh);

    /* Jitter the projection by a different sub-pixel offset each frame: the
       TAA pass averages the results, so eight frames behave like a 2x2-to-3x3
       supersample without the memory. m[8] carries the x offset because
       clip.w = -z_view, so adding jx*clip.w is m[8] -= jx. */
    /* a jump no walk could make is a cut: the history is from another place */
    {
        float jx = cam_x - taa_last_x, jy = cam_y - taa_last_y, jz = cam_z - taa_last_z;
        if (jx*jx + jy*jy + jz*jz > 50.0f * 50.0f) prev_vp_valid = 0.0f;
        taa_last_x = cam_x; taa_last_y = cam_y; taa_last_z = cam_z;
    }
    int ji = taa_phase++ & 15;
    float jx = (halton(ji + 1, 2) - 0.5f) * 2.0f / (float)rw;
    float jy = (halton(ji + 1, 3) - 0.5f) * 2.0f / (float)rh;

    float view[16], proj[16], mvp[16];
    mat4_lookat(view, cam_x, cam_y, cam_z, cam_x+fx, cam_y+fy, cam_z+fz);
    mat4_perspective(proj, 1.0f, (float)rw/(float)rh, 0.1f, FAR_PLANE);
    proj[8] -= jx;
    proj[9] -= jy;
    mat4_mul(mvp, proj, view);

    /* --- day/night palette + animation state, uploaded as one blob --- */
    float pal[PAL_VECS * 4];
    build_palette(day_hours, pal);
    {
        /* camera basis in the last three vec4s: the sky shader rebuilds a
           world ray per pixel, so the sun stays put when the camera turns */
        const float tan_half = 0.54630249f;   /* tan(0.5 * fovy), fovy = 1.0 */
        pal[9*4+0] = fx; pal[9*4+1] = fy; pal[9*4+2] = fz; pal[9*4+3] = tan_half;
        pal[10*4+0] = -cosf(yaw); pal[10*4+1] = 0.0f; pal[10*4+2] = sinf(yaw);
        pal[10*4+3] = (float)rw/(float)rh;
        pal[11*4+0] = -sinf(yaw)*sinf(pitch);
        pal[11*4+1] = cosf(pitch);
        pal[11*4+2] = -cosf(yaw)*sinf(pitch);
        pal[11*4+3] = 0.0f;
    }
    /* Streaming: decide tiers and evictions from this frame's camera, then
       move whatever the worker has finished. Sokol wants buffer creation and
       destruction on the render thread, so both happen here. */
    chunks_update();

    /* anything the builder finished since last frame goes up now: sokol wants
       buffer creation on the render thread */
    int uploads = 0;
    for (int i = 0; i < NCHUNKS && uploads < 64; i++) {
        if (chunk_state(&chunks[i]) != CHUNK_DONE) continue;
        /* an empty chunk gets no buffer: sokol asserts on a zero-size one */
        if (chunks[i].words > 0) {
            /* the previous tier's buffer is still live: destroy it only now,
               when its replacement exists */
            if (chunks[i].buf.id != SG_INVALID_ID) {
                sg_destroy_buffer(chunks[i].buf);
                chunks[i].buf.id = SG_INVALID_ID;
            }
            chunks[i].buf = sg_make_buffer(&(sg_buffer_desc){
                .data = { .ptr = chunks[i].data, .size = (size_t)chunks[i].words * sizeof(float) } });
            if (chunks[i].buf.id == SG_INVALID_ID) {
                /* never leave a chunk READY with an invalid handle: the draw
                   loop would bind it and sokol aborts on that */
                fprintf(stderr, "chunk %d: buffer creation failed, dropping it\n", i);
                chunks[i].words = 0;
            }
        }
        free(chunks[i].data);
        chunks[i].data = NULL;
        chunk_set_state(&chunks[i], CHUNK_READY);
        uploads++;              /* capped: 49 buffer creations in one frame is a hitch */
    }

    float vsu[24];
    memcpy(vsu, mvp, sizeof(mvp));
    vsu[16] = 0.80f;                 /* wind direction, unit length */
    vsu[17] = 0.60f;
    vsu[18] = pal[1*4+3];            /* gusting strength */
    vsu[19] = anim_time;
    /* the light vector for the bounce weighting, same as u_sun in the palette */
    pal[12*4+0] = cam_x; pal[12*4+1] = cam_y; pal[12*4+2] = cam_z; pal[12*4+3] = 0.0f;
    vsu[20] = pal[0*4+0];
    vsu[21] = pal[0*4+1];
    vsu[22] = pal[0*4+2];
    vsu[23] = pal[0*4+3];
    /* the clear colour follows the horizon: it is what shows if the sky pass
       ever fails, and it matches on the very first frame */
    pass_action.colors[0].clear_value.r = pal[7*4+0];
    pass_action.colors[0].clear_value.g = pal[7*4+1];
    pass_action.colors[0].clear_value.b = pal[7*4+2];

    /* Loading gate: the sky is always drawn, the world only once every chunk
       is up. Cheap enough to be a no-op after the first second. */
    if (getenv("FLY") && frame_count % 60 == 0) {
        int t1 = 0, t2 = 0, t4 = 0, emp = 0, rdy = 0;
        for (int i = 0; i < NCHUNKS; i++) {
            if (chunk_state(&chunks[i]) == CHUNK_EMPTY) emp++;
            if (chunk_state(&chunks[i]) == CHUNK_READY) rdy++;
            if (chunk_state(&chunks[i]) == CHUNK_READY && chunks[i].tier == 1) t1++;
            if (chunk_state(&chunks[i]) == CHUNK_READY && chunks[i].tier == 2) t2++;
            if (chunk_state(&chunks[i]) == CHUNK_READY && chunks[i].tier == 4) t4++;
        }
        fprintf(stderr, "fly f%d cam %.0f,%.0f  ready=%d empty=%d t1=%d t2=%d t4=%d queued=%d rebuilds=%d\n",
                frame_count, cam_x, cam_z, rdy, emp, t1, t2, t4, jobs_pending(), stream_rebuilds);
    }
    /* Latch: streaming rebuilds make world_ready() false again and again mid-
       walk, and the loading overlay (dimmed frame + progress bar) must be for
       the first load only. Un-latched, every tier rebuild flashed the whole
       screen dark -- the flicker that only appeared when moving forward,
       because that is the motion that crosses band boundaries. */
    static int loaded_once = 0;
    int loaded = world_ready();
    if (loaded) loaded_once = 1;
    if (loaded_once) loaded = 1;
    if (loaded && build_async_ms == 0.0 && builder_started)
        build_async_ms = stm_sec(stm_now() - builder_t0) * 1000.0;
    pal[14*4+0] = NCHUNKS ? (float)nchunks_ready() / (float)NCHUNKS : 1.0f;
    pal[14*4+1] = 1.0f / (float)rw;
    pal[14*4+2] = 1.0f / (float)rh;
    pal[14*4+3] = loaded ? 0.0f : 1.0f;

    /* The scene always goes to our own target; the window only ever receives
       the accumulated frame. That is what makes the same code path serve a
       shot and a play session. */
    sg_begin_pass(&(sg_pass){ .action = { .colors[0] = pass_action.colors[0],
                                          .colors[1] = { .load_action = SG_LOADACTION_CLEAR, .clear_value = {0,0,0,0} },
                                          .depth = pass_action.depth },
                              .attachments = {
        .colors[0] = hd_color_view, .colors[1] = hd_lin_view,
        .depth_stencil = hd_depth_view } });
    sg_apply_pipeline(sky_pip);
    sg_apply_uniforms(0, &(sg_range){ .ptr = pal, .size = sizeof(pal) });
    sg_draw(0, 3, 1);
    if (!loaded) {
        /* the capture below is gated on world_ready() as well, so leaving here
           cannot photograph the loading screen; the TAA pass still runs, with
           zero weight, so the flow stays one shape */
        frame_count++;
        sg_end_pass();
        taa_pass(rw, rh, jx, jy, 0.0f, fx, fy, fz);
        present_pass();
        prev_vp_valid = 0.0f;    /* the first real frame starts from nothing */
        taa_phase = 0;           /* ...and from a known jitter phase */
        sg_commit();
        return;
    }
    sg_apply_pipeline(pip);
    sg_apply_uniforms(0, &(sg_range){ .ptr = pal, .size = sizeof(pal) });
    sg_apply_uniforms(1, &(sg_range){ .ptr = vsu, .size = sizeof(vsu) });
    int drawn_words = 0;
    {
        /* one draw per chunk, thrown away when its bounding sphere is off
           screen: a sphere-vs-frustum test using the basis already in hand */
        float ux = -sinf(yaw) * sinf(pitch), uy = cosf(pitch), uz = -cosf(yaw) * sinf(pitch);
        const float tanv = 0.54630249f;      /* tan(0.5 * fovy), fovy = 1.0 */
        float tanh_h = tanv * (float)rw / (float)rh;
        chunks_drawn = 0;
        int nocull = gfx_nocull != 0;   /* A/B and the probe: draw every chunk */
        for (int i = 0; i < NCHUNKS; i++) {
            /* READY: current mesh. BUILDING: the previous tier's mesh, still in
               its old buffer, because words and buffer are only ever replaced
               together. DONE is the one state where they disagree (the worker
               has published a new count, the render thread has not uploaded it
               yet), so it must not be drawn. */
            int st = chunk_state(&chunks[i]);
            if ((st != CHUNK_READY && st != CHUNK_BUILDING) || chunks[i].words == 0
                || chunks[i].buf.id == SG_INVALID_ID) continue;
            float ccx = ((float)chunks[i].cx + 0.5f) * CHUNK_WORLD - TERRAIN_WORLD * 0.5f;
            float ccz = ((float)chunks[i].cz + 0.5f) * CHUNK_WORLD - TERRAIN_WORLD * 0.5f;
            float ccy = 0.5f * (float)(chunks[i].cy_top + chunks[i].cy_bot);
            /* The sphere has to cover the whole box, not a guessed radius: a
               chunk whose summit is still on screen used to disappear the
               moment its centre left the frame, because the sphere was sized
               from the columns and not from the ground under them. */
            float box_h = CHUNK_WORLD * 0.7072f + 90.0f;   /* half diagonal + trees */
            float box_v = 0.5f * (float)(chunks[i].cy_top - chunks[i].cy_bot) + 90.0f;
            float cdx = ccx - cam_x, cdy = ccy - cam_y, cdz = ccz - cam_z;
            float cdepth = cdx * fx + cdy * fy + cdz * fz;
            if (!nocull && cdepth < -sqrtf(box_h * box_h + box_v * box_v)) continue;
            /* behind the eye the frustum stops widening, so the slack is the
               box extent alone and not a negative cone */
            float fwd = cdepth > 0.0f ? cdepth : 0.0f;
            /* the side planes are vertical, so the horizontal extent is exact;
               the top and bottom planes are tilted, so they take the box's
               extent along their own normal */
            float e_v = box_h * (fabsf(ux) + fabsf(uz)) + box_v * fabsf(uy);
            if (!nocull) {
                /* Plane distance, not "cone half-width plus radius": a side
                   plane's normal is (right - tan*fwd)/|...|, so the box's
                   extent along it is scaled by sqrt(1+tan^2). Dropping that
                   factor under-measures the slack by ~11% and cuts a chunk at
                   the frame edge while it is still on screen -- one sliver, one
                   frame, exactly the "it disappears as it leaves the view"
                   report. The pixel pad covers the projection's sub-pixel
                   jitter, which the test does not apply. */
                float kh = sqrtf(1.0f + tanh_h * tanh_h), kv = sqrtf(1.0f + tanv * tanv);
                float pad_h = 2.0f * fwd * tanh_h / (float)rw;
                float pad_v = 2.0f * fwd * tanv / (float)rh;
                if (fabsf(cdx * rx + cdz * rz) > box_h * kh + fwd * tanh_h + pad_h) continue;
                if (fabsf(cdx * ux + cdy * uy + cdz * uz) > e_v * kv + fwd * tanv + pad_v) continue;
            }
            bindings.vertex_buffers[0] = chunks[i].buf;
            sg_apply_bindings(&bindings);
            sg_draw(0, chunks[i].words / VERT_FLOATS, 1);
            chunks_drawn++;
            drawn_words += chunks[i].words;
        }
    }
    if (getenv("LODDBG"))
        fprintf(stderr, "lod: drawn=%d verts=%d\n", chunks_drawn, drawn_words / VERT_FLOATS);
    /* sea plane, clouds, birds: one buffer, always drawn */
    bindings.vertex_buffers[0] = vbuf;
    sg_apply_bindings(&bindings);
    sg_draw(0, decor_words / VERT_FLOATS, 1);
    sg_end_pass();
    taa_pass(rw, rh, jx, jy, prev_vp_valid ? TAA_BLEND : 0.0f, fx, fy, fz);
    present_pass();
    memcpy(prev_vp, mvp, sizeof(prev_vp));
    prev_vp_valid = 1.0f;
    frame_count++;
    sg_commit();
}

/* The windowed path: real elapsed time in, fixed sim steps out, one render.
   A frame that took 100ms runs six steps instead of one, so movement speed does
   not depend on the refresh rate (and a replay at 60Hz matches what is on
   screen). */

void shutdown_gfx(void) {
    stop_streamer();
    ui_shutdown();       /* imgui's sokol objects belong to that device */
    sg_shutdown();
}


void shrink_to(unsigned char *dst, int dw, int dh,
                      const unsigned char *src, int sw, int sh) {
    /* box filter: every destination pixel is the mean of its source rect.
       Good enough for a model's eyes and no resize library needed. */
    for (int y = 0; y < dh; y++) {
        int y0 = (int)((long)y * sh / dh), y1 = (int)((long)(y + 1) * sh / dh);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < dw; x++) {
            int x0 = (int)((long)x * sw / dw), x1 = (int)((long)(x + 1) * sw / dw);
            if (x1 <= x0) x1 = x0 + 1;
            int n = 0, r = 0, g = 0, b = 0, a = 0;
            for (int yy = y0; yy < y1 && yy < sh; yy++)
                for (int xx = x0; xx < x1 && xx < sw; xx++) {
                    const unsigned char *q = src + ((size_t)yy * sw + xx) * 4;
                    r += q[0]; g += q[1]; b += q[2]; a += q[3]; n++;
                }
            unsigned char *o = dst + ((size_t)y * dw + x) * 4;
            if (n < 1) { o[0] = o[1] = o[2] = o[3] = 0; continue; }
            o[0] = (unsigned char)(r / n); o[1] = (unsigned char)(g / n);
            o[2] = (unsigned char)(b / n); o[3] = (unsigned char)(a / n);
        }
    }
}

char *with_suffix(const char *path, const char *suffix) {
    size_t n = strlen(path), m = strlen(suffix);
    char *o = (char*)malloc(n + m + 1);
    memcpy(o, path, n); memcpy(o + n, suffix, m + 1);
    return o;
}

/* --- flicker probe ---------------------------------------------------------
   The renderer is judged by sub-pixel stability: render the same frame twice,
   with the camera moved by a fraction of a pixel, and count the pixels that
   move by more than 8/255. Edge aliasing shows up as a large number here and
   nowhere else, which is why this is the gate a rendering change has to pass.

   Everything except the camera is frozen (the shot path never advances
   anim_time or the sun), so a difference is the renderer's, not the scene's. */

static float flicker_pct(const unsigned char *a, const unsigned char *b, int only_top) {
    size_t n = (size_t)gfx_w * (size_t)gfx_h;
    size_t end = only_top ? n * 55 / 100 : n;
    size_t bad = 0;
    for (size_t i = 0; i < end; i++) {
        int d = 0;
        for (int c = 0; c < 3; c++) {
            int v = (int)a[i*4+c] - (int)b[i*4+c];
            if (v < 0) v = -v;
            if (v > d) d = v;
        }
        if (d > 8) bad++;
    }
    return 100.0f * (float)bad / (float)end;
}

void shrink_to(unsigned char *dst, int dw, int dh,
                      const unsigned char *src, int sw, int sh);

/* One frame at the internal size, boxed down to the requested size. Every
   headless consumer sees gfx_w x gfx_h; the supersampling is this function's
   business alone. */
static size_t fb_buf_bytes = 0, cap_buf_bytes = 0;
static unsigned char *grab(void) {
    size_t big = (size_t)fb_w * (size_t)fb_h * 4;
    size_t small = (size_t)gfx_w * (size_t)gfx_h * 4;
    /* a live shot changes the render size, so these are not one-shot mallocs */
    if (big != fb_buf_bytes) { free(fb_buf); fb_buf = (unsigned char*)malloc(big); fb_buf_bytes = big; }
    if (small != cap_buf_bytes) { free(cap_buf); cap_buf = (unsigned char*)malloc(small); cap_buf_bytes = small; }
    render(gfx_w, gfx_h);
    headless_readback(fb_buf);
    if (gfx_ssaa > 1) shrink_to(cap_buf, gfx_w, gfx_h, fb_buf, fb_w, fb_h);
    else memcpy(cap_buf, fb_buf, small);
    return cap_buf;
}

/* TAA is a filter with a memory, so a single frame is not representative:
   settle on a camera, measure, settle again. The number that matters is how
   different the *converged* images are for a sub-pixel camera change -- that is
   the shimmer a player sees while walking. */
static void settle(int frames) {
    /* start every measurement from a known accumulator: a value that depends on
       how many frames the streamer happened to need is not a measurement */
    prev_vp_valid = 0.0f;
    taa_phase = 0;
    for (int i = 0; i < frames; i++) grab();
}

void flicker_probe(void) {
    uint64_t t0 = stm_now();
    do {
        grab();
        if (stm_sec(stm_now() - t0) > SHOT_PUMP_TIMEOUT) {
            fprintf(stderr, "flicker: world not ready after %.0fs\n", SHOT_PUMP_TIMEOUT);
            exit(1);
        }
    } while (!world_ready());
    size_t bytes = (size_t)gfx_w * (size_t)gfx_h * 4;
    unsigned char *base = (unsigned char*)malloc(bytes);
    unsigned char *alt  = (unsigned char*)malloc(bytes);

    const float yaw0 = yaw, pitch0 = pitch, cy0 = cam_y, cx0 = cam_x, cz0 = cam_z;
    struct { const char *name; float dyaw, dpitch, dcam; } cases[] = {
        { "static",0.0f,      0.0f,      0.0f },   /* must be ~0: TAA converging */
        { "yaw",   0.000349f, 0.0f,      0.0f },   /* ~0.22 px at 800 wide */
        { "pitch", 0.0f,      0.000349f, 0.0f },
        { "rise",  0.0f,      0.0f,      0.05f },  /* geometry-invisible: catches streaming */
    };
    float res[4];
    for (int i = 0; i < 4; i++) {
        yaw = yaw0; pitch = pitch0; cam_y = cy0;
        settle(48);
        memcpy(base, cap_buf, bytes);
        yaw = yaw0 + cases[i].dyaw;
        pitch = pitch0 + cases[i].dpitch;
        cam_y = cy0 + cases[i].dcam;
        settle(48);
        memcpy(alt, cap_buf, bytes);
        res[i] = flicker_pct(base, alt, 0);
    }
    /* Culling: the frame must be identical with the frustum test switched off.
       Anything else means a chunk that is still on screen was thrown away --
       the "terrain disappears when it reaches the edge" report, which no
       frame-to-frame metric can see because the hole holds still. */
    yaw = yaw0; pitch = pitch0; cam_y = cy0; cam_x = cx0; cam_z = cz0;
    settle(48);
    memcpy(base, cap_buf, bytes);
    gfx_nocull = 1;
    settle(48);
    memcpy(alt, cap_buf, bytes);
    gfx_nocull = 0;
    float cull = flicker_pct(base, alt, 0);

    /* Walking: what the player actually reports is the wobble of the image
       from frame to frame while moving, so step the camera a fixed small
       amount and average the consecutive-frame differences. */
    /* Tier swap last, because it rebuilds every chunk. The frames used to come
       out byte-identical without that rebuild -- setting lod_clamp does nothing
       until the streamer re-queues -- which made "does swapping tiers change the
       light?" pass by accident of sampling rather than by design. Rebuild, then
       measure the area and the worst pixel. */
    yaw = yaw0; pitch = pitch0; cam_y = cy0; cam_x = cx0; cam_z = cz0;
    settle(48);
    memcpy(base, cap_buf, bytes);
    lod_clamp = 1;
    force_rebuild_all();
    {
        uint64_t t1 = stm_now();
        do {
            grab();
            if (stm_sec(stm_now() - t1) > SHOT_PUMP_TIMEOUT) {
                fprintf(stderr, "flicker: tier rebuild stalled after %.0fs\n", SHOT_PUMP_TIMEOUT);
                exit(1);
            }
        } while (!world_ready());
    }
    settle(48);
    memcpy(alt, cap_buf, bytes);
    lod_clamp = 0;
    float lod_lvl = flicker_pct(base, alt, 0);
    int lod_max = 0;
    for (size_t i = 0; i < (size_t)gfx_w * (size_t)gfx_h; i++)
        for (int c = 0; c < 3; c++) {
            int v = (int)base[i*4+c] - (int)alt[i*4+c];
            if (v < 0) v = -v;
            if (v > lod_max) lod_max = v;
        }
    settle(48);
    memcpy(base, cap_buf, bytes);
    float walk = 0.0f, walk_max = 0.0f, walk_up = 0.0f;
    const char *dump = getenv("FLICKER_DUMP");
    for (int i = 0; i < 30; i++) {
        cam_x += sinf(yaw) * 0.6f;
        cam_z += cosf(yaw) * 0.6f;
        grab();
        {
            /* a pop is one frame that jumps while its neighbours do not, so the
               maximum matters more than the mean */
            float d1 = flicker_pct(base, cap_buf, 0);
            if (d1 > walk_max) walk_max = d1;
        }
        if (dump) {
            char p[512];
            snprintf(p, sizeof p, "%s/walk-%02d.png", dump, i);
            if (fb_buf && gfx_ssaa > 1) { /* cap_buf is already the output size */ }
            stbi_write_png(p, gfx_w, gfx_h, 4, cap_buf, gfx_w * 4);
        }
        walk += flicker_pct(base, cap_buf, 0);
        memcpy(base, cap_buf, bytes);
    }
    walk /= 30.0f;

    /* The same step straight up: forward motion expands the image radially and
       leans on reprojection depth, vertical motion barely does. Comparing the
       two is how a reprojection fault is told apart from plain aliasing. */
    cam_x = cx0; cam_z = cz0; cam_y = cy0;
    settle(48);
    memcpy(base, cap_buf, bytes);
    for (int i = 0; i < 30; i++) {
        cam_y += 0.6f;
        grab();
        walk_up += flicker_pct(base, cap_buf, 0);
        memcpy(base, cap_buf, bytes);
    }
    walk_up /= 30.0f;

    /* And the real thing: hold W through sim_step, exactly as the player does,
       which walks at 12 units/s and rides the ground. A camera that follows the
       terrain changes its height every frame, so this is not the same motion as
       "forward at a fixed height" and is the only version that can show what a
       player sees. */
    cam_x = cx0; cam_z = cz0; cam_y = cy0;
    settle(48);
    memcpy(base, cap_buf, bytes);
    float walk_player = 0.0f;
    keys[SAPP_KEYCODE_W] = 1;
    for (int i = 0; i < 60; i++) {
        sim_step(1.0f / 60.0f);
        grab();
        walk_player += flicker_pct(base, cap_buf, 0);
        memcpy(base, cap_buf, bytes);
    }
    keys[SAPP_KEYCODE_W] = 0;
    walk_player /= 60.0f;

    /* Streaming: rebuild every chunk and watch for a flash. Walking forward is
       the motion that triggers tier rebuilds -- going up does not change the
       horizontal distance the streamer keys on -- which is why this, and not
       the walk, is the number that says whether streaming is visible. */
    cam_x = cx0; cam_z = cz0; cam_y = cy0;
    settle(48);
    memcpy(base, cap_buf, bytes);
    /* Measured *after* the rebuild settles, not during it. The frames in
       flight while 2500 chunks stream are a race against how fast this machine
       builds them -- the same build read 2.6% alone and 43% with the seed sweep
       running beside it -- so it was an alarm that fired on a busy morning. The
       real question is whether a rebuild leaves anything behind: rebuild every
       chunk, wait for the world to be ready, and diff against the frame before.
       That number is deterministic, and it is 0.00% when the streamer is right. */
    float rebuild_max = 0.0f;
    force_rebuild_all();
    {
        uint64_t t1 = stm_now();
        do {
            grab();
            if (stm_sec(stm_now() - t1) > SHOT_PUMP_TIMEOUT) {
                fprintf(stderr, "flicker: rebuild stalled after %.0fs\n", SHOT_PUMP_TIMEOUT);
                exit(1);
            }
        } while (!world_ready());
    }
    settle(48);
    rebuild_max = flicker_pct(base, cap_buf, 0);
    cam_x = cx0; cam_z = cz0; cam_y = cy0;
    cam_x = cx0; cam_z = cz0; cam_y = cy0;      /* the walk moved the camera: put it back, or
                                      every measurement after this one is taken
                                      from somewhere else */

    yaw = yaw0; pitch = pitch0; cam_y = cy0;
    settle(48);
    memcpy(alt, cap_buf, bytes);
    yaw = yaw0; pitch = pitch0; cam_y = cy0;
    settle(48);
    memcpy(base, cap_buf, bytes);
    printf("flicker seed=%u size=%dx%d ssaa=%d static=%.2f%% yaw=%.2f%% pitch=%.2f%% rise=%.2f%% walk=%.2f%% wmax=%.2f%% up=%.2f%% player=%.2f%% rebuild=%.2f%% cull=%.2f%% top=%.2f%% lod=%.2f%% lodmax=%d\n",
           terrain_seed, gfx_w, gfx_h, gfx_ssaa, res[0], res[1], res[2], res[3], walk, walk_max, walk_up, walk_player, rebuild_max,
           cull, flicker_pct(base, alt, 1), lod_lvl, lod_max);
    fflush(stdout);
    free(base); free(alt);
}

/* Everything below works off gfx_w/gfx_h/gfx_ssaa, so a windowed screenshot is
   a size change plus the same code path. */
static void capture_current(const char *path) {
    /* do/while, not while: before the first render nothing is queued yet, so
       world_ready() is trivially true and the capture would photograph an
       empty world. The first render is what asks the streamer for chunks. */
    uint64_t t0 = stm_now();
    do {
        grab();
        if (stm_sec(stm_now() - t0) > SHOT_PUMP_TIMEOUT) {
            fprintf(stderr, "capture %s: world not ready after %.0fs "
                            "(%d of %d chunks), giving up\n",
                    path, SHOT_PUMP_TIMEOUT, nchunks_ready(), NCHUNKS);
            exit(1);
        }
    } while (!world_ready());
    /* let the temporal filter converge to its 16-sample average: its state is
       otherwise "however many frames the streamer happened to need", and this
       is a golden image */
    settle(48);
    stbi_write_png(path, gfx_w, gfx_h, 4, cap_buf, gfx_w * 4);

    /* the downscale is written here rather than in a second command: an agent
       that has to remember it will feed a 1280px image to the model instead */
    {
        int dw = 900, dh = gfx_h * dw / gfx_w;
        if (dh < 1) dh = 1;
        unsigned char *small = (unsigned char*)malloc((size_t)dw * dh * 4);
        shrink_to(small, dw, dh, cap_buf, gfx_w, gfx_h);
        char *sp = with_suffix(path, ".small.png");
        stbi_write_png(sp, dw, dh, 4, small, dw * 4);
        free(sp); free(small);
    }
    /* heightfield dump: the shape is often the thing under test, and it is far
       more readable as an image than as a metric */
    {
        float lo = 1e9f, hi = -1e9f;
        for (int i = 0; i < N * N; i++) {
            if (heights[i] < lo) lo = heights[i];
            if (heights[i] > hi) hi = heights[i];
        }
        unsigned char *hm = (unsigned char*)malloc((size_t)N * N * 4);
        for (int i = 0; i < N * N; i++) {
            float v = (heights[i] - lo) / (hi - lo + 1e-3f);
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            unsigned char c = (unsigned char)(v * 255.0f);
            hm[i*4+0] = c; hm[i*4+1] = c; hm[i*4+2] = c; hm[i*4+3] = 255;
        }
        char *hp = with_suffix(path, ".hm.png");
        stbi_write_png(hp, N, N, 4, hm, N * 4);
        free(hp); free(hm);
    }

    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < N * N; i++) {
        if (heights[i] < lo) lo = heights[i];
        if (heights[i] > hi) hi = heights[i];
    }
    /* stdout, not a sidecar file: nothing detaches stderr any more now that
       there is no `open -g` in front of the process */
    printf("shot %s\n", path);
    printf("seed=%u world=%s size=%dx%d cam=%.1f,%.1f,%.1f yaw=%.2f pitch=%.2f hour=%.2f\n",
           terrain_seed, terrain_gen(terrain_seed).name, gfx_w, gfx_h,
           cam_x, cam_y, cam_z, yaw, pitch, day_hours);
    printf("height=%.1f..%.1f  shadowed=%d%%  trees=%d  build=%.0f+%.0fms  peak=%.0f,%.0f,%.0f\n",
           lo, hi, shadowed_pct(), tree_count, build_ms, build_async_ms,
           peak_x, peak_y, peak_z);
    printf("chunks: %d of %d ready, %d drawn\n", nchunks_ready(), NCHUNKS, chunks_drawn);
    printf("mesh: %d points, %d triangles, %d facets\n", mesh_pts, mesh_tris, mesh_facets);
    if (seg_n > 0)
        printf("rivers=%d  segments=%d  level %.1f..%.1f\n", river_count, seg_n, river_hi, river_lo);
    else
        printf("rivers=none\n");
    fflush(stdout);
}

void capture(const char *path) { capture_current(path); }

/* F2 in the running game. Renders at --ssaa 2 like a still, from a settled
   accumulator, and prints the command that reproduces the image -- a
   screenshot you cannot reproduce is a rumour. */
void live_shot(const char *path) {
    static int n = 0;
    int ow = gfx_w, oh = gfx_h, os = gfx_ssaa, og = ssaa_given, ofw = fb_w, ofh = fb_h;
    /* the window's size unless --size pinned one, which is what makes a
       scripted screenshot comparable between runs */
    if (!size_given) { gfx_w = sapp_width(); gfx_h = sapp_height(); }
    gfx_ssaa = 2; ssaa_given = 1;
    fb_w = gfx_w * gfx_ssaa; fb_h = gfx_h * gfx_ssaa;
    capture_current(path);
    fprintf(stderr,
        "reproduce: ./build/game shot --seed %u --world %s --size %dx%d --ssaa %d "
        "--cam %.1f,%.1f,%.1f --yaw %.2f --pitch %.2f --time %.2f --anim %.3f -o %s\n",
        terrain_seed, terrain_gen(terrain_seed).name, gfx_w, gfx_h, gfx_ssaa,
        cam_x, cam_y, cam_z, yaw * 180.0f / 3.14159265f, pitch * 180.0f / 3.14159265f,
        day_hours, anim_time, path);
    fprintf(stderr, "screenshot %d: %s\n", ++n, path);
    gfx_w = ow; gfx_h = oh; gfx_ssaa = os; ssaa_given = og; fb_w = ofw; fb_h = ofh;
}

/* --- replay -----------------------------------------------------------------
   A line-oriented script. Same sim_step() the game uses, so a replay is a test
   of the real thing and not of a second implementation. Format and rationale
   are documented in AGENTS.md; keep the two in step. */
