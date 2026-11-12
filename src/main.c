#define SOKOL_DEBUG
#define SOKOL_IMPL
/* never grab focus on launch: see the note in sokol_app.h */
#define SAPP_DONT_ACTIVATE
#define SOKOL_METAL
/* sokol_app defines main(); this file has its own so `game shot` can run
   without ever opening a window (see main() at the bottom) */
#define SOKOL_NO_ENTRY
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_time.h"
#include "sokol_glue.h"
#include <Metal/Metal.h>
#include "math3d.h"
#include "terrain.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <assert.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include <pthread.h>
#include <stdarg.h>
#include <math.h>

#define N TERRAIN_N
#define CELL (TERRAIN_WORLD / (float)(N - 1))

/* Palette measured off the reference image: warm, desaturated, high contrast. */
#define SKY_R 0.90f
#define SKY_G 0.85f
#define SKY_B 0.72f

static float heights[N * N];
static float scratch[N * N];
/* One horizon angle per 45-degree azimuth sector per grid vertex: the highest
   thing seen looking that way. That is enough to light ANY sun position, so the
   day/night cycle gets correct shadows (long at dawn, short at noon) without a
   shadow map or a per-frame CPU bake. */
#define SUN_SECTORS 8
/* No horizon table any more: the shadow a surface casts on itself is needed
   only where there is a vertex, so it is evaluated there (horizon_at). A baked
   grid cost N*N*8 ray marches for the whole world no matter how much of it was
   drawn, and it was the most expensive stage of a build. */
/* Trees do not shadow each other. In a dense stand every trunk would sit below
   its neighbour's skyline and the whole forest would render as a black band --
   the reference's canopies stay green in full sun. Their own shade on the
   ground is still baked, which is the part that reads. */
static float sky_ao[N * N];        /* baked ambient occlusion, 0..1 */
/* Radiance transfer, one colour per azimuth sector per grid vertex: the albedo
   of the ground that direction throws back at this point. It is baked from the
   SURFACE, not from the sun, so one bake serves every hour of the day -- at
   runtime the sectors are weighted by how much the sun lights them, which is
   the whole of the bounce. Cheap stand-in for an SH probe. */
/* No bounce table either: eight sectors of bounced colour per point, computed
   where the points are (bounce_at). As a grid it cost N*N*32 colour probes and
   8 MB of resident floats at this world size, for values only vertices read. */
/* flat shading needs one normal per triangle, so vertices are not shared:
   6 verts per quad, pos(3) + normal(3) + colour(3) + anim(2) + flow(2).
   `anim` is the wind sway weight and per-tree phase (clouds use a negative
   weight as a sentinel); `flow` is the river tangent, zero off the water;
   `horizon` is eight sky-occlusion angles, one per 45-degree azimuth
   sector, which is what lets the shadows follow the sun.
   Everything is appended to the same buffer, so the scene stays one call. */
/* One vertex is 21 32-bit words = 84 bytes:
     pos f3, nrm f3, col f3, anim f2, flow f2   (13 words, 52 B)
     horizon  8 angles, one per azimuth sector (2 words,  8 B, 8-bit each)
     bounce   8 sectors of RGB albedo          (6 words, 24 B, 8-bit each)
   The lighting attributes are the ones that were eating the buffer: 32 floats
   of them on every vertex of every tree. They are low-frequency and they end up
   behind a tone curve, so 8 bits is invisible and the vertex halves. */
#define VERT_FLOATS 21
/* the horizon angles are stored as a fraction of this */
#define HORIZ_RANGE 1.6f
#define TERRAIN_VERTS 36000    /* MAX_PTS 8000 -> at most ~16000 triangles */
#define MAX_TREES 96000
#define MAX_CLOUD_VERTS 20000
#define TREE_VERTS 300         /* trunk + two blobs, with headroom */
/* Two vertex streams, because they are built at different times by different
   threads: the sky furniture (clouds, birds) is emitted on the main thread once
   the camera is known, and the chunk geometry on the builder thread. */
/* sea plane (~34k verts at this world size) + clouds + birds */
#define DECOR_VERTS 40000
static float verts[DECOR_VERTS * VERT_FLOATS];
static int decor_words = 0;
static float *vtarget = verts;  /* push_vert writes here; see chunk_verts */
static int vcount = 0;          /* words written into *vtarget */

/* --- chunked geometry -----------------------------------------------------
   The world is one heightfield and one triangulation, but the vertices are
   uploaded and drawn per chunk: that is what lets the frustum throw away most
   of the island, and it is the shape the mesh needs before it can be streamed
   with an eviction policy. Splitting AFTER triangulation (rather than
   triangulating each chunk separately) means no crack repair, no border
   stitching and no seam through the facets -- the triangles are the global
   ones, only their buffer differs. */
#define CHUNK_CELLS 24
#define CHUNK_WORLD (CHUNK_CELLS * CELL)
/* Integer on purpose: the count sizes arrays. draw_distance_check() asserts it
   still covers the world after any change to TERRAIN_WORLD or CHUNK_CELLS. */
#define CHUNK_GRID 25
#define NCHUNKS (CHUNK_GRID * CHUNK_GRID)
#define MAX_CHUNK_VERTS 40000

enum { CHUNK_EMPTY = 0, CHUNK_BUILDING, CHUNK_DONE, CHUNK_READY };

typedef struct {
    int cx, cz;
    int state;
    sg_buffer buf;
    float *data;          /* worker output, freed once uploaded */
    int words;
    int cy_top;           /* highest terrain in the chunk, for culling */
    int tier;             /* interior spacing in cells of the mesh that is up */
} chunk_t;

static chunk_t chunks[NCHUNKS];
/* the built surface colours, shared with the builder thread */
static const float *gcr, *gcg, *gcb;
static float chunk_verts[MAX_CHUNK_VERTS * VERT_FLOATS];

static unsigned quant8(float v) {
    int i = (int)(v * 255.0f + 0.5f);
    if (i < 0) i = 0;
    if (i > 255) i = 255;
    return (unsigned)i;
}

/* four 0..1 values as one word, little-endian, matching UBYTE4N */
static float pack4(float a, float b, float c, float d) {
    unsigned u = quant8(a) | (quant8(b) << 8) | (quant8(c) << 16) | (quant8(d) << 24);
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

/* Every emitter writes through this, so the capacity check lives here: the
   sea plane once grew with the world and ran off the end of decor verts[]
   (SIGBUS at a write past the array, which is how this was found). */
static int vcap = 0;
static void push_vert(const float *p, const float *n, const float *c,
                      float aw, float ap, float fx, float fz,
                      const float *sh, const float *bc) {
    if (vcount + VERT_FLOATS > vcap) return;
    float *v = vtarget;
    v[vcount++] = p[0]; v[vcount++] = p[1]; v[vcount++] = p[2];
    v[vcount++] = n[0]; v[vcount++] = n[1]; v[vcount++] = n[2];
    v[vcount++] = c[0]; v[vcount++] = c[1]; v[vcount++] = c[2];
    v[vcount++] = aw;   v[vcount++] = ap;
    v[vcount++] = fx;   v[vcount++] = fz;
    for (int k = 0; k < 8; k += 4) {
        if (sh) v[vcount++] = pack4(sh[k]/HORIZ_RANGE, sh[k+1]/HORIZ_RANGE,
                                    sh[k+2]/HORIZ_RANGE, sh[k+3]/HORIZ_RANGE);
        else    v[vcount++] = 0.0f;
    }
    for (int k = 0; k < 24; k += 4) {
        if (bc) v[vcount++] = pack4(bc[k], bc[k+1], bc[k+2], bc[k+3]);
        else    v[vcount++] = 0.0f;
    }
}

/* Written before a tree is emitted: sway weight needs the trunk base and the
   canopy top, and every emit path already knows them. */
static float sway_base = 0.0f, sway_h = 1.0f, sway_phase = 0.0f;
/* horizon of the ground a tree stands on: without it the trees stay sunlit
   while the valley around them goes into shadow at dusk */
static const float *sway_shadow = NULL;
/* the bounce of that same ground cell, so a tree is filled by the same light
   as the meadow it stands in instead of staying a flat cut-out */
static const float *sway_bounce = NULL;

/* Species differ by silhouette, not by parameter: at the size these are drawn
   the outline is the only thing that reads, so each kind is a distinct shape
   (stacked plates, a round crown, a flat umbrella, a thin spire). */
enum { TREE_PINE, TREE_FIR, TREE_SPIRE, TREE_BROADLEAF, TREE_ACACIA, TREE_KINDS };

typedef struct {
    float x, z, y, h, r;
    unsigned hash;
    unsigned char kind;
} tree_t;
static tree_t trees[MAX_TREES];
static int tree_count = 0;

/* Draw distance, in world units. Fog saturates at FOG_FAR, so the far terrain
   dissolves into exactly the sky behind it instead of stopping at a ceiling --
   that ceiling was invisible in a 760-unit world and reads as a hard line at
   any distance you would call long. The far plane lives past FOG_FAR so
   nothing is clipped while it is still partially fogged. Growing the world
   means growing these three numbers and nothing else. */
/* Relative to the world, not absolute. These were 380/1300/1600 on a
   1100-unit island -- i.e. 0.35, 1.18 and 1.45 of it -- so growing the world
   keeps the same haze in the same place in frame instead of fogging the
   island out at 1300 units from anywhere. */
#define FOG_START  (0.35f * TERRAIN_WORLD)
#define FOG_FAR    (1.18f * TERRAIN_WORLD)
#define FAR_PLANE  (1.45f * TERRAIN_WORLD)
/* The sea plane reaches this far out from the origin. Where it ends the fog has
   to be total, or the plane's edge is a hard line across the horizon -- a bug
   this project has introduced twice already (once at the world edge, once at
   1.5x). Keep the two numbers in step; draw_distance_check() enforces it. */
#define SEA_EXTENT (TERRAIN_WORLD * 2.6f)

static void draw_distance_check(void) {
    /* the camera is never farther out than half a world diagonal */
    float reach = SEA_EXTENT - TERRAIN_WORLD * 0.7072f;
    assert(FAR_PLANE > FOG_FAR);
    assert(reach > FOG_FAR * 1.1f);
    assert((float)CHUNK_GRID * CHUNK_WORLD >= TERRAIN_WORLD);
    (void)reach;
}

/* A vec4 per uniform: light, sky state, colours, sky gradient, sun disc, camera
   basis, eye position, fog. Written as plain floats in frame() and uploaded as
   one blob. */
#define PAL_VECS 15
#define PAL_BYTES (PAL_VECS * 16)
#define VS_BYTES 96                    /* mat4 mvp + vec4 wind + vec4 sun */

static sg_pipeline pip;
static sg_pipeline sky_pip;
static sg_bindings bindings;
static sg_pass_action pass_action;
static sg_buffer vbuf;

static unsigned terrain_seed = 1;
static const char *shot_path = "screenshot.png";
static int headless = 0;       /* no window: render an offscreen texture */
static int frame_count = 0;    /* frames rendered, for diagnostics only */
static long sim_frames = 0;    /* fixed steps taken: animation and the clock
                                  are derived from this, never from wall time */
static float cam_x = 0.0f, cam_y = 60.0f, cam_z = -0.34f * TERRAIN_WORLD;
static float yaw = 0.0f, pitch = -0.12f;
static float tyaw = 0.0f, tpitch = -0.12f;
static int keys[512];
static int got_first_mouse = 0;   /* the warp on lock emits one bogus delta */
static float build_ms = 0.0f;
static float day_hours = 9.18f;    /* 0..24, sunrise at 6, sunset at 18 */
static int time_fixed = 0;         /* --time given: do not advance */
static float anim_time = 0.0f;     /* sim_frames / 60, not wall clock: keeps
                                      captures reproducible */
static int have_view = 0;      /* --yaw/--pitch/--cam given: don't auto-aim */
/* debug: flat reference-grey ground so tree silhouettes can be measured with
   the same blob analysis as the reference image */
static int trees_only = 0;
static float peak_x = 0.0f, peak_y = 0.0f, peak_z = 0.0f;

#include "shaders.h"   /* generated by tools/shaders.sh -- never edit by hand */

/* bilinear height lookup on the eroded grid, for the sun ray-march */
static float sample_height(float x, float z) {
    float gx = (x + TERRAIN_WORLD * 0.5f) / CELL;
    float gz = (z + TERRAIN_WORLD * 0.5f) / CELL;
    int x0 = (int)floorf(gx), z0 = (int)floorf(gz);
    if (x0 < 0 || z0 < 0 || x0 >= N - 1 || z0 >= N - 1) return -1e9f;
    float fx = gx - x0, fz = gz - z0;
    float h00 = heights[z0 * N + x0], h10 = heights[z0 * N + x0 + 1];
    float h01 = heights[(z0 + 1) * N + x0], h11 = heights[(z0 + 1) * N + x0 + 1];
    return (h00 * (1 - fx) + h10 * fx) * (1 - fz) + (h01 * (1 - fx) + h11 * fx) * fz;
}

/* How high the terrain rises in each of the eight azimuth sectors, and how
   much of the sun that leaves. Trees go in as well -- a crown is an occluder,
   and that is what gives the forest floor its shade and each trunk its
   shadow. The per-point version is horizon_at; these are the pieces. */
static void sun_angles(float hours, float *az, float *elev);
static void horizon_at(float x, float z, float y, int with_trees, float *out);
static float surface_height(float x, float z);
static float shadow_lit(const float *h, float az, float elev);

/* Sampled on a 48x48 lattice rather than per grid cell: this is a number in the
   capture sidecar, and marching the horizon for every cell of a world-scale
   grid to produce it is exactly the cost this change removed. */
static int shadowed_pct(void) {
    float az, el;
    sun_angles(day_hours, &az, &el);
    const int S = 48;
    int lit = 0;
    float h[SUN_SECTORS];
    for (int zi = 0; zi < S; zi++) {
        for (int xi = 0; xi < S; xi++) {
            float x = (xi / (float)(S - 1)) * TERRAIN_WORLD - TERRAIN_WORLD * 0.5f;
            float z = (zi / (float)(S - 1)) * TERRAIN_WORLD - TERRAIN_WORLD * 0.5f;
            float y = surface_height(x, z);
            if (y < -1e8f) { lit++; continue; }
            horizon_at(x, z, y, 1, h);
            if (shadow_lit(h, az, el) > 0.5f) lit++;
        }
    }
    return 100 * (S * S - lit) / (S * S);
}

/* Bilinear read of the baked surface colours at an arbitrary world point. */
static void sample_rgb(const float *cr, const float *cg, const float *cb,
                       float x, float z, float *o) {
    float gx = (x + TERRAIN_WORLD * 0.5f) / CELL;
    float gz = (z + TERRAIN_WORLD * 0.5f) / CELL;
    int x0 = (int)floorf(gx), z0 = (int)floorf(gz);
    if (x0 < 0) x0 = 0; if (x0 > N - 2) x0 = N - 2;
    if (z0 < 0) z0 = 0; if (z0 > N - 2) z0 = N - 2;
    float fx = gx - x0, fz = gz - z0;
    if (fx < 0.0f) fx = 0.0f; if (fx > 1.0f) fx = 1.0f;
    if (fz < 0.0f) fz = 0.0f; if (fz > 1.0f) fz = 1.0f;
    int i00 = z0 * N + x0, i10 = i00 + 1, i01 = i00 + N, i11 = i01 + 1;
    #define SAMP(a) ((a[i00]*(1-fx)+a[i10]*fx)*(1-fz) + (a[i01]*(1-fx)+a[i11]*fx)*fz)
    o[0] = SAMP(cr); o[1] = SAMP(cg); o[2] = SAMP(cb);
    #undef SAMP
}

/* What colour of light does the ground around this point throw back? Four
   probes per azimuth sector, weighted toward the near ground. Deliberately not
   raycast: a bounce probe wants the AVERAGE of what is out there, and an
   occlusion test would only re-derive sky_ao. */
/* What colour of light does the ground around this point throw back? Four
   probes per azimuth sector, weighted toward the near ground. Deliberately not
   raycast: a bounce probe wants the AVERAGE of what is out there, and an
   occlusion test would only re-derive sky_ao. */
static void bounce_at(float x, float z, float y, const float *cr,
                      const float *cg, const float *cb, float *out) {
    static const float D[4] = { 26.0f, 55.0f, 95.0f, 150.0f };
    for (int k = 0; k < SUN_SECTORS; k++) {
        float a = (float)k * 6.28318531f / (float)SUN_SECTORS;
        float dx = cosf(a), dz = sinf(a);
        float wr = 0.0f, wg = 0.0f, wb = 0.0f, ws = 0.0f;
        for (int q = 0; q < 4; q++) {
            float d = D[q];
            float px = x + dx * d, pz = z + dz * d;
            float h = sample_height(px, pz);
            if (h < -1e8f) continue;
            float c[3];
            sample_rgb(cr, cg, cb, px, pz, c);
            float w = 1.0f / (1.0f + d * 0.012f);
            /* a wall of ground above us faces away, so it lights us less than
               a field at our own level does */
            float dy = (h - y) / d;
            if (dy > 0.0f) w /= 1.0f + dy * 1.6f;
            wr += c[0] * w; wg += c[1] * w; wb += c[2] * w; ws += w;
        }
        float *o = out + k * 3;
        if (ws > 1e-4f) {
            o[0] = wr / ws; o[1] = wg / ws; o[2] = wb / ws;
        } else {
            o[0] = 0.30f; o[1] = 0.32f; o[2] = 0.20f;
        }
    }
}

/* Trees bucketed so a horizon query only looks at the crowns that can matter:
   110 world units is the reach of a crown in the shade, so the 3x3 bucket
   neighbourhood around a point holds every tree that can occlude it. */
#define TREE_BUCKET 110.0f
static int tree_nb = 0;
static int *tree_bstart = NULL;
static int *tree_blist = NULL;

static int tree_bucket_of(float v) {
    int i = (int)floorf((v + TERRAIN_WORLD * 0.5f + TREE_BUCKET) / TREE_BUCKET);
    if (i < 0) i = 0;
    if (i >= tree_nb) i = tree_nb - 1;
    return i;
}

static void build_tree_index(void) {
    free(tree_bstart); tree_bstart = NULL;
    free(tree_blist);  tree_blist = NULL;
    tree_nb = (int)(TERRAIN_WORLD / TREE_BUCKET) + 3;
    int nb = tree_nb * tree_nb;
    tree_bstart = (int *)calloc((size_t)nb + 1, sizeof(int));
    if (!tree_bstart) return;
    if (!tree_count) return;
    for (int i = 0; i < tree_count; i++)
        tree_bstart[tree_bucket_of(trees[i].z) * tree_nb + tree_bucket_of(trees[i].x)]++;
    int total = 0;
    for (int b = 0; b < nb; b++) { int c = tree_bstart[b]; tree_bstart[b] = total; total += c; }
    tree_bstart[nb] = total;
    tree_blist = (int *)malloc((size_t)(total ? total : 1) * sizeof(int));
    int *cur = (int *)malloc((size_t)nb * sizeof(int));
    if (!tree_blist || !cur) { free(cur); free(tree_blist); free(tree_bstart); tree_blist = NULL; tree_bstart = NULL; return; }
    memcpy(cur, tree_bstart, (size_t)nb * sizeof(int));
    for (int i = 0; i < tree_count; i++)
        tree_blist[cur[tree_bucket_of(trees[i].z) * tree_nb + tree_bucket_of(trees[i].x)]++] = i;
    free(cur);
}

/* Eight azimuth sectors of sky occlusion at a point on the surface: the
   highest thing in that direction, clamped. `with_trees` adds the crowns,
   which is what puts the ground in shade in a forest. */
static void horizon_at(float x, float z, float y, int with_trees, float *out) {
    const float step = 1.7f * CELL;
    const int STEPS = 18;
    for (int k = 0; k < SUN_SECTORS; k++) {
        float a = (float)k * 6.28318531f / (float)SUN_SECTORS;
        float dx = cosf(a), dz = sinf(a);
        float best = 0.0f;
        for (int i = 1; i <= STEPS; i++) {
            float t = step * (float)i;
            float g = sample_height(x + dx * t, z + dz * t);
            if (g < -1e8f) break;
            float ang = atan2f(g - y, t);
            if (ang > best) best = ang;
            if (best > 1.45f) break;
        }
        out[k] = best;
    }
    if (!with_trees || !tree_blist) return;
    const float reach = TREE_BUCKET;
    int bx = tree_bucket_of(x), bz = tree_bucket_of(z);
    for (int nz = bz - 1; nz <= bz + 1; nz++) {
        if (nz < 0 || nz >= tree_nb) continue;
        for (int nx = bx - 1; nx <= bx + 1; nx++) {
            if (nx < 0 || nx >= tree_nb) continue;
            int b = nz * tree_nb + nx;
            for (int q = tree_bstart[b]; q < tree_bstart[b + 1]; q++) {
                const tree_t *t = &trees[tree_blist[q]];
                float dx = x - t->x, dz = z - t->z;
                float d2 = dx * dx + dz * dz;
                if (d2 > reach * reach || d2 < 1e-4f) continue;
                float d = sqrtf(d2);
                float ang = atan2f(t->y + t->h * 0.92f - y, d);
                if (ang < 0.03f) continue;
                float az = atan2f(dz, dx);
                if (az < 0.0f) az += 6.28318531f;
                int sec = (int)floorf(az * (float)SUN_SECTORS / 6.28318531f + 0.5f) & (SUN_SECTORS - 1);
                /* spread into the neighbours at a fraction: without it the
                   shadow jumps between sectors as the sun crosses one */
                if (ang > out[sec]) out[sec] = ang;
                float half = ang * 0.55f;
                int a1 = (sec + 1) & (SUN_SECTORS - 1), a0 = (sec + SUN_SECTORS - 1) & (SUN_SECTORS - 1);
                if (half > out[a1]) out[a1] = half;
                if (half > out[a0]) out[a0] = half;
            }
        }
    }
}

/* Interpolated the same way the shader does it, for a point whose horizon is
   already in hand, and for the sampled world figure the log reports. */
static float shadow_lit(const float *h, float az, float elev) {
    float u = az * (float)SUN_SECTORS / 6.28318531f;
    if (u < 0.0f) u += (float)SUN_SECTORS;
    int k0 = (int)floorf(u) & (SUN_SECTORS - 1);
    int k1 = (k0 + 1) & (SUN_SECTORS - 1);
    float f = u - floorf(u);
    float occ = h[k0] * (1.0f - f) + h[k1] * f;
    float t = (elev - (occ - 0.03f)) / 0.06f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

/* How much of the sky this point can see, 0..1. Eight short rays at ~30
   degrees above horizontal: valleys, the base of the massif and the foot of a
   hill come back dark, ridges and open meadow bright. Baked once for the same
   reason as the sun shadow -- ambient occlusion is free at frame time if it is
   not evaluated at frame time. */
static float sky_occlusion(float x, float y, float z) {
    const int DIRS = 8, STEPS = 4;
    int hit = 0;
    for (int d = 0; d < DIRS; d++) {
        float a = (float)d * 6.28318531f / (float)DIRS;
        float dx = cosf(a), dz = sinf(a);
        for (int i = 1; i <= STEPS; i++) {
            float t = (float)i * 1.4f * CELL;
            float g = sample_height(x + dx * t, z + dz * t);
            if (g < -1e8f) break;
            if (g > y + t * 0.55f) { hit++; break; }
        }
    }
    return (float)hit / (float)DIRS;
}

/* deterministic per-facet hash, 0..1 */
static float facet_hash(int tri) {
    unsigned h = (unsigned)tri * 2654435761u;
    h ^= h >> 15;
    return (float)(h & 1023u) / 1023.0f;
}

/* Low-poly look comes from facets being *distinct*, not from smooth shading.
   Four discrete shade steps instead of a continuous multiplier: a gradient of
   the same size reads as smooth, four steps read as polygons. */
static const float FACET_SHADES[4] = { 0.64f, 0.86f, 1.10f, 1.32f };

/* Height of the drawn surface. The planar mesh below interpolates the same
   grid, so bilinear is the right answer for placement; the small sag inside a
   facet is what chunk_build() corrects afterwards. */
static float surface_height(float x, float z) {
    return sample_height(x, z);
}

/* River: a polyline walked down the mountain by steepest descent, then carved
   into the heightfield as a parabolic channel. Water is a ribbon emitted at the
   path height, so it always sits on its own bed instead of floating. */
#define RIVER_MAX 200
/* A river network is a list of SEGMENTS, not a polyline: tributaries join the
   trunk, so the drawing is a tree and "the previous point" is meaningless
   across a junction. Each segment carries its own water level and half width,
   which is what lets the channel narrow as it climbs. */
#define RIVER_MAX_SEG 1600
static float seg_x0[RIVER_MAX_SEG], seg_z0[RIVER_MAX_SEG];
static float seg_x1[RIVER_MAX_SEG], seg_z1[RIVER_MAX_SEG];
static float seg_w[RIVER_MAX_SEG], seg_hw[RIVER_MAX_SEG];
static int seg_n = 0;
static int river_count = 0;
static float river_lo = 0.0f, river_hi = 0.0f;
/* The channel has to be several cells wide or the grid can miss it: a trench
   narrower than CELL loses the carve between vertices and the water ribbon
   ends up lying on the un-carved surface. */
/* one colour for every water surface: the sea plane and the river painted
   into the terrain have to match or the mouth shows a seam */
static const float WATER_COL[3] = { 0.07f, 0.27f, 0.31f };

/* half width by drainage area: a stream at the top of a range is a few units
   wide, the trunk at the coast is 24. One constant width reads as a canal. */
#define RIVER_HW_MIN  7.0f
#define RIVER_HW_MAX 24.0f
#define RIVER_BANK   18.0f   /* bank reaches natural ground this far out */


/* Distance from (x,z) to the river centreline; *w gets the water level there.
   Returns 1e9 when there is no river. */
/* Bucket index over the segments. The carve and every per-facet water query
   used to walk all ~400 segments, so both were O(cells x segments) -- the
   second-largest cost in a big world after the horizon march, and the reason
   the river can never become a per-point function while it stays linear. Each
   segment registers in every bucket its box (grown by the carve's outer
   radius) touches, so a point close enough to care about always finds it. */
#define SEG_BUCKET 128.0f
static int seg_nb = 0;              /* buckets per axis */
static int *seg_bstart = NULL;      /* nb*nb+1 offsets into seg_blist */
static int *seg_blist = NULL;       /* CSR: segment indices per bucket */

static int seg_bucket_of(float v) {
    int i = (int)floorf((v + TERRAIN_WORLD * 0.5f + SEG_BUCKET) / SEG_BUCKET);
    if (i < 0) i = 0;
    if (i >= seg_nb) i = seg_nb - 1;
    return i;
}

static void build_seg_index(void) {
    free(seg_bstart); seg_bstart = NULL;
    free(seg_blist);  seg_blist = NULL;
    seg_nb = (int)(TERRAIN_WORLD / SEG_BUCKET) + 3;
    int nb = seg_nb * seg_nb;
    seg_bstart = (int *)calloc((size_t)nb + 1, sizeof(int));
    if (!seg_bstart) return;
    if (!seg_n) return;
    for (int i = 0; i < seg_n; i++) {
        float pad = seg_hw[i] + RIVER_BANK + 2.0f;
        int x0 = seg_bucket_of(fminf(seg_x0[i], seg_x1[i]) - pad);
        int x1 = seg_bucket_of(fmaxf(seg_x0[i], seg_x1[i]) + pad);
        int z0 = seg_bucket_of(fminf(seg_z0[i], seg_z1[i]) - pad);
        int z1 = seg_bucket_of(fmaxf(seg_z0[i], seg_z1[i]) + pad);
        for (int bz = z0; bz <= z1; bz++)
            for (int bx = x0; bx <= x1; bx++) seg_bstart[bz * seg_nb + bx]++;
    }
    int total = 0;
    for (int b = 0; b < nb; b++) { int c = seg_bstart[b]; seg_bstart[b] = total; total += c; }
    seg_bstart[nb] = total;
    seg_blist = (int *)malloc((size_t)(total ? total : 1) * sizeof(int));
    int *cur = (int *)malloc((size_t)nb * sizeof(int));
    if (!seg_blist || !cur) { free(cur); free(seg_blist); free(seg_bstart); seg_blist = NULL; seg_bstart = NULL; return; }
    memcpy(cur, seg_bstart, (size_t)nb * sizeof(int));
    for (int i = 0; i < seg_n; i++) {
        float pad = seg_hw[i] + RIVER_BANK + 2.0f;
        int x0 = seg_bucket_of(fminf(seg_x0[i], seg_x1[i]) - pad);
        int x1 = seg_bucket_of(fmaxf(seg_x0[i], seg_x1[i]) + pad);
        int z0 = seg_bucket_of(fminf(seg_z0[i], seg_z1[i]) - pad);
        int z1 = seg_bucket_of(fmaxf(seg_z0[i], seg_z1[i]) + pad);
        for (int bz = z0; bz <= z1; bz++)
            for (int bx = x0; bx <= x1; bx++) seg_blist[cur[bz * seg_nb + bx]++] = i;
    }
    free(cur);
}

/* Nearest point on the network. hw comes back as the half width there, so
   callers compare distance against the river's own width instead of a
   constant. Falls back to the full scan while the network is still being
   traced (the index can only be built once the segments are final). */
static float river_query(float x, float z, float *w, float *fx, float *fz,
                         float *hw) {
    float best = 1e18f;
    const int *list = NULL;
    int n = seg_n;
    if (seg_blist) {
        int b = seg_bucket_of(z) * seg_nb + seg_bucket_of(x);
        list = seg_blist + seg_bstart[b];
        n = seg_bstart[b + 1] - seg_bstart[b];
    }
    for (int k = 0; k < n; k++) {
        int i = list ? list[k] : k;
        float ax = seg_x0[i], az = seg_z0[i];
        float vx = seg_x1[i] - ax, vz = seg_z1[i] - az;
        float L2 = vx*vx + vz*vz;
        float t = L2 > 1e-9f ? ((x-ax)*vx + (z-az)*vz) / L2 : 0.0f;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        float dx = x - (ax + vx*t), dz = z - (az + vz*t);
        float d2 = dx*dx + dz*dz;
        if (d2 < best) {
            best = d2;
            if (w)  *w  = seg_w[i];
            if (hw) *hw = seg_hw[i];
            if (fx) { float il = L2 > 1e-9f ? 1.0f/sqrtf(L2) : 0.0f; *fx = vx*il; *fz = vz*il; }
        }
    }
    return seg_n ? sqrtf(best) : 1e9f;
}

/* Steepest descent with momentum: pure gradient descent stalls in every flat
   basin on the way down, momentum carries it through. The river is only kept
   if it actually reaches the sea, so a failed attempt is simply retried with a
   different heading. */
/* --- hydrology -------------------------------------------------------------
   Rivers run where water actually goes. Two passes over the eroded grid:

   1. Priority-flood. Every pit is filled to its spill height. A noise field is
      full of pits, and a pit with no outlet is not a lake, it is a hole in the
      watershed -- filling gives each one an outlet, and the fill depth is
      exactly the lake surface. Lakes shallower than a cell or smaller than a
      pond are dropped back to the ground, or the island is peppered with
      puddles.
   2. Flow accumulation (D8). Each cell drains into its steepest neighbour;
      summing the upstream count gives the trunk of each watershed, and the
      biggest trunk is the river.

   Both are O(n log n) on the 74x74 grid, once, at load. ponytail: one trunk,
   extracted from the largest accumulation; tributaries are the natural next
   step and need a threshold on the accumulation, not new machinery. */
#define HEAP_MAX (N * N * 4)
static float heap_k[HEAP_MAX];
static int   heap_i[HEAP_MAX];
static int   heap_n = 0;
static float h_src[N * N];       /* heights before the fill */
static float h_fill[N * N];      /* heights after it: the water surface */
static unsigned char lake_cell[N * N];
static int h_order[N * N], h_down[N * N];
static float h_acc[N * N];

static void heap_push(float k, int i) {
    if (heap_n >= HEAP_MAX) return;
    int c = heap_n++;
    while (c > 0) {
        int p = (c - 1) / 2;
        if (heap_k[p] <= k) break;
        heap_k[c] = heap_k[p]; heap_i[c] = heap_i[p];
        c = p;
    }
    heap_k[c] = k; heap_i[c] = i;
}

static int heap_pop(float *k) {
    int top = heap_i[0];
    *k = heap_k[0];
    if (--heap_n > 0) {
        float kk = heap_k[heap_n];
        int ii = heap_i[heap_n], c = 0;
        for (;;) {
            int l = 2 * c + 1, r = l + 1, m = c;
            float mk = kk;
            if (l < heap_n && heap_k[l] < mk) { m = l; mk = heap_k[l]; }
            if (r < heap_n && heap_k[r] < mk) { m = r; mk = heap_k[r]; }
            if (m == c) break;
            heap_k[c] = heap_k[m]; heap_i[c] = heap_i[m];
            c = m;
        }
        heap_k[c] = kk; heap_i[c] = ii;
    }
    return top;
}

static int h_cmp(const void *a, const void *b) {
    float ha = h_fill[*(const int *)a], hb = h_fill[*(const int *)b];
    return ha < hb ? 1 : (ha > hb ? -1 : 0);      /* descending */
}

#define MIN_LAKE_CELLS 5        /* ~1100 world units of surface */
#define LAKE_DEPTH 0.45f

static void build_hydrology(void) {
    for (int i = 0; i < N * N; i++) {
        h_src[i] = h_fill[i] = heights[i];
        lake_cell[i] = 0;
    }
    /* --- 1: priority-flood from every cell that already drains --- */
    heap_n = 0;
    for (int i = 0; i < N * N; i++) {
        h_fill[i] = 3.0e38f;
    }
    for (int zi = 0; zi < N; zi++) {
        for (int xi = 0; xi < N; xi++) {
            int i = zi * N + xi;
            if (heights[i] > 0.0f && xi && zi && xi < N - 1 && zi < N - 1) continue;
            h_fill[i] = heights[i];
            heap_push(heights[i], i);
        }
    }
    while (heap_n > 0) {
        float h0;
        int c = heap_pop(&h0);
        int cx = c % N, cz = c / N;
        for (int dz = -1; dz <= 1; dz++) {
            for (int dx = -1; dx <= 1; dx++) {
                if (!dx && !dz) continue;
                int nx = cx + dx, nz = cz + dz;
                if (nx < 0 || nz < 0 || nx >= N || nz >= N) continue;
                int n = nz * N + nx;
                if (h_fill[n] < 3.0e37f) continue;
                float f = heights[n] > h0 ? heights[n] : h0;
                h_fill[n] = f;
                if (f > heights[n] + LAKE_DEPTH) lake_cell[n] = 1;
                heap_push(f, n);
            }
        }
    }
    /* drop the ponds: connected components below the minimum area go back to
       being ground, or the meadow is covered in puddles */
    {
        static int stack[N * N];
        static unsigned char seen[N * N];
        for (int i = 0; i < N * N; i++) seen[i] = 0;
        for (int i = 0; i < N * N; i++) {
            if (!lake_cell[i] || seen[i]) continue;
            int sp = 0, cnt = 0;
            stack[sp++] = i; seen[i] = 1;
            while (sp > 0) {
                int c = stack[--sp];
                cnt++;
                int cx = c % N, cz = c / N;
                for (int dz = -1; dz <= 1; dz++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int nx = cx + dx, nz = cz + dz;
                        if (nx < 0 || nz < 0 || nx >= N || nz >= N) continue;
                        int n = nz * N + nx;
                        if (!lake_cell[n] || seen[n]) continue;
                        seen[n] = 1;
                        stack[sp++] = n;
                    }
            }
            if (cnt < MIN_LAKE_CELLS) {
                /* second pass over the component to clear it */
                int sp2 = 0;
                stack[sp2++] = i;
                lake_cell[i] = 2;          /* 2 = visited-and-rejected */
                while (sp2 > 0) {
                    int c = stack[--sp2];
                    lake_cell[c] = 0;
                    h_fill[c] = h_src[c];
                    int cx = c % N, cz = c / N;
                    for (int dz = -1; dz <= 1; dz++)
                        for (int dx = -1; dx <= 1; dx++) {
                            int nx = cx + dx, nz = cz + dz;
                            if (nx < 0 || nz < 0 || nx >= N || nz >= N) continue;
                            int n = nz * N + nx;
                            if (lake_cell[n] != 1) continue;
                            lake_cell[n] = 2;
                            stack[sp2++] = n;
                        }
                }
            } else {
                for (int k = 0; k < N * N; k++) if (lake_cell[k] == 2) lake_cell[k] = 1;
            }
        }
        for (int i = 0; i < N * N; i++) if (lake_cell[i] == 2) lake_cell[i] = 1;
    }
    /* the drawn surface has to be the water surface, flat, or the lake is a
       sloped sheet painted blue */
    for (int i = 0; i < N * N; i++)
        if (lake_cell[i]) heights[i] = h_fill[i];

    /* --- 2: a strictly descending surface to steer the flow ---
       Filled ground is FLAT, and D8 needs a strict descent: on a flat basin
       every cell has "no lower neighbour", so the river stops exactly where the
       lake starts. Walk outward from the cells that already drain (the sea and
       anything with a lower neighbour) and lift each flat cell a hair above the
       one it came from. The result is monotone everywhere, and the lift is
       1e-3 -- invisible in the terrain, decisive for the flow. */
    static float h_drain[N * N];
    static int queue[N * N];
    for (int i = 0; i < N * N; i++) h_drain[i] = h_fill[i];
    {
        int qh = 0, qt = 0;
        static unsigned char queued[N * N];
        for (int i = 0; i < N * N; i++) queued[i] = 0;
        for (int i = 0; i < N * N; i++) {
            int cx = i % N, cz = i / N;
            int lower = h_fill[i] <= 0.0f;
            for (int dz = -1; dz <= 1 && !lower; dz++)
                for (int dx = -1; dx <= 1 && !lower; dx++) {
                    if (!dx && !dz) continue;
                    int nx = cx + dx, nz = cz + dz;
                    if (nx < 0 || nz < 0 || nx >= N || nz >= N) continue;
                    if (h_fill[nz * N + nx] < h_fill[i]) lower = 1;
                }
            if (lower) { queue[qt++] = i; queued[i] = 1; }
        }
        while (qh < qt) {
            int c = queue[qh++];
            int cx = c % N, cz = c / N;
            for (int dz = -1; dz <= 1; dz++)
                for (int dx = -1; dx <= 1; dx++) {
                    if (!dx && !dz) continue;
                    int nx = cx + dx, nz = cz + dz;
                    if (nx < 0 || nz < 0 || nx >= N || nz >= N) continue;
                    int n = nz * N + nx;
                    if (queued[n]) continue;
                    if (h_fill[n] != h_fill[c]) continue;     /* only flats */
                    h_drain[n] = h_drain[c] + 1.0e-3f;
                    queued[n] = 1;
                    queue[qt++] = n;
                }
        }
    }

    /* --- 3: flow accumulation --- */
    for (int i = 0; i < N * N; i++) {
        h_order[i] = i;
        h_acc[i] = 1.0f;
        h_down[i] = -1;
        if (h_fill[i] <= 0.0f) continue;           /* the sea does not drain */
        int cx = i % N, cz = i / N;
        int best = -1;
        /* Flow runs on the FILLED surface, not the drawn one: a pit shallower
           than a lake still has to drain, and on the raw heights it is a
           dead end that stops the river in the middle of the meadow. */
        /* Steepest descent by SLOPE, not by height: a diagonal step is 1.41x
           longer, and on the sub-millimetre gradient a filled basin has, a
           fixed height penalty of any size closes the outlet entirely and the
           river stops in the meadow. */
        float best_slope = 0.0f;
        for (int dz = -1; dz <= 1; dz++) {
            for (int dx = -1; dx <= 1; dx++) {
                if (!dx && !dz) continue;
                int nx = cx + dx, nz = cz + dz;
                if (nx < 0 || nz < 0 || nx >= N || nz >= N) continue;
                int n = nz * N + nx;
                float dist = (dx && dz) ? 1.41421356f : 1.0f;
                float sl = (h_drain[i] - h_drain[n]) / dist;
                if (sl > best_slope) { best_slope = sl; best = n; }
            }
        }
        h_down[i] = best;
    }
    qsort(h_order, N * N, sizeof(int), h_cmp);
    for (int k = 0; k < N * N; k++) {
        int i = h_order[k];
        if (h_down[i] >= 0) h_acc[h_down[i]] += h_acc[i];
    }

    /* --- 4: the network ---
       Trunk first, then the biggest tributaries that are not already on it.
       Every river is traced downstream from its spring, so a tributary ends
       exactly where it joins the trunk instead of crossing it. */
    seg_n = 0;
    river_count = 0;
    river_lo = 1e9f;
    river_hi = -1e9f;
    int mouth = -1;
    float best_acc = 0.0f;
    for (int i = 0; i < N * N; i++) {
        if (h_fill[i] <= 0.5f) continue;
        if (h_acc[i] > best_acc) { best_acc = h_acc[i]; mouth = i; }
    }
    if (mouth < 0 || best_acc < 12.0f) return;   /* no watershed worth drawing */

    static int path[RIVER_MAX];
    for (int r = 0; r < 12 && seg_n < RIVER_MAX_SEG - 4; r++) {
        int start = mouth;
        if (r > 0) {
            /* the next biggest drainage that is not already in the network */
            int best = -1;
            float ba = 0.0f;
            for (int i = 0; i < N * N; i++) {
                if (h_fill[i] <= 0.5f || h_acc[i] < 18.0f || h_acc[i] <= ba) continue;
                float x = (i % N) / (float)(N - 1) * TERRAIN_WORLD - TERRAIN_WORLD * 0.5f;
                float z = (i / N) / (float)(N - 1) * TERRAIN_WORLD - TERRAIN_WORLD * 0.5f;
                if (river_query(x, z, NULL, NULL, NULL, NULL) < 26.0f) continue;
                best = i; ba = h_acc[i];
            }
            if (best < 0) break;
            start = best;
        }
        /* upstream to the spring */
        static int up[RIVER_MAX];
        int nu = 0;
        int c = start;
        while (nu < RIVER_MAX) {
            up[nu++] = c;
            int cx = c % N, cz = c / N, prev = -1;
            float pa = 0.0f;
            for (int dz = -1; dz <= 1; dz++)
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = cx + dx, nz = cz + dz;
                    if (nx < 0 || nz < 0 || nx >= N || nz >= N) continue;
                    int n = nz * N + nx;
                    if (h_down[n] != c) continue;
                    if (h_acc[n] > pa) { pa = h_acc[n]; prev = n; }
                }
            /* A river starts where its catchment is big enough to feed one,
               not at the summit: tracing to the top draws a sheet of water
               straight down the mountain face and reads as a waterfall. */
            if (prev < 0 || h_acc[prev] < 6.0f) break;
            c = prev;
        }
        int np = 0;
        for (int k = nu - 1; k >= 0 && np < RIVER_MAX; k--) path[np++] = up[k];
        /* then down: to the sea for the trunk, to the network for a tributary */
        int c2 = start;
        while (np < RIVER_MAX) {
            int dn = h_down[c2];
            if (dn < 0) break;
            if (r > 0) {
                float x = (dn % N) / (float)(N - 1) * TERRAIN_WORLD - TERRAIN_WORLD * 0.5f;
                float z = (dn / N) / (float)(N - 1) * TERRAIN_WORLD - TERRAIN_WORLD * 0.5f;
                if (river_query(x, z, NULL, NULL, NULL, NULL) < 10.0f) { path[np++] = dn; break; }
            }
            c2 = dn;
            path[np++] = c2;
            if (h_fill[c2] <= 0.0f) break;
        }
        for (int k = 0; k + 1 < np && seg_n < RIVER_MAX_SEG; k++) {
            int a = path[k], b = path[k + 1];
            seg_x0[seg_n] = (a % N) / (float)(N - 1) * TERRAIN_WORLD - TERRAIN_WORLD * 0.5f;
            seg_z0[seg_n] = (a / N) / (float)(N - 1) * TERRAIN_WORLD - TERRAIN_WORLD * 0.5f;
            seg_x1[seg_n] = (b % N) / (float)(N - 1) * TERRAIN_WORLD - TERRAIN_WORLD * 0.5f;
            seg_z1[seg_n] = (b / N) / (float)(N - 1) * TERRAIN_WORLD - TERRAIN_WORLD * 0.5f;
            seg_w[seg_n] = h_fill[a];
            /* width follows drainage area, so the channel narrows as it climbs
               instead of running as a canal from the summit to the sea */
            float t = sqrtf(h_acc[a] / best_acc);
            seg_hw[seg_n] = RIVER_HW_MIN + (RIVER_HW_MAX - RIVER_HW_MIN) * t;
            if (h_fill[a] > river_hi) river_hi = h_fill[a];
            if (h_fill[a] < river_lo) river_lo = h_fill[a];
            seg_n++;
        }
        river_count++;
    }
}

static int lake_at(float x, float z) {
    int xi = (int)((x + TERRAIN_WORLD * 0.5f) / CELL);
    int zi = (int)((z + TERRAIN_WORLD * 0.5f) / CELL);
    if (xi < 0 || zi < 0 || xi >= N || zi >= N) return 0;
    return lake_cell[zi * N + xi];
}

/* Cut the channel. Inside the local half width the bed lies at the water level,
   so the water painted on this same mesh meets the ground exactly at its own
   edge instead of ending in a visible step; outside it the ground blends back
   up to untouched height over RIVER_BANK. Width comes from the segment, so the
   channel narrows as it climbs. */
static void carve_river(void) {
    if (!seg_n) return;
    for (int zi = 0; zi < N; zi++) {
        for (int xi = 0; xi < N; xi++) {
            float x = (xi / (float)(N - 1)) * TERRAIN_WORLD - TERRAIN_WORLD * 0.5f;
            float z = (zi / (float)(N - 1)) * TERRAIN_WORLD - TERRAIN_WORLD * 0.5f;
            int i = zi * N + xi;
            /* Inside a lake the surface is already flat at the water level and
               the river runs *through* it, not over it: carving there dug a
               trench across the lake bed and the ripple-shaded river facets
               showed up as a striped ribbon lying on the still water. */
            if (lake_cell[i]) continue;
            float w = 0.0f, hw = RIVER_HW_MIN;
            float d = river_query(x, z, &w, NULL, NULL, &hw);
            if (d > hw + RIVER_BANK) continue;
            float h = heights[i];
            if (d <= hw) {
                /* the bed lies *at* the water level and the water is painted on
                   this same mesh -- a separate water ribbon cannot match a
                   channel carved on a coarser grid, and every mismatch shows up
                   as a sliver of sand sticking through the surface */
                heights[i] = w;
            } else {
                float u = (d - hw) / RIVER_BANK;
                if (u > 1.0f) u = 1.0f;
                float target = w + (h - w) * u;
                if (target < h) heights[i] = target;   /* carve only, never raise */
            }
        }
    }
}

/* One flat-shaded triangle for the props. Winding normal is flipped to face
   away from `inner`. The shade is baked into the colour here and the shader
   gets a normal blended toward the sun, so canopies darken on their far side
   without dropping below the ambient floor. */
static void emit_tri(const float *p0, const float *p1, const float *p2,
                     const float *inner, const float *base, float jitter) {
    float u[3] = { p1[0]-p0[0], p1[1]-p0[1], p1[2]-p0[2] };
    float w[3] = { p2[0]-p0[0], p2[1]-p0[1], p2[2]-p0[2] };
    float n[3] = { u[1]*w[2]-u[2]*w[1], u[2]*w[0]-u[0]*w[2], u[0]*w[1]-u[1]*w[0] };
    float l = sqrtf(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
    if (l < 1e-9f) return;
    n[0] /= l; n[1] /= l; n[2] /= l;
    if (inner) {
        float cx = (p0[0]+p1[0]+p2[0]) * (1.0f/3.0f) - inner[0];
        float cy = (p0[1]+p1[1]+p2[1]) * (1.0f/3.0f) - inner[1];
        float cz = (p0[2]+p1[2]+p2[2]) * (1.0f/3.0f) - inner[2];
        if (cx*n[0] + cy*n[1] + cz*n[2] < 0.0f) {
            n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2];
        }
    }
    /* per-facet shading against straight up, not against a sun that moves:
       canopies keep their volume and their lighting follows the clock */
    float upn = n[1] > 0.0f ? n[1] : 0.0f;
    float s = (0.45f + 0.72f * upn) * jitter;
    float col[3] = { base[0]*s, base[1]*s, base[2]*s };
    /* Handed to the shader as a blend of the true facet normal and straight up:
       the true normal alone puts every side face below the ambient floor and the
       tree goes black, and biasing it toward a FIXED sun only worked while the
       sun did not move. Up is the average direction of a round canopy, so it is
       the honest bias now that the light follows the clock. */
    float nv[3] = { n[0]*0.60f, n[1]*0.60f + 0.40f, n[2]*0.60f };
    float nl2 = sqrtf(nv[0]*nv[0] + nv[1]*nv[1] + nv[2]*nv[2]);
    if (nl2 > 1e-6f) { nv[0]/=nl2; nv[1]/=nl2; nv[2]/=nl2; }
    for (int k = 0; k < 3; k++) {
        const float *p = (k == 0) ? p0 : (k == 1) ? p1 : p2;
        /* no sway at the roots, full at the tips, so the canopy bends and the
           trunk base stays planted in the ground */
        float w = (p[1] - sway_base) / sway_h;
        if (w < 0.0f) w = 0.0f;
        if (w > 1.0f) w = 1.0f;
        w = w * w * sqrtf(w);
        push_vert(p, nv, col, w, sway_phase, 0.0f, 0.0f, sway_shadow, sway_bounce);
    }
}

/* Canopy/trunk colours measured off the reference (#758f50 canopy, #706050
   trunk), divided by the shader's constant factor so they land on target. */
static const float LEAF_COL[3]  = { 0.354f, 0.466f, 0.096f };
/* one green per species, spread in hue as well as value: five shades of the
   same green would read as one species with noise */
static const float FIR_COL[3]      = { 0.205f, 0.330f, 0.115f };
static const float SPIRE_COL[3]    = { 0.290f, 0.400f, 0.090f };
static const float BROADLEAF_COL[3]= { 0.300f, 0.425f, 0.150f };
static const float ACACIA_COL[3]   = { 0.400f, 0.480f, 0.130f };
/* warm, like the reference sheet's trunks; the old grey-brown vanished into
   the shadow side of the canopy */
static const float TRUNK_COL[3] = { 0.330f, 0.225f, 0.130f };

/* Jittered grid of candidates, thinned by slope: flat ground keeps almost all
   of them (forests), steep ground almost none. */
/* Water is painted per facet, and a facet is up to ~40 units across while the
   channel is 48 wide, so the painted water spreads a facet radius past the
   shore -- which is why trees used to stand in the middle of lakes. Keep trees
   a facet away from any water instead of testing the point exactly. */
static int near_water(float x, float z) {
    float hw = RIVER_HW_MIN;
    if (river_query(x, z, NULL, NULL, NULL, &hw) < hw + 18.0f) return 1;
    static const float DX[9] = { 0, 1, -1, 0, 0, 1, 1, -1, -1 };
    static const float DZ[9] = { 0, 0, 0, 1, -1, 1, -1, 1, -1 };
    for (int k = 0; k < 9; k++)
        if (lake_at(x + DX[k] * CELL, z + DZ[k] * CELL)) return 1;
    return 0;
}

static void place_trees(unsigned seed) {
    /* spacing must exceed the canopy diameter (~0.57 * height) or neighbouring
       trees merge into one silhouette -- the reference shows isolated crowns */
    const float spacing = 20.0f;
    int g = (int)(TERRAIN_WORLD / spacing);
    tree_count = 0;
    for (int gz = 0; gz < g; gz++) {
        for (int gx = 0; gx < g; gx++) {
            if (tree_count >= MAX_TREES) return;
            unsigned h = terrain_hash(gx, gz, seed ^ 0x9e3779b9u);
            float r0 = (float)( h        & 1023u) / 1023.0f;
            float r1 = (float)((h >> 10) & 1023u) / 1023.0f;
            float r2 = (float)((h >> 20) & 1023u) / 1023.0f;
            /* jitter within half a cell: a full-cell jitter lets neighbours
               coincide, which merges crowns into one wide blob */
            float x = (gx + 0.5f + (r0 - 0.5f) * 0.55f) * spacing - TERRAIN_WORLD * 0.5f;
            float z = (gz + 0.5f + (r1 - 0.5f) * 0.55f) * spacing - TERRAIN_WORLD * 0.5f;
            float y = surface_height(x, z);
            /* the treeline follows the summit, not a fixed altitude: a fixed
               one either buries the peak in forest or leaves the lowland bare
               as soon as the amplitude changes */
            float treeline = 0.40f * peak_y;
            if (y < 5.0f || y > treeline) continue;   /* no water, sand or snow */
            if (near_water(x, z)) continue;
            float e = CELL;
            float sx = surface_height(x + e, z) - surface_height(x - e, z);
            float sz = surface_height(x, z + e) - surface_height(x, z - e);
            float slope = sqrtf(sx*sx + sz*sz) / (2.0f * e);   /* tan of the tilt */
            if (slope > 0.90f) continue;
            /* species mix follows altitude: broadleaf and umbrella trees in
               the lowland, firs and spires up toward the treeline -- the
               reference island reads the same way from above */
            float alt = (y - 5.0f) / (treeline - 5.0f);
            if (alt < 0.0f) alt = 0.0f;
            if (alt > 1.0f) alt = 1.0f;
            /* thin out toward the line: a hard edge reads as someone switching
               a forest off halfway up the slope */
            float fade = 1.0f;
            if (alt > 0.62f) {
                fade = 1.0f - (alt - 0.62f) / 0.38f;
                if (fade < 0.0f) fade = 0.0f;
            }
            if (r2 > (0.62f + 0.33f * (1.0f - slope / 0.90f)) * fade) continue;
            unsigned sh = terrain_hash(gx * 3 + 1, gz * 5 + 2, seed ^ 0x2545f491u);
            float sp = (float)((sh >> 8) & 1023u) / 1023.0f;
            float t_fir  = 0.10f + 0.30f * alt;
            float t_spire = t_fir + 0.12f + 0.02f * alt;
            float t_pine = t_spire + 0.42f - 0.25f * alt;
            float t_broad = t_pine + 0.22f - 0.18f * alt;
            if (t_broad > 0.95f) t_broad = 0.95f;
            int kind;
            if (sp < t_fir)        kind = TREE_FIR;
            else if (sp < t_spire) kind = TREE_SPIRE;
            else if (sp < t_pine)  kind = TREE_PINE;
            else if (sp < t_broad) kind = TREE_BROADLEAF;
            else                   kind = TREE_ACACIA;

            tree_t *t = &trees[tree_count++];
            t->x = x; t->z = z; t->y = y;
            t->kind = (unsigned char)kind;
            switch (kind) {
            case TREE_FIR:       t->h = 15.0f + r0 * 12.0f; t->r = 0.16f * t->h; break;
            case TREE_SPIRE:     t->h = 16.0f + r0 * 13.0f; t->r = 0.13f * t->h; break;
            case TREE_BROADLEAF: t->h = 10.0f + r0 *  7.0f; t->r = 0.30f * t->h; break;
            case TREE_ACACIA:    t->h = 12.0f + r0 *  7.0f; t->r = 0.40f * t->h; break;
            default:             t->h = 12.0f + r0 * 12.0f; t->r = 0.22f * t->h; break;
            }
            t->hash = h;
        }
    }
}

/* 6-sided trunk from `by` up `th`. Shared by every species; only the height
   and radius change, which is what makes a bare poplar trunk and a hidden
   conifer trunk cost the same code. */
static void emit_trunk(const tree_t *t, float by, float th, float tr) {
    const int SIDES = 6;
    float inner[3] = { t->x, by + th * 0.5f, t->z };
    unsigned h = t->hash;
    for (int s = 0; s < SIDES; s++) {
        float a0 = (float)s * 6.28318531f / SIDES;
        float a1 = (float)(s + 1) * 6.28318531f / SIDES;
        float q[4][3] = {
            { t->x + cosf(a0)*tr, by, t->z + sinf(a0)*tr },
            { t->x + cosf(a1)*tr, by, t->z + sinf(a1)*tr },
            { t->x + cosf(a1)*tr, by + th, t->z + sinf(a1)*tr },
            { t->x + cosf(a0)*tr, by + th, t->z + sinf(a0)*tr },
        };
        float j = 0.93f + 0.14f * facet_hash(s + (int)(h & 4095u));
        emit_tri(q[0], q[1], q[2], inner, TRUNK_COL, j);
        emit_tri(q[0], q[2], q[3], inner, TRUNK_COL, j);
    }
}

/* One conical tier: a lobed rim ring under an apex. Stacked at different
   radii these are the conifer silhouettes. */
static void emit_tier(const tree_t *t, float by, float y0, float y1, float rr,
                      float H, const float *col, int k) {
    const int SIDES = 6;
    unsigned h = t->hash;
    float ring[6][3];
    for (int s = 0; s < SIDES; s++) {
        float a = (float)s * 6.28318531f / SIDES + (float)k * 0.55f;
        unsigned jh = terrain_hash(s, k, h);
        float wob = 0.92f + 0.16f * ((jh & 255u) / 255.0f);
        ring[s][0] = t->x + cosf(a) * rr * wob;
        ring[s][1] = y0 + (((jh >> 8) & 127u) / 127.0f - 0.5f) * 0.04f * H;
        ring[s][2] = t->z + sinf(a) * rr * wob;
    }
    float apex[3] = { t->x, y1, t->z };
    float mid[3] = { t->x, (y0 + y1) * 0.5f, t->z };
    for (int s = 0; s < SIDES; s++) {
        int s2 = (s + 1) % SIDES;
        float j = 0.88f + 0.24f * facet_hash((int)(h & 4095u) + k * 8 + s);
        emit_tri(ring[s], ring[s2], apex, mid, col, j);
    }
    (void)by;
}

/* Low-poly blob: two rings plus a pole at each end, 24 flat triangles. This is
   the broadleaf crown, the poplar spire and the umbrella disc -- only the
   radii change, so all three cost one function. */
static void emit_blob(const tree_t *t, float cx, float cy, float cz,
                      float rx, float ry, float rz, const float *col, int lobe) {
    const int SIDES = 6;
    const float PHI[3] = { -0.80f, 0.0f, 0.80f };
    float ring[3][6][3];
    for (int b = 0; b < 3; b++) {
        float sy = sinf(PHI[b]), cr = cosf(PHI[b]);
        for (int s = 0; s < SIDES; s++) {
            float a = (float)s * 6.28318531f / SIDES
                    + ((terrain_hash(s, b + lobe, t->hash) & 255u) / 255.0f - 0.5f) * 0.10f;
            float wob = 0.94f + 0.12f * ((terrain_hash(s + 7, b + lobe, t->hash) & 255u) / 255.0f);
            ring[b][s][0] = cx + cosf(a) * rx * cr * wob;
            ring[b][s][1] = cy + sy * ry * wob;
            ring[b][s][2] = cz + sinf(a) * rz * cr * wob;
        }
    }
    float top[3] = { cx, cy + ry, cz };
    float bot[3] = { cx, cy - ry, cz };
    float inner[3] = { cx, cy, cz };
    for (int s = 0; s < SIDES; s++) {
        int s2 = (s + 1) % SIDES;
        float j = 0.90f + 0.20f * facet_hash((int)(t->hash & 2047u) + lobe * 16 + s);
        emit_tri(bot, ring[0][s2], ring[0][s], inner, col, j);       /* underside */
        for (int b = 0; b < 2; b++) {
            emit_tri(ring[b][s], ring[b][s2], ring[b+1][s2], inner, col, j);
            emit_tri(ring[b][s], ring[b+1][s2], ring[b+1][s], inner, col, j);
        }
        emit_tri(ring[2][s], ring[2][s2], top, inner, col, j);       /* crown */
    }
}

static int tree_detail = 0;   /* 0 = full geometry, 1 = one cone */

static void emit_tree(const tree_t *t) {
    float H = t->h;
    unsigned h = t->hash;
    float by = t->y - 2.5f;            /* sunk: grid skew/jitter can lift the mesh */
    float tint = 0.86f + 0.28f * ((h & 255u) / 255.0f);   /* per-tree variation */
    const float *base = LEAF_COL;
    switch (t->kind) {
    case TREE_FIR:       base = FIR_COL;       break;
    case TREE_SPIRE:     base = SPIRE_COL;     break;
    case TREE_BROADLEAF: base = BROADLEAF_COL; break;
    case TREE_ACACIA:    base = ACACIA_COL;    break;
    default: break;
    }
    float leaf[3] = { base[0]*tint, base[1]*tint, base[2]*tint };
    sway_base = by;
    sway_h = H * 1.02f;
    sway_phase = (float)((h >> 8) & 1023u) / 1023.0f;
    /* Far trees are a few pixels across, and they are 92% of the vertex
       stream. One tier instead of three to five, same shading path so they
       still take the baked shadow and sway. ponytail: one silhouette for all
       species -- at this distance none of the difference lands on a pixel. */
    if (tree_detail) {
        emit_tier(t, by, by + H * 0.18f, by + H, t->r * 1.30f, H, leaf, 0);
        return;
    }

    /* three overlapping conical skirts with a lobed rim: a cone silhouette
       with visible tiers reads as pine at any size, a smooth egg reads as blob */
    static const float PINE_BASE[3] = { 0.16f, 0.42f, 0.66f };
    static const float PINE_R[3]    = { 1.00f, 0.72f, 0.44f };
    static const float PINE_TOP[3]  = { 0.56f, 0.80f, 1.00f };
    /* slender conifer: four tighter plates on a taller trunk */
    static const float FIR_BASE[4]  = { 0.12f, 0.34f, 0.55f, 0.74f };
    static const float FIR_R[4]     = { 1.00f, 0.80f, 0.58f, 0.32f };
    static const float FIR_TOP[4]   = { 0.42f, 0.62f, 0.82f, 1.00f };
    const float TIER[4] = { 0.84f, 0.93f, 1.02f, 1.10f };

    switch (t->kind) {
    case TREE_PINE:
        emit_trunk(t, by, H * 0.20f, t->r * 0.14f);
        for (int k = 0; k < 3; k++) {
            float tc = TIER[k];
            float lc[3] = { leaf[0]*tc, leaf[1]*tc, leaf[2]*tc };
            emit_tier(t, by, by + PINE_BASE[k]*H, by + PINE_TOP[k]*H,
                      t->r * PINE_R[k], H, lc, k);
        }
        break;

    case TREE_FIR:
        emit_trunk(t, by, H * 0.16f, t->r * 0.15f);
        for (int k = 0; k < 4; k++) {
            float tc = TIER[k];
            float lc[3] = { leaf[0]*tc, leaf[1]*tc, leaf[2]*tc };
            emit_tier(t, by, by + FIR_BASE[k]*H, by + FIR_TOP[k]*H,
                      t->r * FIR_R[k], H, lc, k);
        }
        break;

    case TREE_SPIRE: {
        /* tall narrow crown on a bare trunk: the sheet's poplar. ry carries
           the height, so the trunk stays visible underneath */
        float th = H * 0.38f;
        emit_trunk(t, by, th, t->r * 0.16f);
        float ry = H * 0.30f;
        emit_blob(t, t->x, by + th + ry * 0.85f, t->z,
                  t->r, ry, t->r * 0.85f, leaf, 0);
        break;
    }

    case TREE_BROADLEAF: {
        float th = H * 0.42f;
        emit_trunk(t, by, th, t->r * 0.19f);
        float ry = t->r * 1.15f;
        emit_blob(t, t->x, by + th + ry * 0.80f, t->z, t->r, ry, t->r, leaf, 0);
        break;
    }

    case TREE_ACACIA:
        /* flat umbrella disc over a thick short trunk, plus one offset lobe so
           the crown is not a symmetrical mushroom */
        emit_trunk(t, by, H * 0.52f, t->r * 0.22f);
        {
            float ry = t->r * 0.42f;
            float cy = by + H * 0.52f + ry * 0.9f;
            emit_blob(t, t->x, cy, t->z, t->r * 1.20f, ry, t->r, leaf, 0);
            emit_blob(t, t->x + t->r * 0.55f, cy + ry * 0.9f, t->z - t->r * 0.35f,
                      t->r * 0.62f, ry * 0.75f, t->r * 0.58f, leaf, 1);
        }
        break;

    default:
        break;
    }
}

/* Clouds are lit flat, with one upward normal and a per-facet jitter, instead
   of per-facet normals: real normals turn the undersides into rock. */
static void emit_flat_tri(const float *p0, const float *p1, const float *p2,
                          const float *base, float jitter, float phase) {
    float c[3] = { base[0]*jitter, base[1]*jitter, base[2]*jitter };
    for (int k = 0; k < 3; k++) {
        const float *p = (k == 0) ? p0 : (k == 1) ? p1 : p2;
        /* negative weight: the vertex shader drifts these instead of swaying */
        const float cup[3] = { 0.0f, 1.0f, 0.0f };
        push_vert(p, cup, c, -1.0f, phase, 0.0f, 0.0f, NULL, NULL);
    }
}

/* Low-poly cloud: a cluster of squashed 6-sided ellipsoids. Same emit_tri path
   as everything else, so clouds cost no extra pipeline and shade with the same
   sun -- which is what makes them read as faceted volumes instead of sprites. */
static void emit_cloud(unsigned seed) {
    const int NCLOUDS = 4, LOBES = 5;
    for (int c = 0; c < NCLOUDS; c++) {
        unsigned ch = terrain_hash(c, 91, seed);
        /* inside the view cone: clouds scattered over the whole island were
           almost never in frame */
        float ang = yaw + (((float)(ch & 1023u) / 1023.0f) - 0.5f) * 1.6f;
        float rad = (0.55f + 0.30f * (float)((ch >> 10) & 1023u) / 1023.0f) * TERRAIN_WORLD;
        float cx = cosf(ang) * rad;
        float cz = sinf(ang) * rad;
        float cy = peak_y * (1.25f + 0.45f * (float)((ch >> 20) & 255u) / 255.0f);
        float size = 34.0f + 26.0f * (float)((ch >> 4) & 255u) / 255.0f;
        float phase = (float)((ch >> 16) & 255u) / 255.0f;
        for (int l = 0; l < LOBES; l++) {
            unsigned lh = terrain_hash(c * 17 + l, 7, seed);
            float ox = ((float)(lh & 255u) / 255.0f - 0.5f) * 2.2f * size;
            float oz = ((float)((lh >> 8) & 255u) / 255.0f - 0.5f) * size;
            float rx = size * (0.6f + 0.5f * (float)((lh >> 16) & 127u) / 127.0f);
            float ry = rx * 0.55f;
            float rz = rx * 0.85f;
            const float col[3] = { 0.97f, 0.95f, 0.90f };
            /* 6 sides x 5 bands, closed near the pole: rings must line up
               (no per-band rotation) or the quads twist into streaks */
            float mid[3] = { cx + ox, cy, cz + oz };
            const float PHI[5] = { -0.70f, -0.28f, 0.14f, 0.55f, 1.25f };
            float band[5][6][3];
            for (int b = 0; b < 5; b++) {
                float sy = sinf(PHI[b]), cr = cosf(PHI[b]);
                for (int sd = 0; sd < 6; sd++) {
                    float a = (float)sd * 6.28318531f / 6.0f;
                    band[b][sd][0] = mid[0] + cosf(a) * rx * cr;
                    band[b][sd][1] = mid[1] + sy * ry;
                    band[b][sd][2] = mid[2] + sinf(a) * rz * cr;
                }
            }
            for (int b = 0; b < 4; b++)
                for (int sd = 0; sd < 6; sd++) {
                    int s2 = (sd + 1) % 6;
                    float j = 0.93f + 0.13f * facet_hash(c * 64 + l * 8 + b);
                    emit_flat_tri(band[b][sd], band[b][s2], band[b+1][s2], col, j, phase);
                    emit_flat_tri(band[b][sd], band[b+1][s2], band[b+1][sd], col, j, phase);
                }
        }
    }
}

/* One cheap runnable check: a tree that floats, sinks, or goes NaN is the
   failure mode of the placement maths, and it is invisible in a screenshot. */
static void trees_selfcheck(void) {
    for (int i = 0; i < tree_count; i++) {
        assert(trees[i].y > 0.0f && trees[i].h > 0.0f && trees[i].r > 0.0f);
        assert(fabsf(trees[i].y - surface_height(trees[i].x, trees[i].z)) < 1e-3f);
    }
}

static void pick_camera(void);

/* A tree in the first frame's lens reads as a bug, so drop the ones the camera
   stands among. Swap-remove; order does not matter. */
static void clear_start_clearing(void) {
    const float r2 = 18.0f * 18.0f;
    float vx = peak_x - cam_x, vz = peak_z - cam_z;
    float len = sqrtf(vx * vx + vz * vz);
    if (len > 1.0f) { vx /= len; vz /= len; }
    float corridor = 0.45f * len;      /* keep midground forest, clear the view */
    for (int i = 0; i < tree_count; ) {
        float dx = trees[i].x - cam_x, dz = trees[i].z - cam_z;
        float along = dx * vx + dz * vz;
        float side = fabsf(dx * vz - dz * vx);   /* perpendicular distance */
        bool near_cam = dx * dx + dz * dz < r2;
        bool in_corridor = along > 0.0f && along < corridor && side < 16.0f;
        if (near_cam || in_corridor) trees[i] = trees[--tree_count];
        else i++;
    }
}

/* Flat water at y = 0. Before this the sea was just terrain painted teal, so
   the coast read as oddly coloured dirt. Opaque, no waves: the reference is a
   flat colour with a hard shoreline, and the fog does the distance work. */
static void emit_flat_up(const float *p0, const float *p1, const float *p2,
                         const float *c, float fx, float fz) {
    const float up[3] = { 0.0f, 1.0f, 0.0f };
    for (int k = 0; k < 3; k++) {
        const float *p = (k == 0) ? p0 : (k == 1) ? p1 : p2;
        push_vert(p, up, c, -2.0f, 0.0f, fx, fz, NULL, NULL);   /* -2: water */
    }
}

/* Birds: a two-triangle V each, flown entirely in the vertex shader -- the
   flock circles on the clock, which keeps the scene one draw call and keeps
   --shot reproducible. Placed once around the summit, at altitudes where a
   silhouette reads against the sky and not against the rock. */
static void emit_birds(unsigned seed) {
    static const float BIRD_COL[3] = { 0.17f, 0.16f, 0.15f };
    const float up[3] = { 0.0f, 1.0f, 0.0f };
    for (int i = 0; i < 11; i++) {
        unsigned h = terrain_hash(i, 77, seed ^ 0x51ed270bu);
        float ang = (float)(h & 1023u) / 1023.0f * 6.28318531f;
        float rad = TERRAIN_WORLD * (0.10f + 0.06f * (float)((h >> 10) & 255u) / 255.0f);
        float y   = peak_y * (0.55f + 0.32f * (float)((h >> 18) & 255u) / 255.0f);
        float sz  = 1.7f + 1.3f * (float)((h >> 26) & 63u) / 63.0f;
        float ph  = (float)((h >> 4) & 255u) / 255.0f;
        float bx = peak_x + cosf(ang) * rad, bz = peak_z + sinf(ang) * rad;
        float v[6][3] = {
            { bx,          y, bz + sz*0.55f },   /* nose  */
            { bx - sz,     y, bz - sz*0.45f },   /* tip   */
            { bx - sz*0.30f, y, bz - sz*0.10f }, /* inner */
            { bx,          y, bz + sz*0.55f },
            { bx + sz*0.30f, y, bz - sz*0.10f },
            { bx + sz,     y, bz - sz*0.45f },
        };
        for (int k = 0; k < 6; k++)
            push_vert(v[k], up, BIRD_COL, 3.0f, ph, v[k][0] - bx, 0.0f, NULL, NULL);
    }
}

static void emit_water(void) {
    /* well past the terrain: a plane that stops at the world edge shows the
       sky through the gap where the sea meets the horizon.
       One quad, not a grid. The grid existed to shade each panel separately,
       and the water shader now reflects the sky instead, so the subdivision
       bought nothing -- while its quad count grew with the world and could
       overflow the decor vertex buffer. */
    const float ext = SEA_EXTENT;
    const float y = 0.0f;
    float q[4][3] = { { -ext, y, -ext }, { ext, y, -ext },
                      {  ext, y,  ext }, { -ext, y,  ext } };
    float c[3] = { WATER_COL[0], WATER_COL[1], WATER_COL[2] };
    emit_flat_up(q[0], q[1], q[2], c, 0.0f, 0.0f);
    emit_flat_up(q[0], q[2], q[3], c, 0.0f, 0.0f);
}

/* ------------------------------------------------------------- planar mesh
   The terrain is not drawn on the grid it is sampled on. A variable-density
   point set follows the shape (dense on the massif, sparse on the meadow),
   Delaunay-triangulates it, and coplanar neighbours are merged into one facet
   colour. Irregular polygons with straight edges instead of a lattice, and the
   triangles land where the shape is instead of everywhere equally. */

/* --- per-chunk mesh -------------------------------------------------------
   Each chunk is triangulated on its own, from points generated inside its own
   rectangle. That is what makes build cost a function of the chunk instead of
   the world: the global Delaunay was O(n^2) over every point on the map, so a
   continental world was seconds of startup however the threading was arranged.

   Neighbours meet without a crack because the border ring is generated with no
   jitter and at full resolution whatever the LOD, so both sides of a shared
   border produce the identical chain of points there and their hull edges
   match exactly. Only the interior coarsens with distance, which is why there
   are no skirts here. */
#define MAX_PTS  2000
#define MAX_DT   5200
static float pts_x[MAX_PTS + 3], pts_z[MAX_PTS + 3];
/* ground shadow and bounced light per point, filled right after the point set
   is final */
static float hor_pt[MAX_PTS + 3][SUN_SECTORS];
static float bnc_pt[MAX_PTS + 3][SUN_SECTORS * 3];
static int npts = 0;

typedef struct { int v[3]; float cx, cz, r2; int alive; } dtri_t;
static dtri_t dt[MAX_DT];
static int ndt = 0;

static void dt_add(int a, int b, int c) {
    if (ndt >= MAX_DT) return;
    dtri_t *t = &dt[ndt++];
    t->v[0] = a; t->v[1] = b; t->v[2] = c; t->alive = 1; t->r2 = -1.0f;
    float ax = pts_x[a], az = pts_z[a];
    float bx = pts_x[b], bz = pts_z[b];
    float cx = pts_x[c], cz = pts_z[c];
    float d = 2.0f * (ax*(bz-cz) + bx*(cz-az) + cx*(az-bz));
    if (fabsf(d) < 1e-6f) { t->alive = 0; return; }     /* degenerate */
    float a2 = ax*ax+az*az, b2 = bx*bx+bz*bz, c2 = cx*cx+cz*cz;
    t->cx = (a2*(bz-cz) + b2*(cz-az) + c2*(az-bz)) / d;
    t->cz = (a2*(cx-bx) + b2*(ax-cx) + c2*(bx-ax)) / d;
    float dx = t->cx - ax, dz = t->cz - az;
    t->r2 = dx*dx + dz*dz;
}

/* Bowyer-Watson, O(n^2) in the triangle scan. Per chunk that is ~200 points,
   i.e. a fraction of a millisecond, and it stays that size whatever the world
   is -- which is the entire point of triangulating per chunk. */
static void delaunay(void) {
    int n = npts;
    pts_x[n] = -30000.0f; pts_z[n] = -30000.0f;
    pts_x[n+1] = 30000.0f; pts_z[n+1] = -30000.0f;
    pts_x[n+2] = 0.0f;     pts_z[n+2] = 30000.0f;
    ndt = 0;
    dt_add(n, n+1, n+2);
    static int ea[MAX_DT * 3], eb[MAX_DT * 3];
    for (int i = 0; i < n; i++) {
        int ne = 0;
        for (int k = 0; k < ndt; k++) {
            if (!dt[k].alive) continue;
            float dx = pts_x[i] - dt[k].cx, dz = pts_z[i] - dt[k].cz;
            if (dx*dx + dz*dz >= dt[k].r2 - 1e-3f) continue;
            dt[k].alive = 0;
            for (int e = 0; e < 3; e++) {
                int a = dt[k].v[e], b = dt[k].v[(e+1) % 3];
                int found = -1;                       /* shared edge: cancel */
                for (int q = 0; q < ne; q++)
                    if (ea[q] == b && eb[q] == a) { found = q; break; }
                if (found >= 0) {
                    ea[found] = ea[--ne]; eb[found] = eb[ne];
                } else {
                    ea[ne] = a; eb[ne] = b; ne++;
                }
            }
        }
        for (int q = 0; q < ne; q++) dt_add(ea[q], eb[q], i);
    }
    for (int k = 0; k < ndt; k++)
        if (dt[k].alive && (dt[k].v[0] >= n || dt[k].v[1] >= n || dt[k].v[2] >= n))
            dt[k].alive = 0;
}

/* A boundary point remembers which side(s) of the chunk rect it sits on, so
   the emitter can find the boundary edges that need a skirt. */
static unsigned char pt_edge[MAX_PTS + 3];

/* --- point sampling ------------------------------------------------------ */
/* Every point must land inside the chunk being built. A point outside the rect
   (a river bank one cell over, say) extends the convex hull beyond the chunk
   and Delaunay fans it back to the nearest border run -- a handful of slivers
   200 units long, which is exactly what that looked like on screen. */
static float clip_x0 = 0.0f, clip_x1 = 0.0f, clip_z0 = 0.0f, clip_z1 = 0.0f;
static void add_point(float x, float z, float min_d) {
    if (npts >= MAX_PTS) return;
    if (x < clip_x0 - 0.5f || x > clip_x1 + 0.5f) return;
    if (z < clip_z0 - 0.5f || z > clip_z1 + 0.5f) return;
    /* the jitter can push a border point off the heightfield: sampling there
       returns the out-of-grid sentinel and the triangle would spike to -1e9 */
    if (sample_height(x, z) < -1e8f) return;
    for (int i = 0; i < npts; i++) {
        float dx = pts_x[i] - x, dz = pts_z[i] - z;
        if (dx*dx + dz*dz < min_d*min_d) return;
    }
    unsigned char e = 0;
    if (x <= clip_x0 + 0.05f) e |= 1u;
    if (x >= clip_x1 - 0.05f) e |= 2u;
    if (z <= clip_z0 + 0.05f) e |= 4u;
    if (z >= clip_z1 - 0.05f) e |= 8u;
    pts_x[npts] = x; pts_z[npts] = z; pt_edge[npts] = e; npts++;
}

/* Height, colour and lighting are all sampled at the facet centroid: one value
   per polygon is what makes neighbouring coplanar triangles read as a single
   plane instead of a pair. */
static int mesh_pts = 0, mesh_tris = 0, mesh_facets = 0;

static float tri_col[MAX_DT][3];
static float tri_flow[MAX_DT][2];
static float nx_[MAX_DT], ny_[MAX_DT], nz_[MAX_DT];

static inline int chunk_of(float x, float z) {
    int cx = (int)((x + TERRAIN_WORLD * 0.5f) / CHUNK_WORLD);
    int cz = (int)((z + TERRAIN_WORLD * 0.5f) / CHUNK_WORLD);
    if (cx < 0) cx = 0; if (cx >= CHUNK_GRID) cx = CHUNK_GRID - 1;
    if (cz < 0) cz = 0; if (cz >= CHUNK_GRID) cz = CHUNK_GRID - 1;
    return cz * CHUNK_GRID + cx;
}

/* One shade step per facet plane. This used to be a hash of the coplanar
   cluster id, which cannot work per chunk: two coplanar triangles either side
   of a chunk border would get different shades and the ground would be ruled
   with a 181-unit grid of colour breaks. Keying on the plane plus a coarse
   position cell makes the shade a function of world position only, so the
   border disappears and the block size stays the same as the old cluster cap gave. */
static float facet_shade(float nx, float ny, float nz, float off, float x, float z) {
    int qx = (int)floorf(nx * 10.0f), qy = (int)floorf(ny * 10.0f), qz = (int)floorf(nz * 10.0f);
    int qo = (int)floorf(off * 0.5f);
    int gx = (int)floorf(x / 47.0f), gz = (int)floorf(z / 47.0f);
    unsigned h = (unsigned)qx * 73856093u ^ (unsigned)qy * 19349663u
               ^ (unsigned)qz * 83492791u ^ (unsigned)qo * 2654435761u
               ^ (unsigned)gx * 15485863u ^ (unsigned)gz * 32452843u;
    return (float)(h & 255u) / 255.0f;
}

/* Build one chunk from scratch and return its vertex word count. Runs on the
   builder thread; chunk_verts is this thread's scratch. */
static int chunk_build(int ci, int step, unsigned seed, const float *cr,
                       const float *cg, const float *cb) {
    const int cx = chunks[ci].cx, cz = chunks[ci].cz;
    const int c0 = cx * CHUNK_CELLS, r0 = cz * CHUNK_CELLS;
    int c1 = c0 + CHUNK_CELLS, r1 = r0 + CHUNK_CELLS;
    if (c1 > N - 1) c1 = N - 1;          /* the last column/row is short */
    if (r1 > N - 1) r1 = N - 1;
    if (c1 <= c0 || r1 <= r0) return 0;  /* chunk entirely past the world edge */
    const float ox = c0 * CELL - TERRAIN_WORLD * 0.5f;
    const float oz = r0 * CELL - TERRAIN_WORLD * 0.5f;
    const float ex = c1 * CELL - TERRAIN_WORLD * 0.5f;
    const float ez = r1 * CELL - TERRAIN_WORLD * 0.5f;
    tree_detail = step > 2;

    clip_x0 = ox; clip_x1 = ex; clip_z0 = oz; clip_z1 = ez;
    npts = 0;
    /* Border ring first, unjittered and at full resolution: the neighbour
       generates exactly these points along the same line, so the two chunk
       surfaces share a vertex chain and cannot crack or overlap. */
    for (int k = c0; k <= c1; k += step) {
        float f = k * CELL - TERRAIN_WORLD * 0.5f;
        add_point(f, oz, 0.01f);
        add_point(f, ez, 0.01f);
    }
    for (int k = r0; k <= r1; k += step) {
        float f = k * CELL - TERRAIN_WORLD * 0.5f;
        add_point(ox, f, 0.01f);
        add_point(ex, f, 0.01f);
    }
    /* River first among the interior, for the same reason as before: vertices
       on both banks make the water edge a crease in the mesh instead of a
       facet that merely straddles the channel. */
    for (int i = 0; i < seg_n; i++) {
        float dx = seg_x1[i] - seg_x0[i], dz = seg_z1[i] - seg_z0[i];
        float l = sqrtf(dx*dx + dz*dz);
        if (l < 1e-4f) continue;
        float px = -dz / l, pz = dx / l;
        float hw = seg_hw[i];
        add_point(seg_x0[i] + px * hw, seg_z0[i] + pz * hw, 9.0f);
        add_point(seg_x0[i] - px * hw, seg_z0[i] - pz * hw, 9.0f);
        add_point(seg_x1[i] + px * hw, seg_z1[i] + pz * hw, 9.0f);
        add_point(seg_x1[i] - px * hw, seg_z1[i] - pz * hw, 9.0f);
    }
    for (int zi = r0 + step; zi < r1; zi += step) {
        for (int xi = c0 + step; xi < c1; xi += step) {
            int xm = xi > 0 ? xi-1 : xi, xp = xi < N-1 ? xi+1 : xi;
            int zm = zi > 0 ? zi-1 : zi, zp = zi < N-1 ? zi+1 : zi;
            float dhdx = (heights[zi*N+xp] - heights[zi*N+xm]) / ((xp-xm) * CELL);
            float dhdz = (heights[zp*N+xi] - heights[zm*N+xi]) / ((zp-zm) * CELL);
            float slope = sqrtf(dhdx*dhdx + dhdz*dhdz);
            float h = heights[zi*N+xi];
            /* spacing from steepness: the massif is where the shape lives */
            float t = slope / 0.55f;
            if (t > 1.0f) t = 1.0f;
            float d = 36.0f - 25.0f * t;
            if (fabsf(h) < 12.0f) d = fminf(d, 15.0f);   /* keep the shoreline readable */
            /* LOD wins over the slope rule, or the interior would not cost less */
            float floor_d = (float)step * CELL * 0.95f;
            if (d < floor_d) d = floor_d;
            /* jitter the candidate so the set does not inherit the lattice */
            unsigned hs = terrain_hash(xi, zi, seed ^ 0x68e31da4u);
            float jx = ((float)( hs        & 255u) / 255.0f - 0.5f) * CELL * 0.9f * step;
            float jz = ((float)((hs >> 8)  & 255u) / 255.0f - 0.5f) * CELL * 0.9f * step;
            float x = xi * CELL - TERRAIN_WORLD * 0.5f + jx;
            float z = zi * CELL - TERRAIN_WORLD * 0.5f + jz;
            if (x < ox + 0.5f || x > ex - 0.5f || z < oz + 0.5f || z > ez - 0.5f) continue;
            add_point(x, z, d);
        }
    }
    /* One horizon per point, not per grid cell: the point set is what the
       surface is drawn from, so this is both more accurate (no bilinear
       smear across a 15-unit cell) and less work (points < cells). */
    for (int i = 0; i < npts; i++) {
        float py = sample_height(pts_x[i], pts_z[i]);
        horizon_at(pts_x[i], pts_z[i], py, 1, hor_pt[i]);
        bounce_at(pts_x[i], pts_z[i], py, cr, cg, cb, bnc_pt[i]);
    }
    delaunay();

    int ntri = 0;
    for (int k = 0; k < ndt; k++) {
        if (!dt[k].alive) continue;
        float ax = pts_x[dt[k].v[0]], az = pts_z[dt[k].v[0]];
        float bx = pts_x[dt[k].v[1]], bz = pts_z[dt[k].v[1]];
        float qx = pts_x[dt[k].v[2]], qz = pts_z[dt[k].v[2]];
        float u[3] = { bx-ax, sample_height(bx,bz)-sample_height(ax,az), bz-az };
        float w[3] = { qx-ax, sample_height(qx,qz)-sample_height(ax,az), qz-az };
        nx_[k] = u[1]*w[2]-u[2]*w[1];
        ny_[k] = u[2]*w[0]-u[0]*w[2];
        nz_[k] = u[0]*w[1]-u[1]*w[0];
        float nl = sqrtf(nx_[k]*nx_[k]+ny_[k]*ny_[k]+nz_[k]*nz_[k]);
        if (nl < 1e-9f) { nx_[k]=0; ny_[k]=1; nz_[k]=0; }
        else { nx_[k]/=nl; ny_[k]/=nl; nz_[k]/=nl; }
        if (ny_[k] < 0.0f) { nx_[k]=-nx_[k]; ny_[k]=-ny_[k]; nz_[k]=-nz_[k]; }
        ntri++;
    }
    if (ntri < 8) return 0;

    /* --- one colour per facet ------------------------------------------- */
    float top = -1e9f, bot = 1e9f;
    for (int k = 0; k < ndt; k++) {
        if (!dt[k].alive) continue;
        float cxw = (pts_x[dt[k].v[0]] + pts_x[dt[k].v[1]] + pts_x[dt[k].v[2]]) / 3.0f;
        float czw = (pts_z[dt[k].v[0]] + pts_z[dt[k].v[1]] + pts_z[dt[k].v[2]]) / 3.0f;
        float cyw = (sample_height(pts_x[dt[k].v[0]], pts_z[dt[k].v[0]])
                   + sample_height(pts_x[dt[k].v[1]], pts_z[dt[k].v[1]])
                   + sample_height(pts_x[dt[k].v[2]], pts_z[dt[k].v[2]])) / 3.0f;
        if (cyw > top) top = cyw;
        if (cyw < bot) bot = cyw;

        float r, g, b;
        {
            float gx = (cxw + TERRAIN_WORLD * 0.5f) / CELL;
            float gz = (czw + TERRAIN_WORLD * 0.5f) / CELL;
            int x0 = (int)floorf(gx), z0 = (int)floorf(gz);
            if (x0 < 0) x0 = 0; if (x0 > N-2) x0 = N-2;
            if (z0 < 0) z0 = 0; if (z0 > N-2) z0 = N-2;
            float fx = gx - x0, fz = gz - z0;
            if (fx < 0.0f) fx = 0.0f; if (fx > 1.0f) fx = 1.0f;
            if (fz < 0.0f) fz = 0.0f; if (fz > 1.0f) fz = 1.0f;
            int i00 = z0*N+x0, i10 = i00+1, i01 = i00+N, i11 = i01+1;
            #define SAMP(arr) ((arr[i00]*(1-fx)+arr[i10]*fx)*(1-fz) + (arr[i01]*(1-fx)+arr[i11]*fx)*fz)
            r = SAMP(cr); g = SAMP(cg); b = SAMP(cb);
            #undef SAMP
        }

        float steep = 1.0f - ny_[k];
        if (steep > 1.0f) steep = 1.0f;
        float fw = 0.30f + steep * 1.30f;
        if (fw > 1.0f) fw = 1.0f;
        float off = nx_[k]*pts_x[dt[k].v[0]] + ny_[k]*sample_height(pts_x[dt[k].v[0]], pts_z[dt[k].v[0]])
                  + nz_[k]*pts_z[dt[k].v[0]];
        float j = 1.0f + (FACET_SHADES[(int)(facet_shade(nx_[k],ny_[k],nz_[k],off,cxw,czw) * 4.0f) & 3]
                          - 1.0f) * fw;
        r *= j; g *= j; b *= j;

        float fx2 = 0.0f, fz2 = 0.0f, rx = 0.0f, rz = 0.0f;
        float rhw = RIVER_HW_MIN;
        /* the lake wins where the two overlap: a river mouth is still water,
           and painting it as a river put its moving ripple band on top of the
           lake as a bright dashed stripe */
        int is_lake = lake_at(cxw, czw);
        if (!is_lake && river_query(cxw, czw, NULL, &rx, &rz, &rhw) < rhw) {
            fx2 = rx; fz2 = rz;
            float wj = 1.0f + (j - 1.0f) * 0.5f;
            r = WATER_COL[0]*wj; g = WATER_COL[1]*wj; b = WATER_COL[2]*wj;
        } else if (is_lake) {
            /* length 2 marks a lake: still water to the shader (which keys on
               a non-zero flow), but a fixed unit direction on a big lake draws
               one long straight ripple band across the whole surface */
            fx2 = 1.6f; fz2 = 1.2f;
            float wj = 1.0f + (j - 1.0f) * 0.5f;
            r = WATER_COL[0]*wj; g = WATER_COL[1]*wj; b = WATER_COL[2]*wj;
        }

        tri_col[k][0] = r; tri_col[k][1] = g; tri_col[k][2] = b;
        tri_flow[k][0] = fx2; tri_flow[k][1] = fz2;
    }
    chunks[ci].cy_top = (int)top;

    /* --- emit ------------------------------------------------------------
       Trees are placed globally (the camera and the diagnostics walk that one
       list) but snapped and emitted here, against this chunk's own triangles:
       the drawn surface is the chord between vertices and sags inside a big
       facet, so a tree placed from the heightfield alone floats. */
    vtarget = chunk_verts;
    vcap = MAX_CHUNK_VERTS * VERT_FLOATS;
    vcount = 0;
    for (int k = 0; k < ndt; k++) {
        if (!dt[k].alive) continue;
        if (vcount + 3 * VERT_FLOATS > MAX_CHUNK_VERTS * VERT_FLOATS) break;
        const float *shp = hor_pt[dt[k].v[0]];
        const float *bcp = bnc_pt[dt[k].v[0]];
        float nrm[3] = { nx_[k], ny_[k], nz_[k] };
        for (int e = 0; e < 3; e++) {
            int id = dt[k].v[e];
            float p[3] = { pts_x[id], sample_height(pts_x[id], pts_z[id]), pts_z[id] };
            push_vert(p, nrm, tri_col[k], 0.0f, 0.0f,
                      tri_flow[k][0], tri_flow[k][1], shp, bcp);
        }
    }
    /* Skirts. The border ring follows the tier, so a chunk and its neighbour
       only share a vertex chain when their tiers match. Where they differ, the
       two surfaces meet at slightly different heights and the gap shows as a
       sliver of sky. Drop a wall from every boundary edge: 40 units is far
       more than the chord error of the coarsest facet. Only tier 2+ chunks
       pay for it, because a fine chunk borders either fine chunks (exact
       match) or a coarse one, and the coarse side's wall closes the gap. */
    if (step > 1) {
        const float SK = 40.0f;
        for (int k = 0; k < ndt; k++) {
            if (!dt[k].alive) continue;
            if (vcount + 12 * VERT_FLOATS > MAX_CHUNK_VERTS * VERT_FLOATS) break;
            const float *shp = hor_pt[dt[k].v[0]];
            const float *bcp = bnc_pt[dt[k].v[0]];
            float nrm[3] = { nx_[k], ny_[k], nz_[k] };
            for (int e = 0; e < 3; e++) {
                int a = dt[k].v[e], b = dt[k].v[(e + 1) % 3];
                if (!(pt_edge[a] & pt_edge[b])) continue;
                float pa[3] = { pts_x[a], sample_height(pts_x[a], pts_z[a]), pts_z[a] };
                float pb[3] = { pts_x[b], sample_height(pts_x[b], pts_z[b]), pts_z[b] };
                float da[3] = { pa[0], pa[1] - SK, pa[2] };
                float db[3] = { pb[0], pb[1] - SK, pb[2] };
                push_vert(pa, nrm, tri_col[k], 0,0, 0,0, shp, bcp);
                push_vert(pb, nrm, tri_col[k], 0,0, 0,0, shp, bcp);
                push_vert(db, nrm, tri_col[k], 0,0, 0,0, shp, bcp);
                /* both windings: the wall is visible from either side of it */
                push_vert(pa, nrm, tri_col[k], 0,0, 0,0, shp, bcp);
                push_vert(db, nrm, tri_col[k], 0,0, 0,0, shp, bcp);
                push_vert(da, nrm, tri_col[k], 0,0, 0,0, shp, bcp);
            }
        }
    }

    int ntrees = 0;
    for (int t = 0; t < tree_count; t++) {
        if (chunk_of(trees[t].x, trees[t].z) != ci) continue;
        if (vcount + TREE_VERTS * VERT_FLOATS > MAX_CHUNK_VERTS * VERT_FLOATS) break;
        float bx = trees[t].x, bz = trees[t].z;
        for (int k = 0; k < ndt; k++) {
            if (!dt[k].alive) continue;
            int id[3] = { dt[k].v[0], dt[k].v[1], dt[k].v[2] };
            float ax = pts_x[id[0]], az = pts_z[id[0]];
            float v0x = pts_x[id[1]] - ax, v0z = pts_z[id[1]] - az;
            float v1x = pts_x[id[2]] - ax, v1z = pts_z[id[2]] - az;
            float v2x = bx - ax, v2z = bz - az;
            float den = v0x*v1z - v1x*v0z;
            if (fabsf(den) < 1e-9f) continue;
            float u = (v2x*v1z - v1x*v2z) / den;
            float v = (v0x*v2z - v2x*v0z) / den;
            if (u < -0.02f || v < -0.02f || u + v > 1.02f) continue;
            trees[t].y = sample_height(ax, az) * (1.0f - u - v)
                       + sample_height(pts_x[id[1]], pts_z[id[1]]) * u
                       + sample_height(pts_x[id[2]], pts_z[id[2]]) * v;
            /* the tree takes the shade and the bounced light of the ground it
               stands in */
            sway_shadow = hor_pt[id[0]];
            sway_bounce = bnc_pt[id[0]];
            break;
        }
        emit_tree(&trees[t]);
        ntrees++;
    }
    (void)ntrees; (void)bot;

    mesh_pts += npts;
    mesh_tris += ntri;
    /* facets are no longer merged, so a facet is a triangle; the diagnostic
       keeps reporting the triangle count it used to call facets */
    mesh_facets += ntri;
    return vcount;
}

/* --- streaming -------------------------------------------------------------
   One worker thread, a job queue, and a chunk lifecycle the main thread owns.
   A chunk is built at a tier (interior spacing in cells) chosen from its
   distance to the camera, evicted when it is past the fog, and rebuilt when
   its tier changes -- which is what a frozen build could not do: fly into a
   chunk that was far away at startup and it stayed coarse.

   Chunk indices never change, so a rebuild is "destroy the buffer, queue the
   job at the new tier". Everything the worker reads is global and immutable
   after startup (heightfield, bakes, tree list), so build order cannot affect
   the result and --shot stays deterministic. */
static pthread_t builder;
static pthread_mutex_t job_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_cv = PTHREAD_COND_INITIALIZER;
#define JOB_MAX 4096
static int job_q[JOB_MAX];
static int job_head = 0, job_tail = 0;
static int jobs_in_flight = 0;
static int stream_rebuilds = 0;   /* diagnostics */
static int builder_live = 0;
static int builder_quit = 0;
static int builder_started = 0;
static uint64_t builder_t0 = 0;
static double build_async_ms = 0.0;

/* tier -> world units between interior points. The band edges are what
   chunk_tier() and the hysteresis in chunks_update() agree on.
   Relative to the world like the fog is, because the point of a tier is to
   keep the facet the same size ON SCREEN: a 500-unit band on a 2200-unit
   island covers a quarter of it, on a 4400-unit one only an eighth, so the
   whole far half of a bigger world would be rendered at step 4 and read flat
   the moment the world grew. 500/2200 and 1000/2200 are the ratios. */
#define TIER_1_MAX (0.2273f * TERRAIN_WORLD)
#define TIER_2_MAX (0.4545f * TERRAIN_WORLD)
static int chunk_tier(float d) {
    if (getenv("NOLOD")) return 1;   /* A/B knob: dense everywhere */
    if (d < TIER_1_MAX) return 1;
    if (d < TIER_2_MAX) return 2;
    return 4;
}

static int jobs_pending(void) {
    pthread_mutex_lock(&job_mu);
    int n = job_tail - job_head;
    if (jobs_in_flight > 0) n += jobs_in_flight;
    pthread_mutex_unlock(&job_mu);
    return n;
}

static void job_push(int ci, int tier) {
    pthread_mutex_lock(&job_mu);
    if (job_tail - job_head < JOB_MAX) {
        job_q[job_tail % JOB_MAX] = ci | (tier << 12);
        job_tail++;
        pthread_cond_signal(&job_cv);
    }
    pthread_mutex_unlock(&job_mu);
}

/* --- environment bakes -----------------------------------------------------
   Ambient occlusion, the horizon sectors, the surface colours and the bounce
   are pure functions of the heightfield, the tree list and the seed, and their
   only consumer is the chunk builder. They ran on the main thread, which is
   why startup cost 56 ms at a 2200-unit world: an O(n^2) shadow march over 22k
   grid cells plus two full colour passes. On the worker they overlap with the
   chunk build, and the first frame arrives while they are still running. */
static float cr[N * N], cg[N * N], cb[N * N];

static void bake_ambient(void) {
    for (int zi = 0; zi < N; zi++) {
        for (int xi = 0; xi < N; xi++) {
            float x = (xi / (float)(N - 1)) * TERRAIN_WORLD - TERRAIN_WORLD * 0.5f;
            float z = (zi / (float)(N - 1)) * TERRAIN_WORLD - TERRAIN_WORLD * 0.5f;
            sky_ao[zi * N + xi] = sky_occlusion(x, heights[zi * N + xi], z);
        }
    }
}

/* Colours per grid point (terrain-type only, lighting is in the shader). */
static void bake_colours(unsigned seed) {
    for (int zi = 0; zi < N; zi++) {
        for (int xi = 0; xi < N; xi++) {
            int i = zi * N + xi;
            float h = heights[i];
            float x = (xi / (float)(N - 1)) * TERRAIN_WORLD - TERRAIN_WORLD * 0.5f;
            float z = (zi / (float)(N - 1)) * TERRAIN_WORLD - TERRAIN_WORLD * 0.5f;
            int xm = xi > 0 ? xi - 1 : xi, xp = xi < N - 1 ? xi + 1 : xi;
            int zm = zi > 0 ? zi - 1 : zi, zp = zi < N - 1 ? zi + 1 : zi;
            /* normals from the *eroded, un-terraced* surface are too smooth for
               slope classification; use the triangle normals later instead */
            float dhdx = (heights[zi * N + xp] - heights[zi * N + xm]) / ((xp - xm) * CELL);
            float dhdz = (heights[zp * N + xi] - heights[zm * N + xi]) / ((zp - zm) * CELL);
            float ny = 1.0f / sqrtf(1.0f + dhdx * dhdx + dhdz * dhdz);
            float slope = 1.0f - ny;

            /* bands are relative to the summit: a fixed altitude puts the
               treeline in the same place whatever the seed generates, and the
               reference mountain is bare rock above roughly its lower third */
            /* Bare rock is per mountain, not per altitude: an island with a
               range has summits at every height, and a single rock line drawn
               from the tallest one paints the whole range orange. The mask
               says "this is the upper body of a peak", which is the shape the
               reference has -- green flanks, bare crown, snow on top. */
            float peakness = terrain_mountain_mask(x, z, seed);
            float snow_line = 0.72f * peak_y;
            float r, g, b;
            if (h < 0.0f)                       { r = 0.10f; g = 0.20f; b = 0.20f; }  /* seabed */
            else if (h < 3.0f)                  { r = 0.63f; g = 0.57f; b = 0.41f; }  /* sand */
            else if (h > snow_line)             { r = 0.90f; g = 0.86f; b = 0.79f; }  /* snow */
            else if (slope > 0.30f || peakness > 0.74f)
                                                { r = 0.49f; g = 0.29f; b = 0.11f; }  /* rock */
            else if (terrain_value_noise(x * 0.075f, z * 0.075f, seed + 9u) > 0.55f) {
                r = 0.27f; g = 0.18f; b = 0.08f;                       /* bare dirt */
            }
            else {
                /* patchy grass: the reference meadow shifts between yellow and
                   blue green in patches several facets wide, and a single flat
                   green is the giveaway that it is not hand-placed */
                float patch = 0.5f + 0.5f * terrain_value_noise(x * 0.022f, z * 0.022f, seed + 5u);
                /* scaled so the brightest facet (shade step x lighting ~1.95)
                   lands on the reference's sunlit grass #a8c94e, not past it:
                   a clipping green channel is what made the lawn neon */
                r = 0.29f + 0.06f * patch;
                g = 0.38f + 0.05f * patch;
                b = 0.13f + 0.04f * (1.0f - patch);
            }

            if (trees_only) { r = 0.475f; g = 0.435f; b = 0.396f; }   /* ref ground */

            /* no directional shadow here any more: it is evaluated per pixel
               from the baked horizon, so it follows the sun round the day.
               AO stays baked -- ambient occlusion does not care where the sun
               is, which is exactly why it can be. */
            float ao = 1.0f - 0.26f * sky_ao[i];
            cr[i] = r * ao; cg[i] = g * ao; cb[i] = b * ao;
        }
    }


}

/* Runs on the builder thread, in this order: each bake feeds the next and the
   chunks need all of them. */
static void bake_environment(unsigned seed) {
    bake_ambient();
    bake_colours(seed);
    gcr = cr; gcg = cg; gcb = cb;
}

static void *builder_main(void *arg) {
    unsigned seed = (unsigned)(uintptr_t)arg;
    bake_environment(seed);
    for (;;) {
        pthread_mutex_lock(&job_mu);
        while (job_head == job_tail && !builder_quit)
            pthread_cond_wait(&job_cv, &job_mu);
        if (builder_quit && job_head == job_tail) {
            pthread_mutex_unlock(&job_mu);
            break;
        }
        int job = job_q[job_head % JOB_MAX];
        job_head++;
        jobs_in_flight++;
        pthread_mutex_unlock(&job_mu);

        int ci = job & 0xFFF, tier = job >> 12;
        int words = chunk_build(ci, tier, seed, gcr, gcg, gcb);
        float *copy = words ? (float *)malloc((size_t)words * sizeof(float)) : NULL;
        if (copy) memcpy(copy, chunk_verts, (size_t)words * sizeof(float));

        pthread_mutex_lock(&job_mu);
        chunks[ci].data = copy;
        chunks[ci].words = copy ? words : 0;
        chunks[ci].state = CHUNK_DONE;
        jobs_in_flight--;
        pthread_mutex_unlock(&job_mu);
    }
    pthread_mutex_lock(&job_mu);
    builder_live = 0;
    build_async_ms = stm_sec(stm_now() - builder_t0) * 1000.0;
    pthread_mutex_unlock(&job_mu);
    return NULL;
}

static void start_streamer(unsigned seed) {
    for (int i = 0; i < NCHUNKS; i++) {
        chunks[i].state = CHUNK_EMPTY;
        chunks[i].tier = 0;
        chunks[i].buf.id = SG_INVALID_ID;
        chunks[i].data = NULL;
        chunks[i].cx = i % CHUNK_GRID;
        chunks[i].cz = i / CHUNK_GRID;
    }
    job_head = job_tail = 0;
    jobs_in_flight = 0;
    builder_quit = 0;
    mesh_pts = mesh_tris = mesh_facets = 0;
    builder_t0 = stm_now();
    builder_live = 1;
    builder_started = 1;
    pthread_create(&builder, NULL, builder_main, (void *)(uintptr_t)seed);
}

static void stop_streamer(void) {
    if (!builder_started) return;
    pthread_mutex_lock(&job_mu);
    builder_quit = 1;
    pthread_cond_signal(&job_cv);
    pthread_mutex_unlock(&job_mu);
    pthread_join(builder, NULL);
    builder_started = 0;
}

static int chunks_drawn = 0;

static int nchunks_ready(void) {
    int n = 0;
    for (int i = 0; i < NCHUNKS; i++) if (chunks[i].state == CHUNK_READY) n++;
    return n;
}

/* Everything queued has to be at its tier before a capture, or --shot could
   photograph a half-streamed world. */
static int world_ready(void) {
    if (jobs_pending()) return 0;
    for (int i = 0; i < NCHUNKS; i++)
        if (chunks[i].state == CHUNK_BUILDING || chunks[i].state == CHUNK_DONE) return 0;
    return 1;
}

/* Decide what each chunk should be, once a frame, from the camera. Runs on the
   main thread because releasing a buffer is a sokol call. */
static void chunks_update(void) {
    const float keep = FOG_FAR + CHUNK_WORLD * 0.75f;   /* evict beyond the haze */
    for (int i = 0; i < NCHUNKS; i++) {
        float ccx = ((float)chunks[i].cx + 0.5f) * CHUNK_WORLD - TERRAIN_WORLD * 0.5f;
        float ccz = ((float)chunks[i].cz + 0.5f) * CHUNK_WORLD - TERRAIN_WORLD * 0.5f;
        float d = sqrtf((ccx - cam_x) * (ccx - cam_x) + (ccz - cam_z) * (ccz - cam_z));
        int want = (d < keep) ? chunk_tier(d) : 0;
        if (chunks[i].state == CHUNK_BUILDING || chunks[i].state == CHUNK_DONE) continue;
        if (chunks[i].state == CHUNK_READY && chunks[i].tier == want) continue;
        if (chunks[i].state == CHUNK_EMPTY && want == 0) continue;
        /* Hysteresis: a camera parked on a band edge must not rebuild the same
           chunk every frame. Inside the dead band the current tier stays. */
        if (chunks[i].state == CHUNK_READY && want != 0 && want != chunks[i].tier) {
            float edge = (d < 0.5f * (TIER_1_MAX + TIER_2_MAX)) ? TIER_1_MAX : TIER_2_MAX;
            if (fabsf(d - edge) < 60.0f) continue;
        }
        if (chunks[i].buf.id != SG_INVALID_ID) {
            sg_destroy_buffer(chunks[i].buf);
            chunks[i].buf.id = SG_INVALID_ID;
        }
        chunks[i].words = 0;
        if (want == 0) { chunks[i].state = CHUNK_EMPTY; chunks[i].tier = 0; continue; }
        stream_rebuilds++;
        chunks[i].tier = want;
        chunks[i].state = CHUNK_BUILDING;
        job_push(i, want);
    }
}

/* sokol calls this for every validation failure; level 0 is a panic, after
   which it aborts. Printing the message is the entire point. */
static void game_logger(const char *tag, uint32_t level, uint32_t item,
                      const char *msg, uint32_t line, const char *file, void *ud) {
    (void)ud;
    /* The palette is one uniform block shared by the terrain and sky shaders,
       and each uses only part of it: the GL compiler removes the members a
       stage never reads, so sokol cannot find them when it walks the list.
       Benign by construction, and it fires seven times per launch. */
    if (item == SG_LOGITEM_GL_UNIFORMBLOCK_NAME_NOT_FOUND_IN_SHADER) return;
    fprintf(stderr, "%s%s%s(%u): %s\n", tag, level ? " warning: " : " PANIC: ",
            file ? file : "sokol", line, msg ? msg : "(no message)");
}

static void build_terrain(unsigned seed) {
    for (int zi = 0; zi < N; zi++) {
        for (int xi = 0; xi < N; xi++) {
            float x = (xi / (float)(N - 1)) * TERRAIN_WORLD - TERRAIN_WORLD * 0.5f;
            float z = (zi / (float)(N - 1)) * TERRAIN_WORLD - TERRAIN_WORLD * 0.5f;
            heights[zi * N + xi] = terrain_height(x, z, seed);
            if (heights[zi * N + xi] > peak_y) {
                peak_y = heights[zi * N + xi];
                peak_x = x;
                peak_z = z;
            }
        }
    }
    /* talus follows the cell: same slope limit as the old 1.35-unit cell, so
       the terrace risers survive erosion the same way they did */
    terrain_erode(heights, scratch, N, CELL, 2, 2.22f * CELL);

    /* Lakes first: they flatten their basins, which changes where the river
       can run. Then carve the channel, before the shadow/AO bake so the water
       gets its own shading. */
    (void)seed;
    build_hydrology();
    build_seg_index();
    carve_river();

    /* trees before anything that marches the horizon: a crown is an occluder */
    place_trees(seed);
    build_tree_index();
    /* the start vantage has to see past the trees it stands in */
    if (!have_view) { pick_camera(); clear_start_clearing(); }
    trees_selfcheck();

    /* Weather that does not belong to a chunk: the sea plane, clouds, birds.
       Emitted here because the clouds are placed in the camera's view cone and
       this is everything the first frame needs; the rest waits on the worker. */
    vtarget = verts;
    vcap = DECOR_VERTS * VERT_FLOATS;
    vcount = 0;
    emit_water();
    emit_cloud(seed);
    emit_birds(seed);
    decor_words = vcount;

    start_streamer(seed);
    if (headless) {
        int lit = 0, rn = 0;
        double relief = 0.0;
        lit = N * N - (shadowed_pct() * N * N) / 100;
        for (int zi = 0; zi < N; zi++)
            for (int xi = 0; xi + 1 < N; xi++) {
                relief += fabs(heights[zi*N+xi+1] - heights[zi*N+xi]);
                rn++;
            }
        /* no shadow figure here: the horizon bake now runs on the worker, so
           at this point nothing is shaded yet. The capture sidecar reports it
           once the world has settled. */
        (void)lit;
        fprintf(stderr, "terrain: trees=%d  mean |dh| per cell=%.2f (cell %.2f) peak=%.0f,%.0f,%.0f\n",
                tree_count, relief / rn, CELL, peak_x, peak_y, peak_z);
    }
}

/* Framing wants a low vantage across land with the peak high in frame. A fixed
   camera gives cliffs or open sea on half the seeds, so search azimuths x radii
   and score what the camera actually sees: is the summit unoccluded, does it
   subtend roughly the share of frame height the reference peak does, is the
   foreground flat enough to read as meadow.
   ponytail: coarse search, a few hundred height samples, runs once per launch. */
static void pick_camera(void) {
    const float fovy = 1.0f;          /* must match frame()'s perspective */
    const float want = 0.40f;         /* target summit elevation, radians */
    float start_az, start_elev;
    sun_angles(day_hours, &start_az, &start_elev);
    float best = 1e9f, bx = 0.0f, bz = 0.0f, bEye = 0.0f;
    /* Distance at which a summit peak_y above eye height sits at `want`.
       Capped: a tall peak would otherwise put the camera so far back that the
       frame is all foreground meadow, which is what a bigger island does. */
    const float want_d = fminf((peak_y - 8.0f) / tanf(want), 620.0f);
    for (int k = 0; k < 32; k++) {
        float a = (float)k * 6.28318531f / 32.0f;
        /* try every radius, not just the first land one: breaking at the first
           hit always keeps the farthest reachable vantage, which frames the
           summit small */
        for (int si = 0; si <= 10; si++) {
            float d = want_d * (1.0f - 0.06f * (float)si);
            float x = peak_x + cosf(a) * d, z = peak_z + sinf(a) * d;
            float h = surface_height(x, z);
            if (h < 4.0f) continue;
            /* a lake basin is flat ground at the water surface, so the height
               test passes and the camera stands in the water */
            if (lake_at(x, z)) continue;
            float eye = h + 7.0f;                          /* standing on it */
            float dx = peak_x - x, dz = peak_z - z;
            float dist = sqrtf(dx * dx + dz * dz);
            if (dist < 60.0f) continue;                    /* nose against it */
            float elev = atan2f(peak_y - eye, dist);
            /* occluders: terrain poking above the line of sight to the summit.
               Also the tallest thing anywhere in front, as an angle: a nearer
               crest the sight line clears sideways still fills the frame, and
               that is the failure this catches -- a wall of rock with the
               summit cut off above it. */
            int blocked = 0;
            float top_ang = 0.0f;
            for (int s = 1; s < 24; s++) {
                float t = (float)s / 24.0f;
                float ray = eye + (peak_y - eye) * t;
                float g = surface_height(x + dx * t, z + dz * t);
                if (g > ray) blocked++;
                float dd = dist * t;
                if (dd > 8.0f) {
                    float ag = atan2f(g - eye, dd);
                    if (ag > top_ang) top_ang = ag;
                }
            }
            /* and trees on that line: the heightfield test alone calls the
               summit visible when it is behind a wall of pines */
            float len2 = dx * dx + dz * dz;
            if (len2 > 1e-3f) {
                for (int ti = 0; ti < tree_count; ti++) {
                    const tree_t *tr = &trees[ti];
                    float t = ((tr->x - x) * dx + (tr->z - z) * dz) / len2;
                    if (t <= 0.02f || t >= 0.98f) continue;
                    float px = x + dx * t, pz = z + dz * t;
                    float ox = tr->x - px, oz = tr->z - pz;
                    if (ox * ox + oz * oz > tr->r * tr->r * 1.44f) continue;
                    float ray = eye + (peak_y - eye) * t;
                    if (tr->y < ray && tr->y + tr->h > ray) blocked++;
                }
            }
            /* flat ground reads as meadow, steep as cliff. Measured over ONE
               cell, not two: a facet is ~20 units across, so averaging over 58
               units calls a single steep slab flat and the camera ends up
               standing on it with rock filling half the frame. */
            float e = CELL;
            float sx = surface_height(x + e, z) - surface_height(x - e, z);
            float sz = surface_height(x, z + e) - surface_height(x, z - e);
            float slope = sqrtf(sx * sx + sz * sz) / (2.0f * e);
            /* A steep spot is penalised, not rejected: rejecting every steep
               candidate can leave the search with none at all, and the
               fallback is a fixed world position that means nothing on a
               generated island -- the camera ends up at the mountain's foot. */
            float cost = 0.0f;
            if (slope > 0.36f) cost += 40.0f;
            /* Stand in the valley, not on the mountain. A shoulder of the
               massif matches the target summit elevation exactly -- and then
               the mountain's own flank fills the bottom half of the frame. */
            if (terrain_mountain_mask(x, z, terrain_seed) > 0.15f) cost += 40.0f;
            cost += 4.0f * (float)blocked + 8.0f * fabsf(elev - want) + 3.0f * slope;
            /* the frame holds one summit, not a range wall: charge the search
               for anything in front that stands well above the target angle */
            if (top_ang > want * 1.2f) cost += 60.0f + 120.0f * (top_ang - want);
            /* ground immediately in front of the eye must fall away, or the
               camera stands in a hollow and a near bank fills the frame */
            for (int st = 1; st <= 6; st++) {
                float t = (float)st * 0.03f;
                if (surface_height(x + dx * t, z + dz * t) > eye - 2.0f) cost += 4.0f;
            }
            /* And the ground has to keep falling for the first ~50 units. On
               level meadow the eye sits 7 up, everything ahead is 7 down, the
               test above passes -- and the near ground is one plane filling
               the bottom third of the frame. A brow that drops away gives the
               foreground facets instead. */
            {
                float l = sqrtf(dx * dx + dz * dz);
                if (l > 1.0f) {
                    float ux = dx / l, uz = dz / l;
                    float ahead = surface_height(x + ux * 50.0f, z + uz * 50.0f);
                    if (ahead > eye - 5.0f) cost += 8.0f;
                }
            }
            /* do not stand the camera in the massif's shadow: with a low sun
               that is most of the valley, and the whole foreground renders as
               one dark slab */
            {
                float ch[SUN_SECTORS];
                horizon_at(x, z, h, 1, ch);
                if (shadow_lit(ch, start_az, start_elev) < 0.5f) cost += 20.0f;
            }
            /* A tree within 12 units is in the lens and disqualifying; a few
               within 30 is normal in a forest. Charging the full penalty out
               to 45 made the search prefer treeless ground -- which is bare
               rock and beach, so the camera parked at the foot of the massif
               looking up it. */
            int in_lens = 0;
            for (int ti = 0; ti < tree_count; ti++) {
                float tdx = x - trees[ti].x, tdz = z - trees[ti].z;
                float d2 = tdx * tdx + tdz * tdz;
                if (d2 < 144.0f) { in_lens = 1; break; }
                else if (d2 < 900.0f) cost += 3.0f;
            }
            /* a trunk in the lens is a defect, not a cost: as a penalty the
               search could still buy it when every other candidate looked
               worse, which is how the camera ended up inside a thicket with a
               canopy filling a corner of the frame */
            if (in_lens) continue;
            if (cost < best) { best = cost; bx = x; bz = z; bEye = eye; }
        }
    }
    if (best > 1e8f) return;   /* nothing usable: keep the fixed default */
    cam_x = bx; cam_z = bz; cam_y = bEye;
    float dx = peak_x - cam_x, dz = peak_z - cam_z;
    yaw = tyaw = atan2f(dx, dz);
    float elev = atan2f(peak_y - cam_y, sqrtf(dx * dx + dz * dz));
    /* summit ~12% below the top edge: aim below it by 0.76 of a half frame */
    pitch = tpitch = elev - 0.76f * 0.5f * fovy;
    if (pitch < -0.55f) pitch = tpitch = -0.55f;
    if (pitch > 0.25f) pitch = tpitch = 0.25f;
}

/* --- day/night palette ------------------------------------------------- */
static void lerp3(float *o, const float *a, const float *b, float t) {
    o[0] = a[0] + (b[0]-a[0])*t; o[1] = a[1] + (b[1]-a[1])*t; o[2] = a[2] + (b[2]-a[2])*t;
}
static float ramp01(float a, float b, float x) {
    float t = (x - a) / (b - a);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

/* Rayleigh extinction per airmass. The ratios are the physical ones -- blue is
   scattered out of the beam fastest -- and the scale is set so that a high sun
   is white-ish. Everything else the palette needs (sun colour, sky gradient,
   ambient, fill) is then derived from this one number. */
static const float BETA_R[3] = { 0.085f, 0.190f, 0.400f };
/* the pale sky the art direction asks for: the model tints toward this rather
   than toward a physical deep blue, and the reference sky is this colour */
static const float SKY_PALE_Z[3] = { 0.64f, 0.78f, 0.75f };
static const float SKY_PALE_H[3] = { 0.88f, 0.88f, 0.82f };
/* scattered light that is no longer the sun's own colour: what fills a shadow */
static const float SKY_LIFT[3]   = { 0.100f, 0.150f, 0.270f };
/* twilight zenith: what is left overhead once the warm light is all near
   the horizon. Hand-set because it is a look, not a transmittance. */
static const float SKY_TWILIGHT[3] = { 0.30f, 0.29f, 0.44f };

/* Night is not sunlight, so the last palette keeps its own hand-set values:
   there is no atmosphere model for a moon. */
static const float NIGHT_ZEN[3]  = { 0.030f, 0.045f, 0.100f };
static const float NIGHT_HOR[3]  = { 0.075f, 0.095f, 0.170f };
static const float NIGHT_SUN[3]  = { 0.42f, 0.50f, 0.72f };   /* moonlight */
static const float NIGHT_AMBS[3] = { 0.075f, 0.095f, 0.160f };
static const float NIGHT_AMBG[3] = { 0.030f, 0.035f, 0.055f };
static const float NIGHT_FILL[3] = { 0.020f, 0.030f, 0.060f };

/* Fills pal[] with the 12 vec4s the shaders expect. Sun and moon share one
   code path: at night the "sun" is simply the body on the other side of the
   sky, which keeps one shadow direction and one disc. */
/* Where the light is at a given hour. Night reuses the same body on the other
   side of the sky, so there is one light direction, one disc and one shadow
   term to reason about. */
/* Signed elevation, one source of truth: build_palette needs the sign (to know
   whether the sun is up) and sun_angles wants the magnitude. */
static float sun_elevation(float hours) {
    return sinf((hours - 6.0f) / 12.0f * 3.14159265f) * 1.05f;   /* 0 at sunrise */
}

static void sun_angles(float hours, float *az, float *elev) {
    float a = (hours - 6.0f) / 12.0f * 3.14159265f;
    float e = sun_elevation(hours);
    float le = e < 0.0f ? -e : e;
    float laz = (e < 0.0f ? a + 3.14159265f + 2.85f : a + 2.85f);
    laz = fmodf(laz, 6.28318531f);
    if (laz < 0.0f) laz += 6.28318531f;
    *az = laz;
    *elev = le;
}

static void build_palette(float hours, float *pal) {
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
static int gfx_w = 1280, gfx_h = 720;
static const int GFX_MSAA = 4;     /* silhouettes against sky are the aliasing */
static id<MTLDevice> mtl_device = nil;
static id<MTLCommandQueue> mtl_queue = nil;
static id<MTLTexture> mtl_resolve = nil;   /* ours, so readback needs no digging */
static sg_image hd_color, hd_resolve, hd_depth;
static sg_view hd_color_view, hd_resolve_view, hd_depth_view;

static void headless_gfx_setup(void) {
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
        .buffer_pool_size = 2048,
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
static void headless_readback(unsigned char *dst) {
    NSUInteger row = (NSUInteger)gfx_w * 4;
    id<MTLBuffer> buf = [mtl_device newBufferWithLength:row * (NSUInteger)gfx_h
                                                options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [mtl_queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:mtl_resolve sourceSlice:0 sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake((NSUInteger)gfx_w, (NSUInteger)gfx_h, 1)
                 toBuffer:buf destinationOffset:0
        destinationBytesPerRow:row destinationBytesPerImage:row * (NSUInteger)gfx_h];
    [blit endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    memcpy(dst, [buf contents], (size_t)row * (size_t)gfx_h);
    [buf release];
}

static void init(void) {
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
        .buffer_pool_size = 2048,
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
        .depth.compare = SG_COMPAREFUNC_LESS_EQUAL,
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
    pass_action = (sg_pass_action){ .colors[0] = { .load_action = SG_LOADACTION_CLEAR, .clear_value = {SKY_R, SKY_G, SKY_B, 1.0f} } };
    /* mouse lock waits for focus: locking while the window is in the
       background would hijack the pointer the user is still using */
}

/* One fixed timestep of simulation. No GPU calls and no wall clock: the same
   scripted input produces the same camera path on any machine, which is what
   makes a replay a test rather than a different program. */
#define SMOOTH_K 17.0f    /* 1-exp(-k/60) == the old 0.25 per-frame lerp */
static float move_t = 0.0f, move_dur = 0.0f;      /* active scripted move, s */
static float move_x0, move_y0, move_z0, move_x1, move_y1, move_z1;
static void sim_step(float dt) {
    yaw += (tyaw - yaw) * (1.0f - expf(-SMOOTH_K * dt));
    pitch += (tpitch - pitch) * (1.0f - expf(-SMOOTH_K * dt));

    if (move_dur > 0.0f) {
        move_t += dt;
        float k = move_t / move_dur;
        if (k > 1.0f) k = 1.0f;
        cam_x = move_x0 + (move_x1 - move_x0) * k;
        cam_y = move_y0 + (move_y1 - move_y0) * k;
        cam_z = move_z0 + (move_z1 - move_z0) * k;
        if (k >= 1.0f) { move_dur = 0.0f; move_t = 0.0f; }
    }

    sim_frames++;
    anim_time = (float)sim_frames / 60.0f;
    /* one in-game hour per real minute */
    if (!time_fixed) {
        day_hours += dt * (24.0f / 1440.0f);
        if (day_hours >= 24.0f) day_hours -= 24.0f;
    }

    float fx = sinf(yaw) * cosf(pitch);
    float fy = sinf(pitch);
    float fz = cosf(yaw) * cosf(pitch);
    /* screen-right is cross(fwd, up) = (-cos yaw, 0, sin yaw) */
    float rx = -cosf(yaw), rz = sinf(yaw);
    float spd = (keys[SAPP_KEYCODE_LEFT_SHIFT] ? 40.0f : 12.0f) * dt;
    if (keys[SAPP_KEYCODE_W]) { cam_x += fx*spd; cam_y += fy*spd; cam_z += fz*spd; }
    if (keys[SAPP_KEYCODE_S]) { cam_x -= fx*spd; cam_y -= fy*spd; cam_z -= fz*spd; }
    if (keys[SAPP_KEYCODE_A]) { cam_x -= rx*spd; cam_z -= rz*spd; }
    if (keys[SAPP_KEYCODE_D]) { cam_x += rx*spd; cam_z += rz*spd; }
    if (getenv("FLY")) { cam_x += fx * 6.0f; cam_z += fz * 6.0f; }
    if (keys[SAPP_KEYCODE_Q]) { cam_y -= spd; }
    if (keys[SAPP_KEYCODE_E]) { cam_y += spd; }

    float ground = terrain_height(cam_x, cam_z, terrain_seed) + 2.0f;
    if (cam_y < ground) cam_y = ground;
}

/* Everything that touches the GPU. Takes the size explicitly so the same code
   serves a window and an offscreen texture. */
static void render(int w, int h) {
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
        if (chunks[i].state != CHUNK_DONE) continue;
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
        chunks[i].state = CHUNK_READY;
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
            if (chunks[i].state == CHUNK_EMPTY) emp++;
            if (chunks[i].state == CHUNK_READY) rdy++;
            if (chunks[i].state == CHUNK_READY && chunks[i].tier == 1) t1++;
            if (chunks[i].state == CHUNK_READY && chunks[i].tier == 2) t2++;
            if (chunks[i].state == CHUNK_READY && chunks[i].tier == 4) t4++;
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
            if (chunks[i].state != CHUNK_READY || chunks[i].words == 0) continue;
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
#define MAX_CATCHUP 5
static void frame(void) {
    static uint64_t last = 0;
    uint64_t now = stm_now();
    float dt = last ? (float)stm_sec(now - last) : (1.0f / 60.0f);
    last = now;
    if (dt > 0.1f) dt = 0.1f;
    int steps = (int)(dt * 60.0f + 0.5f);
    if (steps < 1) steps = 1;
    if (steps > MAX_CATCHUP) steps = MAX_CATCHUP;
    for (int i = 0; i < steps; i++) sim_step(1.0f / 60.0f);
    render(sapp_width(), sapp_height());
}

static void event(const sapp_event *ev) {
    switch (ev->type) {
    case SAPP_EVENTTYPE_KEY_DOWN:
        if (ev->key_code == SAPP_KEYCODE_ESCAPE) sapp_request_quit();
        if (ev->key_code < 512) keys[ev->key_code] = 1;
        break;
    case SAPP_EVENTTYPE_KEY_UP:
        if (ev->key_code < 512) keys[ev->key_code] = 0;
        break;
    case SAPP_EVENTTYPE_FOCUSED:
        sapp_lock_mouse(true);    /* only now is it safe to take the pointer */
        got_first_mouse = 0;
        break;
    case SAPP_EVENTTYPE_UNFOCUSED:
        sapp_lock_mouse(false);
        memset(keys, 0, sizeof(keys));   /* key held during unfocus would stick */
        break;
    case SAPP_EVENTTYPE_MOUSE_MOVE: {
        /* ponytail: locking the cursor emits one huge bogus delta as macOS
           warps the pointer to the centre. Drop the first event and clamp
           the rest, or the camera snaps/spins on its own. */
        if (!got_first_mouse) { got_first_mouse = 1; break; }
        float dx = ev->mouse_dx, dy = ev->mouse_dy;
        if (dx >  200.0f) dx =  200.0f;
        if (dx < -200.0f) dx = -200.0f;
        if (dy >  200.0f) dy =  200.0f;
        if (dy < -200.0f) dy = -200.0f;
        tyaw -= dx * 0.0025f;   /* mouse right = turn right */
        tpitch -= dy * 0.0025f;
        if (tpitch >  1.35f) tpitch =  1.35f;
        if (tpitch < -1.35f) tpitch = -1.35f;
        break;
    }
    default: break;
    }
}

static void shutdown_gfx(void) {
    stop_streamer();
    sg_shutdown();
}

static void cleanup(void) {
    shutdown_gfx();
}

/* --- capture ---------------------------------------------------------------
   One frame out to a PNG plus a sidecar of numbers, blocking until the
   streamer has caught the camera up. `dt` is never advanced while waiting:
   otherwise a busy machine would render a different camera than an idle one
   and the same replay would produce two different images. */
#define SHOT_MAX_PUMP 6000    /* frames of upload pumping before we give up */

static unsigned char *cap_buf = NULL;   /* one allocation, large enough for any
                                           shot in a replay, reused */

static void shrink_to(unsigned char *dst, int dw, int dh,
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
            o[0] = (unsigned char)(r / n); o[1] = (unsigned char)(g / n);
            o[2] = (unsigned char)(b / n); o[3] = (unsigned char)(a / n);
        }
    }
}

static char *with_suffix(const char *path, const char *suffix) {
    size_t n = strlen(path), m = strlen(suffix);
    char *o = (char*)malloc(n + m + 1);
    memcpy(o, path, n); memcpy(o + n, suffix, m + 1);
    return o;
}

static void capture(const char *path) {
    /* do/while, not while: before the first render nothing is queued yet, so
       world_ready() is trivially true and the capture would photograph an
       empty world. The first render is what asks the streamer for chunks. */
    int pumped = 0;
    do {
        render(gfx_w, gfx_h);
        if (++pumped > SHOT_MAX_PUMP) {
            fprintf(stderr, "capture %s: world never became ready\n", path);
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
static double replay_now = 0.0;     /* sim seconds since the script started */
static int render_ok = 1;           /* 0 in `dump` mode: sim only, no GPU */
static int replay_line = 0;
static const char *replay_name = "replay";

static void replay_die(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s:%d: ", replay_name, replay_line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void advance(float seconds) {
    int n = (int)lroundf(seconds * 60.0f);
    if (n < 0) n = 0;
    for (int i = 0; i < n; i++) sim_step(1.0f / 60.0f);
    replay_now += (double)n / 60.0;
}

static void key_by_name(const char *name, int down) {
    int code = -1;
    if (!strcmp(name, "W")) code = SAPP_KEYCODE_W;
    else if (!strcmp(name, "A")) code = SAPP_KEYCODE_A;
    else if (!strcmp(name, "S")) code = SAPP_KEYCODE_S;
    else if (!strcmp(name, "D")) code = SAPP_KEYCODE_D;
    else if (!strcmp(name, "Q")) code = SAPP_KEYCODE_Q;
    else if (!strcmp(name, "E")) code = SAPP_KEYCODE_E;
    else if (!strcmp(name, "SHIFT")) code = SAPP_KEYCODE_LEFT_SHIFT;
    else replay_die("unknown key '%s'", name);
    keys[code] = down;
}

/* Render-derived counters cannot be asserted in `dump` mode, which never calls
   render(): they would be zero and every script would need two variants. */
static int key_needs_render(const char *field) {
    return !strcmp(field, "tris") || !strcmp(field, "chunks_drawn");
}

static float key_state(const char *field) {
    if (!strcmp(field, "cam_x")) return cam_x;
    if (!strcmp(field, "cam_y")) return cam_y;
    if (!strcmp(field, "cam_z")) return cam_z;
    if (!strcmp(field, "yaw")) return yaw;
    if (!strcmp(field, "pitch")) return pitch;
    if (!strcmp(field, "hour")) return day_hours;
    if (!strcmp(field, "chunks_drawn")) return (float)chunks_drawn;
    if (!strcmp(field, "trees")) return (float)tree_count;
    if (!strcmp(field, "shadowed_pct")) return (float)shadowed_pct();
    if (!strcmp(field, "tris")) return (float)mesh_tris;
    replay_die("assert: unknown field '%s'", field);
    return 0.0f;
}

/* `assert field OP value` with OP one of > < >= <= == != */
static void replay_assert(const char *rest) {
    char field[64], op[4];
    double want;
    if (sscanf(rest, "%63s %3s %lf", field, op, &want) != 3)
        replay_die("assert: expected `assert <field> <op> <value>`");
    float got = key_state(field);
    if (!render_ok && key_needs_render(field)) {
        printf("%s:%d: skip assert %s (no rendering in this mode)\n",
               replay_name, replay_line, field);
        return;
    }
    int ok = 0;
    if (!strcmp(op, ">"))  ok = got >  want;
    else if (!strcmp(op, "<"))  ok = got <  want;
    else if (!strcmp(op, ">=")) ok = got >= want;
    else if (!strcmp(op, "<=")) ok = got <= want;
    else if (!strcmp(op, "==")) ok = fabsf(got - (float)want) < 1e-4f;
    else if (!strcmp(op, "!=")) ok = fabsf(got - (float)want) >= 1e-4f;
    else replay_die("assert: unknown operator '%s'", op);
    if (!ok) replay_die("%s is %.2f, expected %s %g (t=%.2fs)", field, got, op, want, replay_now);
}

/* `seed N` only makes sense before anything has been simulated or rendered,
   and the world is built in init(); so the value is pre-scanned by the caller
   and a seed line anywhere else is an error. */
static void replay_run(const char *path, int render_enabled) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "replay: cannot open %s\n", path); exit(1); }
    replay_name = path;
    char line[512];
    int executed = 0;
    while (fgets(line, sizeof line, f)) {
        replay_line++;
        char *hash = strchr(line, '#');      /* comments, including trailing */
        if (hash) *hash = 0;
        char *cmd = strtok(line, " \t\r\n");
        if (!cmd) continue;
        char *rest = strtok(NULL, "\r\n");
        if (!rest) rest = (char*)"";
        while (*rest == ' ' || *rest == '\t') rest++;

        if (!strcmp(cmd, "seed")) {
            if (executed) replay_die("seed must be the first command");
            continue;                        /* applied before init(), see main */
        }
        executed++;
        if (!strcmp(cmd, "wait")) {
            advance((float)atof(rest));
        } else if (!strcmp(cmd, "time")) {
            day_hours = (float)atof(rest);
            time_fixed = 1;
        } else if (!strcmp(cmd, "cam")) {
            if (sscanf(rest, "%f,%f,%f", &cam_x, &cam_y, &cam_z) != 3)
                replay_die("cam: expected x,y,z");
            move_dur = 0.0f;
        } else if (!strcmp(cmd, "look")) {
            float y = 0, p = 0;
            if (sscanf(rest, "%f %f", &y, &p) != 2) replay_die("look: expected yaw pitch");
            yaw = tyaw = y * 3.14159265f / 180.0f;
            pitch = tpitch = p * 3.14159265f / 180.0f;
            have_view = 1;
        } else if (!strcmp(cmd, "move")) {
            char over[32] = {0};
            float dur = 0.0f;
            int n = sscanf(rest, "%f,%f,%f over %fs", &move_x1, &move_y1, &move_z1, &dur);
            if (n == 4) {
                move_x0 = cam_x; move_y0 = cam_y; move_z0 = cam_z;
                move_dur = dur > 0.0f ? dur : 0.0f;
                move_t = 0.0f;
            } else if (n == 3) {
                cam_x = move_x1; cam_y = move_y1; cam_z = move_z1;
                move_dur = 0.0f;
            } else {
                replay_die("move: expected x,y,z [over seconds]");
            }
            (void)over;
        } else if (!strcmp(cmd, "key")) {
            char name[32], action[32] = {0};
            float secs = 0.0f;
            int n = sscanf(rest, "%31s %31s %f", name, action, &secs);
            if (n < 2) replay_die("key: expected KEY down|up|hold [seconds]");
            if (!strcmp(action, "down")) key_by_name(name, 1);
            else if (!strcmp(action, "up")) key_by_name(name, 0);
            else if (!strcmp(action, "hold")) {
                if (n < 3) replay_die("key: `hold` needs a duration");
                key_by_name(name, 1);
                advance(secs);
                key_by_name(name, 0);
            } else replay_die("key: unknown action '%s'", action);
        } else if (!strcmp(cmd, "shot")) {
            if (!*rest) replay_die("shot: expected a path");
            if (!render_enabled) continue;       /* `dump` mode */

            capture(rest);
        } else if (!strcmp(cmd, "assert")) {
            replay_assert(rest);
        } else {
            replay_die("unknown command '%s'", cmd);
        }
    }
    fclose(f);
    printf("replay ok: %s (%d commands, t=%.2fs)\n", path, executed, replay_now);
}

/* --- entry point -----------------------------------------------------------
   No sokol_app main() (SOKOL_NO_ENTRY): the window is one mode among four.
   `shot` and `replay` never create a window or a drawable, so they can run with
   nothing on screen; `dump` does not even create a Metal device. */
static sapp_desc game_desc(int argc, char *argv[]);

int main(int argc, char *argv[]) {
    const char *cmd = (argc > 1 && argv[1][0] != '-') ? argv[1] : NULL;
    int first = cmd ? 2 : 1;              /* where the flags start */
    int dump_only = cmd && !strcmp(cmd, "dump");

    /* flag pass shared by every headless mode, plus the seed pre-scan for
       replays (the world is built before the script runs) */
    const char *replay_path = NULL;
    for (int i = first; i < argc; i++) {
        if (!strcmp(argv[i], "--seed") && i + 1 < argc) terrain_seed = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--time") && i + 1 < argc) { day_hours = (float)atof(argv[++i]); time_fixed = 1; }
        else if (!strcmp(argv[i], "--yaw") && i + 1 < argc) { yaw = tyaw = (float)atof(argv[++i]) * 3.14159265f / 180.0f; have_view = 1; }
        else if (!strcmp(argv[i], "--pitch") && i + 1 < argc) { pitch = tpitch = (float)atof(argv[++i]) * 3.14159265f / 180.0f; have_view = 1; }
        else if (!strcmp(argv[i], "--cam") && i + 1 < argc) {
            const char *v = argv[++i];
            cam_x = (float)atof(v);
            const char *c1 = strchr(v, ',');
            if (c1) { cam_y = (float)atof(c1 + 1);
                const char *c2 = strchr(c1 + 1, ',');
                if (c2) cam_z = (float)atof(c2 + 1); }
        }
        else if (!strcmp(argv[i], "--trees-only")) trees_only = 1;
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) shot_path = argv[++i];
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &gfx_w, &gfx_h) != 2) {
                fprintf(stderr, "--size: expected WxH\n"); return 1;
            }
        }
        else if (!strcmp(argv[i], "--replay") && i + 1 < argc) replay_path = argv[++i];
    }
    if (cmd && !strcmp(cmd, "replay")) replay_path = (argc > 2) ? argv[2] : NULL;
    if (cmd && !strcmp(cmd, "dump")) replay_path = (argc > 2) ? argv[2] : NULL;

    if (!cmd) {
        if (getenv("HEADLESS")) {            /* old escape hatch, kept honest */
            fprintf(stderr, "HEADLESS is gone: use `game shot` / `game replay`\n");
            return 1;
        }
        sapp_desc d = game_desc(argc, argv);
        sapp_run(&d);
        return 0;
    }
    if (strcmp(cmd, "shot") && strcmp(cmd, "replay") && strcmp(cmd, "dump")) {
        fprintf(stderr, "usage: game [shot|replay FILE|dump FILE] [flags]\n"
                        "  flags: --seed N --cam x,y,z --yaw DEG --pitch DEG\n"
                        "         --time HOURS --size WxH --out FILE --trees-only\n");
        return 1;
    }
    if ((!strcmp(cmd, "replay") || !strcmp(cmd, "dump")) && !replay_path) {
        fprintf(stderr, "usage: game %s FILE\n", cmd);
        return 1;
    }

    headless = 1;
    init();
    if (!strcmp(cmd, "shot")) {
        capture(shot_path);
    } else {
        /* `seed` in the script has to land before init() builds the world, so
           read the first command before doing anything else */
        FILE *f = fopen(replay_path, "r");
        if (f) {
            char line[512];
            while (fgets(line, sizeof line, f)) {
                if (line[0] == '#' || line[0] == '\n') continue;
                unsigned s;
                if (sscanf(line, "seed %u", &s) == 1) {
                    /* too late: init() already built the world at terrain_seed */
                    fprintf(stderr, "replay: `seed` must be given as --seed %u\n", s);
                    fprintf(stderr, "        (the world is built before the script runs)\n");
                    fclose(f);
                    return 1;
                }
                break;
            }
            fclose(f);
        }
        render_ok = !dump_only;
        replay_run(replay_path, render_ok);
    }
    shutdown_gfx();
    return 0;
}

sapp_desc game_desc(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc)
            terrain_seed = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--time") == 0 && i + 1 < argc) {
            day_hours = (float)atof(argv[++i]);
            time_fixed = 1;
        }
        else if (strcmp(argv[i], "--yaw") == 0 && i + 1 < argc) {
            yaw = tyaw = (float)atof(argv[++i]) * 3.14159265f / 180.0f;
            have_view = 1;
        }
        else if (strcmp(argv[i], "--pitch") == 0 && i + 1 < argc) {
            pitch = tpitch = (float)atof(argv[++i]) * 3.14159265f / 180.0f;
            have_view = 1;
        }
        else if (strcmp(argv[i], "--trees-only") == 0) trees_only = 1;
    }
    return (sapp_desc){ .init_cb=init, .frame_cb=frame, .cleanup_cb=cleanup, .event_cb=event,
        .width=1280, .height=720, .window_title="terrain", .high_dpi=true,
        .sample_count=GFX_MSAA };
}
