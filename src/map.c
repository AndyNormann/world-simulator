/* Top-down world map: one PNG per seed, no GPU, no window.

   This is the "look at forty worlds in one image" tool. It exists because
   judging terrain from a 3D screenshot is slow, subjective and only ever shows
   one corner of the map -- the thing that actually needs judging is whether the
   continents, rivers and settlement-bearing valleys are any good, and that is
   legible from straight above in one glance.

   It builds the world with build_world_data() -- the same function the game
   uses -- so a map cannot drift from the world it claims to describe. */
#include "game.h"

#define MAP_MAX 2048

static unsigned char *mpix;          /* MAP_MAX * MAP_MAX * 3, reused */
static int msize;
static unsigned char *mseen;         /* landmass flood fill scratch */

static void put(int x, int y, float r, float g, float b) {
    if (x < 0 || y < 0 || x >= msize || y >= msize) return;
    unsigned char *p = mpix + (size_t)(y * msize + x) * 3;
    p[0] = (unsigned char)(r * 255.0f);
    p[1] = (unsigned char)(g * 255.0f);
    p[2] = (unsigned char)(b * 255.0f);
}

/* Colour ramp for one land cell. Latitude comes in as t (0 = north pole of the
   map, 1 = south), so the same height reads cold in the north and warm in the
   south -- the first piece of the climate model, visible from above. */
static void land_colour(float h, float slope, float lat, float *r, float *g, float *b) {
    float cold = 1.0f - fabsf(lat - 0.5f) * 2.0f;       /* 1 at equator, 0 at edges */
    float snowline = 620.0f - 260.0f * (1.0f - cold);   /* snow starts lower up north */
    if (h > snowline)                { *r = 0.94f; *g = 0.95f; *b = 0.97f; return; }
    if (slope > 1.35f || h > 430.0f) { *r = 0.42f; *g = 0.41f; *b = 0.40f; return; }
    if (h < 8.0f)                    { *r = 0.76f; *g = 0.71f; *b = 0.52f; return; }
    /* grass: greener and deeper towards the warm end */
    float wet = 0.55f + 0.25f * cold;
    *r = 0.32f - 0.10f * wet;
    *g = 0.42f + 0.10f * wet;
    *b = 0.22f + 0.04f * wet;
    if (h > 240.0f) {                                   /* upland: drier, browner */
        float k = (h - 240.0f) / 190.0f;
        *r += 0.16f * k; *g += 0.06f * k; *b += 0.02f * k;
    }
}

/* Land fraction, landmass count, rivers, lakes, relief percentiles. Printed as
   one line so a sweep can diff the numbers instead of the pictures. */
static void report(unsigned seed) {
    int land = 0;
    for (int i = 0; i < N * N; i++) if (heights[i] > 0.0f) land++;

    /* flood fill 4-connected; count components larger than a town */
    memset(mseen, 0, (size_t)N * N);
    int masses = 0;
    int *stack = (int *)malloc(sizeof(int) * (size_t)N * N);
    if (!stack) { fprintf(stderr, "map: out of memory\n"); exit(1); }
    for (int i = 0; i < N * N; i++) {
        if (heights[i] <= 0.0f || mseen[i]) continue;
        int sp = 0, cells = 0;
        stack[sp++] = i; mseen[i] = 1;
        while (sp) {
            int c = stack[--sp];
            cells++;
            int cx = c % N, cz = c / N;
            static const int DX[4] = {1, -1, 0, 0}, DZ[4] = {0, 0, 1, -1};
            for (int k = 0; k < 4; k++) {
                int nx = cx + DX[k], nz = cz + DZ[k];
                if (nx < 0 || nz < 0 || nx >= N || nz >= N) continue;
                int ni = nz * N + nx;
                if (mseen[ni] || heights[ni] <= 0.0f) continue;
                mseen[ni] = 1; stack[sp++] = ni;
            }
        }
        if (cells > 400) masses++;
    }
    free(stack);

    int lakes = 0;
    for (int zi = 0; zi < N; zi += 4)
        for (int xi = 0; xi < N; xi += 4) {
            float x = (xi / (float)(N - 1)) * TERRAIN_WORLD - TERRAIN_WORLD * 0.5f;
            float z = (zi / (float)(N - 1)) * TERRAIN_WORLD - TERRAIN_WORLD * 0.5f;
            if (lake_at(x, z)) lakes++;
        }

    /* steepest neighbour step, in units per cell: the anti-alias number */
    float dmax = 0.0f;
    for (int zi = 0; zi < N; zi++)
        for (int xi = 0; xi + 1 < N; xi++) {
            float d = fabsf(heights[zi * N + xi + 1] - heights[zi * N + xi]);
            if (d > dmax) dmax = d;
        }

    /* relief: 5th and 95th percentile of land height */
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < N * N; i++) {
        if (heights[i] <= 0.0f) continue;
        if (heights[i] > hi) hi = heights[i];
        if (heights[i] < lo) lo = heights[i];
    }
    printf("world %u: %-11s land=%.1f%% masses=%d rivers=%d lakes=%d trees=%d "
           "h=%.0f..%.0f relief=%.0f slope=%.1f cell=%.2f\n",
           seed, terrain_gen(seed).name, 100.0f * (float)land / (float)(N * N), masses, river_count, lakes,
           tree_count, lo, hi, hi - lo, dmax / CELL, CELL);
}

void worldmap(const char *path, int size) {
    if (size < 64) size = 64;
    if (size > MAP_MAX) size = MAP_MAX;
    msize = size;
    mpix = (unsigned char *)malloc((size_t)size * size * 3);
    mseen = (unsigned char *)malloc((size_t)N * N);
    if (!mpix || !mseen) { fprintf(stderr, "map: out of memory\n"); exit(1); }

    build_world_data(terrain_seed);
    report(terrain_seed);

    /* light from the north-west, 45 degrees up: the standard cartographic
       hillshade, and the reason relief is readable at all from above */
    const float lx = -0.5f, ly = 0.7071f, lz = -0.5f;
    for (int py = 0; py < size; py++) {
        float z = (py / (float)(size - 1)) * TERRAIN_WORLD - TERRAIN_WORLD * 0.5f;
        float lat = py / (float)(size - 1);
        for (int px = 0; px < size; px++) {
            float x = (px / (float)(size - 1)) * TERRAIN_WORLD - TERRAIN_WORLD * 0.5f;
            float h = sample_height(x, z);
            float dx = sample_height(x + CELL, z) - sample_height(x - CELL, z);
            float dz = sample_height(x, z + CELL) - sample_height(x, z - CELL);
            float nx = -dx / (2.0f * CELL), nz = -dz / (2.0f * CELL);
            float inv = 1.0f / sqrtf(nx * nx + 1.0f + nz * nz);
            float shade = (nx * lx + ly + nz * lz) * inv;
            if (shade < 0.0f) shade = 0.0f;
            shade = 0.35f + 0.65f * shade;

            float r, g, b;
            float slope = sqrtf(nx * nx + nz * nz);
            if (h < 0.0f) {                        /* sea: darker as it deepens */
                float d = -h / 400.0f; if (d > 1.0f) d = 1.0f;
                r = 0.10f + 0.06f * (1.0f - d);
                g = 0.20f + 0.14f * (1.0f - d);
                b = 0.34f + 0.20f * (1.0f - d);
            } else {
                land_colour(h, slope, lat, &r, &g, &b);
            }
            float hw = 0.0f;
            if (h >= 0.0f) {
                /* rivers are 2-8 world units wide, which is a fraction of a
                   pixel here: draw at least one pixel or the map shows dashes
                   where the world has a river through every valley */
                float cell_px = TERRAIN_WORLD / (float)(size - 1);
                float dist = river_query(x, z, NULL, NULL, NULL, &hw);
                if (hw > 0.5f && dist < (hw > cell_px * 0.8f ? hw : cell_px * 0.8f)) {
                    r = 0.16f; g = 0.34f; b = 0.52f;
                }
            }
            put(px, py, r * shade, g * shade, b * shade);
        }
    }

    if (!stbi_write_png(path, size, size, 3, mpix, size * 3)) {
        fprintf(stderr, "map: cannot write %s\n", path);
        exit(1);
    }
    printf("map %s (%dx%d)\n", path, size, size);
    free(mpix); mpix = NULL;
    free(mseen); mseen = NULL;
}
