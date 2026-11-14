/* Self-check for src/terrain.h. Build+run: ./build.sh test */
#include "../src/terrain.h"

/* the game sets this per world; the tests use the default coastline */
float world_sea = 0.10f;
/* --world override, defined in src/world.c: -1 means "ask the seed" */
int terrain_force = -1;
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N TERRAIN_N
#define CELL (TERRAIN_WORLD / (float)(N - 1))

static void sample_grid(float *h, unsigned seed) {
    for (int zi = 0; zi < N; zi++)
        for (int xi = 0; xi < N; xi++) {
            float x = (xi / (float)(N - 1)) * TERRAIN_WORLD - TERRAIN_WORLD * 0.5f;
            float z = (zi / (float)(N - 1)) * TERRAIN_WORLD - TERRAIN_WORLD * 0.5f;
            h[zi * N + xi] = terrain_height(x, z, seed);
        }
}

static float *grid_a, *grid_b, *grid_c;

static void test_resolution_is_sane(void) {
    /* Highest octave must be sampled >= 4x per wavelength. */
    terrain_check_resolution(CELL);
    assert(1.0f / terrain_max_freq() >= 4.0f * CELL);
}

static void test_deterministic_and_seeded(void) {
    sample_grid(grid_a, 1234u);
    sample_grid(grid_b, 1234u);
    assert(memcmp(grid_a, grid_b, sizeof(float) * N * N) == 0);

    sample_grid(grid_c, 4321u);
    assert(memcmp(grid_a, grid_c, sizeof(float) * N * N) != 0);
}

static void test_heights_finite_and_bounded(void) {
    sample_grid(grid_a, 7u);
    for (int i = 0; i < N * N; i++) {
        assert(isfinite(grid_a[i]));
        /* bound spans rolling (~430) to alpine (~1950 * 1.08 + lowland), with
           headroom: the number is a sanity ceiling, not a design target */
        assert(grid_a[i] > -1500.0f && grid_a[i] < 2600.0f);
    }
}

static void test_island_edges_are_underwater(void) {
    /* the radial falloff should drop the border below sea level */
    float r = TERRAIN_WORLD * 0.5f - CELL;
    for (int k = 0; k < 8; k++) {
        float a = (float)k * 3.14159265f / 4.0f;
        float h = terrain_height(cosf(a) * r, sinf(a) * r, 99u);
        assert(h < 0.0f);
    }
}

static void test_relief_matches_the_archetype(void) {
    /* Relief is the number the whole game is built on: an alpine world with a
       rolling world's relief is a pancake you cannot climb. The old version of
       this test asserted a maximum slope, which was a proxy for aliasing -- but
       slopes are now deliberately steep (cliffs and ranges are what the player
       climbs), so the proxy banned the feature. The Nyquist guard lives in
       terrain_check_resolution; this checks the design number instead. */
    unsigned seeds[] = { 1u, 2u, 6u, 8u, 12u, 55u, 99u, 2024u };
    for (unsigned k = 0; k < sizeof seeds / sizeof seeds[0]; k++) {
        worldgen_t g = terrain_gen(seeds[k]);
        sample_grid(grid_a, seeds[k]);
        float hi = -1e9f, lo = 1e9f;
        for (int i = 0; i < N * N; i++) {
            if (grid_a[i] > hi) hi = grid_a[i];
            if (grid_a[i] < lo) lo = grid_a[i];
        }
        float relief = hi - lo;
        /* generous: the mountain mask is noise-gated, so a seed can land
           anywhere from a sixth of the archetype's amplitude to its full
           value. Outside this band the generator is broken, not varied. */
        assert(relief > 0.28f * g.amp_mtn);
        assert(relief < 1.65f * g.amp_mtn);
    }
}

static void test_erosion_conserves_mass_and_flattens(void) {
    sample_grid(grid_a, 2024u);
    memcpy(grid_b, grid_a, sizeof(float) * N * N);
    double before = 0.0, after = 0.0;
    for (int i = 0; i < N * N; i++) before += grid_a[i];

    float *scratch = grid_c;
    terrain_erode(grid_b, scratch, N, CELL, 8, 1.2f);

    float max_slope_before = 0.0f, max_slope_after = 0.0f;
    for (int i = 0; i < N * N; i++) after += grid_b[i];
    for (int zi = 0; zi < N; zi++)
        for (int xi = 0; xi + 1 < N; xi++) {
            float db = fabsf(grid_a[zi*N+xi+1] - grid_a[zi*N+xi]);
            float da = fabsf(grid_b[zi*N+xi+1] - grid_b[zi*N+xi]);
            if (db > max_slope_before) max_slope_before = db;
            if (da > max_slope_after)  max_slope_after  = da;
        }

    /* thermal erosion only moves material downhill, so total mass is conserved
       (up to float error) and the steepest step must not grow */
    double drift = fabs(after - before) / (double)(N * N);
    if (!(drift < 0.05)) printf("  mass drift per cell = %.6f\n", drift);
    assert(drift < 0.05);
    if (!(max_slope_after <= max_slope_before + 1e-3f))
        printf("  slope grew: %.3f -> %.3f\n", max_slope_before, max_slope_after);
    assert(max_slope_after <= max_slope_before + 1e-3f);
}

static float *alloc(void) { return (float*)calloc((size_t)N * N, sizeof(float)); }

int main(void) {
    grid_a = alloc(); grid_b = alloc(); grid_c = alloc();
    test_resolution_is_sane();
    test_deterministic_and_seeded();
    test_heights_finite_and_bounded();
    test_island_edges_are_underwater();
    test_relief_matches_the_archetype();
    test_erosion_conserves_mass_and_flattens();
    printf("terrain: all checks passed\n");
    return 0;
}
