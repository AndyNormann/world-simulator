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
    for (int i = 0; i < 3; i++) {
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


void headless_gfx_setup(void) {
    mtl_device = MTLCreateSystemDefaultDevice();
    if (!mtl_device) { fprintf(stderr, "headless: no Metal device\n"); exit(1); }
    /* MTLTexture properties are readonly on a live object, so the descriptor is
       what gets configured; MSAA targets have to say so up front */
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
        width:(NSUInteger)gfx_w height:(NSUInteger)gfx_h mipmapped:NO];
    td.textureType = MTLTextureType2DMultisample;
    td.sampleCount = (NSUInteger)GFX_MSAA;
    td.usage = MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> color = [mtl_device newTextureWithDescriptor:td];

    MTLTextureDescriptor *rd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
        width:(NSUInteger)gfx_w height:(NSUInteger)gfx_h mipmapped:NO];
    rd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    rd.storageMode = MTLStorageModePrivate;   /* read back via a blit */
    mtl_resolve = [mtl_device newTextureWithDescriptor:rd];

    MTLTextureDescriptor *dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
        width:(NSUInteger)gfx_w height:(NSUInteger)gfx_h mipmapped:NO];
    dd.textureType = MTLTextureType2DMultisample;
    dd.sampleCount = (NSUInteger)GFX_MSAA;
    dd.usage = MTLTextureUsageRenderTarget;
    dd.storageMode = MTLStorageModePrivate;
    id<MTLTexture> depth = [mtl_device newTextureWithDescriptor:dd];
    mtl_queue = [mtl_device newCommandQueue];

    sg_setup(&(sg_desc){
        .environment = {
            .defaults = { .color_format = SG_PIXELFORMAT_RGBA8,
                          .depth_format = SG_PIXELFORMAT_DEPTH,
                          .sample_count = GFX_MSAA },
            .metal = { .device = (const void*)mtl_device },
        },
        .logger = { .func = game_logger },
        /* one buffer per chunk, and the chunk count grows with the world:
           17600 / (24 cells * 14.9) is a 50x50 grid = 2500 chunks */
        .buffer_pool_size = 4096,
    });
    hd_color   = sg_make_image(&(sg_image_desc){ .width = gfx_w, .height = gfx_h,
                    .pixel_format = SG_PIXELFORMAT_RGBA8, .sample_count = GFX_MSAA,
                    .usage.color_attachment = true,
                    .mtl_texture = (const void*)color, .label = "hd-color" });
    /* sample_count is explicit: sg_environment.defaults.sample_count is 4, so
       an unset one would ask for a multisampled resolve target */
    hd_resolve = sg_make_image(&(sg_image_desc){ .width = gfx_w, .height = gfx_h,
                    .pixel_format = SG_PIXELFORMAT_RGBA8, .sample_count = 1,
                    .usage.resolve_attachment = true, .usage.copy_src = true,
                    .mtl_texture = (const void*)mtl_resolve, .label = "hd-resolve" });
    hd_depth   = sg_make_image(&(sg_image_desc){ .width = gfx_w, .height = gfx_h,
                    .pixel_format = SG_PIXELFORMAT_DEPTH, .sample_count = GFX_MSAA,
                    .usage.depth_stencil_attachment = true,
                    .mtl_texture = (const void*)depth, .label = "hd-depth" });
    hd_color_view   = sg_make_view(&(sg_view_desc){ .color_attachment = { .image = hd_color } });
    hd_resolve_view = sg_make_view(&(sg_view_desc){ .resolve_attachment = { .image = hd_resolve } });
    hd_depth_view   = sg_make_view(&(sg_view_desc){ .depth_stencil_attachment = { .image = hd_depth } });
}

/* Copy the resolved frame into `dst` (RGBA8, row-major, top-left origin).
   Metal's render target origin is already top-left, so unlike the old
   glReadPixels path there is no flip. */
void headless_readback(unsigned char *dst) {
    NSUInteger row = (NSUInteger)gfx_w * 4;
    id<MTLBuffer> buf = [mtl_device newBufferWithLength:row * (NSUInteger)gfx_h
                                                options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cmdbuf = [mtl_queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cmdbuf blitCommandEncoder];
    [blit copyFromTexture:mtl_resolve sourceSlice:0 sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake((NSUInteger)gfx_w, (NSUInteger)gfx_h, 1)
                 toBuffer:buf destinationOffset:0
        destinationBytesPerRow:row destinationBytesPerImage:row * (NSUInteger)gfx_h];
    [blit endEncoding];
    [cmdbuf commit];
    [cmdbuf waitUntilCompleted];
    memcpy(dst, [buf contents], (size_t)row * (size_t)gfx_h);
    [buf release];
}

void init(void) {
    stm_setup();
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
    }
    terrain_check_resolution(CELL);
    draw_distance_check();
    uint64_t t0 = stm_now();
    build_terrain(terrain_seed);
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
    });
    bindings.vertex_buffers[0] = vbuf;

    sg_shader sky_shd = sg_make_shader(sky_shader_desc(sg_query_backend()));
    sky_pip = sg_make_pipeline(&(sg_pipeline_desc){
        .shader = sky_shd,
        .layout.buffers[0].stride = 0,
        .index_type = SG_INDEXTYPE_NONE,
        .depth.compare = SG_COMPAREFUNC_ALWAYS,
        .depth.write_enabled = false,
    });
    pass_action = (sg_pass_action){ .colors[0] = { .load_action = SG_LOADACTION_CLEAR, .clear_value = {SKY_R, SKY_G, SKY_B, 1.0f} },
                                     .depth = { .load_action = SG_LOADACTION_CLEAR, .clear_value = 0.0f } };
    /* mouse lock waits for focus: locking while the window is in the
       background would hijack the pointer the user is still using */
}

/* One fixed timestep of simulation. No GPU calls and no wall clock: the same
   scripted input produces the same camera path on any machine, which is what
   makes a replay a test rather than a different program. */

void render(int w, int h) {
    float fx = sinf(yaw) * cosf(pitch);
    float fy = sinf(pitch);
    float fz = cosf(yaw) * cosf(pitch);
    float rx = -cosf(yaw), rz = sinf(yaw);

    float view[16], proj[16], mvp[16];
    mat4_lookat(view, cam_x, cam_y, cam_z, cam_x+fx, cam_y+fy, cam_z+fz);
    mat4_perspective(proj, 1.0f, (float)w/(float)h, 0.1f, FAR_PLANE);
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
        pal[10*4+3] = (float)w/(float)h;
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
    int loaded = world_ready();
    if (loaded && build_async_ms == 0.0 && builder_started)
        build_async_ms = stm_sec(stm_now() - builder_t0) * 1000.0;
    pal[14*4+0] = NCHUNKS ? (float)nchunks_ready() / (float)NCHUNKS : 1.0f;
    pal[14*4+1] = 1.0f / (float)w;
    pal[14*4+2] = 1.0f / (float)h;
    pal[14*4+3] = loaded ? 0.0f : 1.0f;

    /* one pass, two ways to name the attachments: a window drawable, or our
       own textures when there is no window at all */
    if (headless) {
        sg_begin_pass(&(sg_pass){ .action = pass_action, .attachments = {
            .colors[0] = hd_color_view, .resolves[0] = hd_resolve_view,
            .depth_stencil = hd_depth_view } });
    } else {
        sg_begin_pass(&(sg_pass){ .action = pass_action, .swapchain = sglue_swapchain() });
    }
    sg_apply_pipeline(sky_pip);
    sg_apply_uniforms(0, &(sg_range){ .ptr = pal, .size = sizeof(pal) });
    sg_draw(0, 3, 1);
    if (!loaded) {
        /* the capture below is gated on world_ready() as well, so leaving the
           pass here cannot photograph the loading screen */
        frame_count++;
        sg_end_pass();
        sg_commit();
        return;
    }
    sg_apply_pipeline(pip);
    sg_apply_uniforms(0, &(sg_range){ .ptr = pal, .size = sizeof(pal) });
    sg_apply_uniforms(1, &(sg_range){ .ptr = vsu, .size = sizeof(vsu) });
    {
        /* one draw per chunk, thrown away when its bounding sphere is off
           screen: a sphere-vs-frustum test using the basis already in hand */
        float ux = -sinf(yaw) * sinf(pitch), uy = cosf(pitch), uz = -cosf(yaw) * sinf(pitch);
        const float tanv = 0.54630249f;      /* tan(0.5 * fovy), fovy = 1.0 */
        float tanh_h = tanv * (float)w / (float)h;
        chunks_drawn = 0;
        for (int i = 0; i < NCHUNKS; i++) {
            if (chunk_state(&chunks[i]) != CHUNK_READY || chunks[i].words == 0) continue;
            float ccx = ((float)chunks[i].cx + 0.5f) * CHUNK_WORLD - TERRAIN_WORLD * 0.5f;
            float ccz = ((float)chunks[i].cz + 0.5f) * CHUNK_WORLD - TERRAIN_WORLD * 0.5f;
            float ccy = 0.5f * (float)chunks[i].cy_top;
            /* radius covers the chunk's diagonal plus its relief and trees */
            float r = CHUNK_WORLD * 0.75f + 90.0f;
            float dx = ccx - cam_x, dy = ccy - cam_y, dz = ccz - cam_z;
            float depth = dx * fx + dy * fy + dz * fz;
            if (depth < -r) continue;
            if (fabsf(dx * rx + dz * rz) > r + depth * tanh_h) continue;
            if (fabsf(dx * ux + dy * uy + dz * uz) > r + depth * tanv) continue;
            bindings.vertex_buffers[0] = chunks[i].buf;
            sg_apply_bindings(&bindings);
            sg_draw(0, chunks[i].words / VERT_FLOATS, 1);
            chunks_drawn++;
        }
    }
    /* sea plane, clouds, birds: one buffer, always drawn */
    bindings.vertex_buffers[0] = vbuf;
    sg_apply_bindings(&bindings);
    sg_draw(0, decor_words / VERT_FLOATS, 1);
    frame_count++;
    sg_end_pass();
    sg_commit();
}

/* The windowed path: real elapsed time in, fixed sim steps out, one render.
   A frame that took 100ms runs six steps instead of one, so movement speed does
   not depend on the refresh rate (and a replay at 60Hz matches what is on
   screen). */

void shutdown_gfx(void) {
    stop_streamer();
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

void capture(const char *path) {
    /* do/while, not while: before the first render nothing is queued yet, so
       world_ready() is trivially true and the capture would photograph an
       empty world. The first render is what asks the streamer for chunks. */
    uint64_t t0 = stm_now();
    do {
        render(gfx_w, gfx_h);
        if (stm_sec(stm_now() - t0) > SHOT_PUMP_TIMEOUT) {
            fprintf(stderr, "capture %s: world not ready after %.0fs "
                            "(%d of %d chunks), giving up\n",
                    path, SHOT_PUMP_TIMEOUT, nchunks_ready(), NCHUNKS);
            exit(1);
        }
    } while (!world_ready());
    size_t bytes = (size_t)gfx_w * (size_t)gfx_h * 4;
    if (!cap_buf) cap_buf = (unsigned char*)malloc(bytes);
    headless_readback(cap_buf);
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
    printf("seed=%u size=%dx%d cam=%.1f,%.1f,%.1f yaw=%.2f pitch=%.2f hour=%.2f\n",
           terrain_seed, gfx_w, gfx_h, cam_x, cam_y, cam_z, yaw, pitch, day_hours);
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

/* --- replay -----------------------------------------------------------------
   A line-oriented script. Same sim_step() the game uses, so a replay is a test
   of the real thing and not of a second implementation. Format and rationale
   are documented in AGENTS.md; keep the two in step. */
