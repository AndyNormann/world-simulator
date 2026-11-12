#ifndef TERRAIN_H
#define TERRAIN_H
/* Seeded value-noise heightfield with domain warping and an island falloff.

   Everything here is a pure function of (x, z, seed) so generation is
   reproducible and the same field can be sampled for camera collision. */

#include <assert.h>
#include <math.h>
#include <stdint.h>

/* World size. The grid must scale with it to keep the cell size -- and the
   facet size, which is what the low-poly read depends on -- constant: 112 cells
   over 1600 units is 14.4 a cell, the same as the 760-unit island used to have.
   More world at the same cell size is free in look and expensive in mesh, which
   is why the mesh is built in chunks rather than one Delaunay.
   TERRAIN_N must keep 1/(max_freq) >= 4 * cell; see terrain_check_resolution. */
#define TERRAIN_N 592
#define TERRAIN_WORLD 8800.0f

#define TERRAIN_OCTAVES 3
/* Feature size is ABSOLUTE, in world units: a 555-unit wavelength, the same
   hills and ranges at any world size. (370 units was tried and the resolution
   check rejected it -- a full ridge amplitude per facet is a spike, not
   relief; 0.0018 is the finest this cell size carries.)

   This was world-relative for a while (6 features across whatever the map was),
   which was right when the map was one radial island: a 2x island with absolute
   features read as a pancake because the one landmass grew while its relief did
   not. With the continental field the geography no longer scales with the map
   -- an 8800 world is several 1600-2000 unit landmasses, not one 8800-unit
   blob -- so world-relative features just give the same island at 2x zoom.
   Absolute features make more world mean more countries. */
#define TERRAIN_BASE_FREQ 0.0018f
#define TERRAIN_AMP 225.0f
/* Fraction of AMP lost at the map edge, not a height: a fixed drop stops
   sinking the coast as soon as the amplitude goes up, and the island edge
   pokes out of the sea. 4.25 of AMP over the last 28% of the radius. */
#define TERRAIN_SEA_FALLOFF 4.25f
#define TERRAIN_FALLOFF_START 0.72f
#define TERRAIN_TERRACE_STEP 13.0f
#define TERRAIN_TERRACE_MIX 0.35f
#define TERRAIN_WARP_AMOUNT 70.0f
/* ridges are sampled at a higher frequency than fbm; the resolution check has
   to account for it or the assert passes while the crests alias */
#define TERRAIN_RIDGE_SCALE 1.5f

/* Highest frequency present in any of the height terms. */
static inline float terrain_max_freq(void) {
    float top = TERRAIN_BASE_FREQ * (float)(1u << (TERRAIN_OCTAVES - 1));
    return top * TERRAIN_RIDGE_SCALE;
}

/* Grid must sample the highest octave at least 4x per wavelength or the hills
   alias into noise. Cheap to assert, and we have been bitten by this. */
static inline void terrain_check_resolution(float cell_size) {
    float min_wavelength = 1.0f / terrain_max_freq();
    assert(min_wavelength >= 4.0f * cell_size);
}

static inline unsigned terrain_hash(int x, int z, unsigned seed) {
    unsigned h = (unsigned)x * 374761393u + (unsigned)z * 668265263u + seed * 1274126177u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

/* hash to [-1, 1] */
static inline float terrain_hashf(int x, int z, unsigned seed) {
    return (float)(terrain_hash(x, z, seed) & 0xffffffu) / 8388607.5f - 1.0f;
}

static inline float terrain_value_noise(float x, float z, unsigned seed) {
    float fx = floorf(x), fz = floorf(z);
    int xi = (int)fx, zi = (int)fz;
    float tx = x - fx, tz = z - fz;
    float u = tx * tx * (3.0f - 2.0f * tx);   /* smoothstep */
    float v = tz * tz * (3.0f - 2.0f * tz);
    float a = terrain_hashf(xi,     zi,     seed);
    float b = terrain_hashf(xi + 1, zi,     seed);
    float c = terrain_hashf(xi,     zi + 1, seed);
    float d = terrain_hashf(xi + 1, zi + 1, seed);
    return (a * (1.0f - u) + b * u) * (1.0f - v) + (c * (1.0f - u) + d * u) * v;
}

static inline float terrain_fbm(float x, float z, unsigned seed) {
    float sum = 0.0f, amp = 1.0f, norm = 0.0f, freq = TERRAIN_BASE_FREQ;
    for (int i = 0; i < TERRAIN_OCTAVES; i++) {
        sum += amp * terrain_value_noise(x * freq, z * freq, seed + (unsigned)i * 101u);
        norm += amp;
        amp *= 0.5f;
        freq *= 2.0f;
    }
    return sum / norm;
}

/* Same as terrain_fbm but only octaves [first, first+count). Used to terrace the
   broad shape while keeping the fine octaves as facet-producing detail. */
static inline float terrain_fbm_band(float x, float z, unsigned seed,
                                     int first, int count) {
    float sum = 0.0f, norm = 0.0f;
    for (int i = first; i < first + count && i < TERRAIN_OCTAVES; i++) {
        float amp = 1.0f / (float)(1 << i);
        float freq = TERRAIN_BASE_FREQ * (float)(1 << i);
        sum += amp * terrain_value_noise(x * freq, z * freq, seed + (unsigned)i * 101u);
        norm += amp;
    }
    return norm > 0.0f ? sum / norm : 0.0f;
}

/* Ridged multifractal: 1-|noise| squared, summed over octaves. Produces sharp
   crest lines instead of the rounded blobs plain fbm gives. Range 0..1. */
static inline float terrain_ridge_n(float x, float z, unsigned seed, int octaves) {
    float sum = 0.0f, amp = 0.5f, norm = 0.0f, freq = TERRAIN_BASE_FREQ;
    for (int i = 0; i < octaves; i++) {
        float n = 1.0f - fabsf(terrain_value_noise(x * freq, z * freq, seed + (unsigned)i * 233u));
        sum += amp * n * n;
        norm += amp;
        amp *= 0.5f;
        freq *= 2.0f;
    }
    return sum / norm;
}

/* Summits sit on a coarse lattice rather than in one fixed place: an island
   this size with a single peak is a plain with one cone on it. Each lattice
   cell may hold a peak at a hashed position and a hashed height, and a few are
   left empty so the view opens onto meadow. Neighbouring cells are checked
   because a peak's skirt reaches into its neighbours.
   ponytail: the 3x3 neighbourhood caps the lattice at one peak per cell; a
   clustered range would need a different layout, not a bigger loop. */
/* Land and sea come from a continental field, not from one radial dome. A
   low-frequency mask decides where the coast is, so a big world holds several
   landmasses with channels between them instead of a single concentric island.

   NOTE on units, same trap as the range frame below: terrain_fbm_band() takes
   WORLD units and applies TERRAIN_BASE_FREQ itself, so this multiplier is
   relative to the base frequency, not a frequency. 0.45 is a ~1600-unit
   wavelength, i.e. two or three landmasses across any world -- passing
   1/WORLD here would be a wavelength of 1.5 million units, which is a constant
   field and produces exactly the single island this is meant to replace. */
/* --- world archetypes ------------------------------------------------------
   Every seed picks one of four kinds of world from a weighted table. Same
   generator, different corner of its parameter space -- an archipelago world
   and an alpine world are not two generators, they are two settings.

   A pure function of the seed: terrain is sampled on the builder thread, so a
   mutable cache here would be a data race to save twenty integer ops.

   amp_mtn is the one that matters. Relief is in world units, and an 8800-unit
   world with 240 units of relief is a pancake whatever the noise does. */
typedef struct {
    const char *name;
    float amp_low;      /* lowland relief */
    float amp_mtn;      /* mountain relief */
    float mtn_gain;     /* how much of the land is mountainous */
    float cont_freq;    /* continent scale: higher = smaller, more landmasses */
    float coast_detail; /* high-frequency wobble on the shoreline */
    float talus;        /* erosion slope limit, in cells */
    int   chains;       /* number of mountain chains */
    int   rivers;       /* target river systems */
} worldgen_t;

static inline worldgen_t terrain_gen(unsigned seed) {
    unsigned h = seed * 2654435761u;
    h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
    unsigned pick = h % 100u;
    if (pick < 35u)   /* flagship: ranges with passes, high relief */
        return (worldgen_t){ "alpine",      240.0f, 1950.0f, 1.35f, 0.40f, 0.30f, 3.20f, 2, 24 };
    if (pick < 65u)   /* farmland, low hills, easy to cross */
        return (worldgen_t){ "rolling",     225.0f,  800.0f, 0.85f, 0.45f, 0.26f, 2.30f, 1, 20 };
    if (pick < 85u)   /* many landmasses, straits instead of passes */
        return (worldgen_t){ "archipelago", 210.0f, 1350.0f, 0.95f, 0.78f, 0.50f, 2.60f, 1, 14 };
    return (worldgen_t){ "highland",    300.0f, 1500.0f, 1.15f, 0.36f, 0.24f, 2.80f, 2, 18 };
}

#define TERRAIN_CONTINENT_FREQ 0.45f
static inline float terrain_continent(float x, float z, unsigned seed) {
    worldgen_t g = terrain_gen(seed);
    float c = terrain_fbm_band(x * g.cont_freq, z * g.cont_freq, seed + 211u, 0, 2);
    /* Shoreline detail: the coast is where this field crosses the waterline, so
       a shorter wavelength here turns a scalloped blob into bays, headlands and
       peninsulas. Inland it barely shows. */
    c += g.coast_detail * 0.55f * terrain_fbm_band(x, z, seed + 307u, 1, 2);
    return c;
}

/* Absolute, not a fraction of the world: mountains do not change size when
   the island does, there are just more of them. */
#define PEAK_CELL 340.0f

/* Ranges, not summits. A summit lattice gives a field of cones -- real
   mountains run in lines, bend, and have foothills that are the low end of the
   same shape rather than a separate term.

   So the mask is built in a rotating frame: one axis runs along the range, the
   other across it. The frame turns slowly with a low-frequency field, which is
   what makes a range bend over kilometres instead of standing as a wall, and
   the spine and its width wander along the range so the crest is not parallel.
   The crest detail on top of this is the existing ridged noise in
   terrain_height; this function only decides where there is a mountain at all. */
static inline float terrain_chain(float x, float z, unsigned seed, int k, float gain) {
    /* NOTE on units: terrain_fbm() takes world units (it applies BASE_FREQ
       itself), so 0.22 here is a ~1700-unit wavelength, not a 5-unit one. */
    float turn = terrain_fbm(x * 0.22f, z * 0.22f, seed + 61u + (unsigned)k * 131u);
    float ang = 0.6f + turn * 1.15f + (float)k * 1.9f;
    float ca = cosf(ang), sa = sinf(ang);
    float u =  x * ca + z * sa;                    /* along the range */
    float v = -x * sa + z * ca;                    /* across it */

    float spine = 620.0f * terrain_fbm(u * 0.9f, 0.5f + (float)k * 9.0f, seed + 71u + (unsigned)k);
    float width = 1000.0f * (0.70f + 0.44f * terrain_fbm(u * 0.55f, 3.5f, seed + 97u + (unsigned)k));
    float d = fabsf(v - spine) / width;
    if (d >= 1.0f) return 0.0f;
    float t = 1.0f - d;
    float m = t * t;

    /* Passes: the crest rises and falls along the range at a ~800-unit
       wavelength, so a chain is massifs with saddles between them rather than a
       wall. This is where crossings, borders and battles end up. */
    m *= 0.62f + 0.38f * terrain_fbm(u * 0.7f, 1.5f + (float)k * 4.0f, seed + 151u);

    /* Belts: a chain occupies part of the world, not all of it. */
    m *= 0.45f + 0.95f * terrain_fbm(x * 0.38f, z * 0.38f, seed + 83u + (unsigned)k);

    /* Ranges belong to land, and the middle of a continent is more likely to
       hold one than a coastal strip. */
    m *= 0.5f + 0.5f * (0.55f + 0.75f * terrain_continent(x, z, seed));
    m *= gain;
    return m > 1.0f ? 1.0f : m;
}

/* Several chains, each with its own angle, taken as a max: where two overlap the
   ridge is higher, and the valleys between them are the passes. */
static inline float terrain_mountain_mask(float x, float z, unsigned seed) {
    worldgen_t g = terrain_gen(seed);
    float m = 0.0f;
    for (int k = 0; k < g.chains; k++) {
        float c = terrain_chain(x, z, seed, k, g.mtn_gain);
        if (c > m) m = c;
    }
    return m;
}

/* Height in world units. Sea level is 0. */
static inline float terrain_height(float x, float z, unsigned seed) {
    /* domain warp: push the sample point around by a low-frequency noise field
       so ridges bend instead of lining up on the noise lattice */
    float wx = terrain_fbm(x * 0.30f + 31.0f, z * 0.30f + 57.0f, seed + 7u);
    float wz = terrain_fbm(x * 0.30f - 42.0f, z * 0.30f + 13.0f, seed + 13u);
    float px = x + TERRAIN_WARP_AMOUNT * wx;
    float pz = z + TERRAIN_WARP_AMOUNT * wz;

    /* mask on the raw position: the summit must not wander with the warp */
    float mask = terrain_mountain_mask(x, z, seed);
    /* How far inland this point is, from the continental field: +1 well inside
       a landmass, 0 at the shore, negative out at sea. Peaks stay inland -- a
       summit at the waterline is a cliff into the sea, and it lifts the coast
       above the shoreline so the coast stops being a coast. */
    float cont = terrain_continent(x, z, seed);
    float inland = cont * 3.0f;
    if (inland > 1.0f) inland = 1.0f;
    if (inland < 0.0f) inland = 0.0f;
    mask *= inland;

    /* lowland: rolling ground, lifted above sea level or half the basin drowns
       into ponds and the falloff never gets to make the coast. The shallow
       shelves matter more than the height: a single smooth dome gives every
       foreground facet the same slope, and the meadow reads as a plastic lawn
       instead of the reference's banks and terraces. */
    worldgen_t g = terrain_gen(seed);
    float low = g.amp_low * (0.13f + 0.16f * terrain_fbm_band(px, pz, seed, 0, 3));
    float low_step = 0.045f * g.amp_low;
    float low_shelf = floorf(low / low_step + 0.5f) * low_step;
    low += (low_shelf - low) * 0.35f;

    /* the summit: ridged noise gated by the mask. Terraced, because the
       reference mountain has stepped shelves rather than a smooth cone. */
    /* three octaves only: the fourth sits at the mesh's Nyquist and turns the
       summit into per-facet static without adding any readable shape */
    float mountain = g.amp_mtn * mask
                   * (0.20f + 0.88f * terrain_ridge_n(px * TERRAIN_RIDGE_SCALE, pz * TERRAIN_RIDGE_SCALE, seed, 3));
    float stepped = floorf(mountain / TERRAIN_TERRACE_STEP + 0.5f) * TERRAIN_TERRACE_STEP;
    mountain += (stepped - mountain) * TERRAIN_TERRACE_MIX;
    mountain += g.amp_mtn * 0.06f * mask * terrain_fbm_band(px, pz, seed, 2, 1);

    float h = low + mountain;

    /* Sea. Sunk rather than lifted: raising land as well would fight the rim
       falloff below, whose whole job is keeping the map edge under water. Where
       the continental field is low the ground drops below the waterline, so
       continents are separated by channels and the shelf around each one is as
       deep as the field is low. */
    {
        /* signed sea depth: zero on land above the shoreline, growing with the
           field's distance below it. Sinking only, never lifting. */
        float sea = (0.10f - cont) * 0.90f * g.amp_low * (1.0f - mask);
        if (sea > 0.0f) h -= sea;
    }

    /* radial island falloff so the map edge is under water instead of a cliff.
       Measured noise range is only about +-0.6, so the falloff must stay near
       zero across the interior or it drowns every hill. */
    float d = sqrtf(x * x + z * z) / (TERRAIN_WORLD * 0.5f);
    float t = d > TERRAIN_FALLOFF_START ? d - TERRAIN_FALLOFF_START : 0.0f;
    h -= t * t * TERRAIN_SEA_FALLOFF * TERRAIN_AMP;
    return h;
}

/* Two-pass Lipschitz clamp: no cell may stand more than `step` above a
   neighbour. Two sweeps (forward and reverse) propagate the bound across the
   whole field, so a one-cell needle is pulled down no matter how tall it is.

   This exists because raising relief for the alpine archetype produced
   literal needles: a single cell standing 800 units above the meadow, which
   renders as a spike and parks the auto-camera staring at its tip. Thermal
   erosion cannot fix them -- it relaxes, it does not enforce. A cliff face
   survives because it reaches its height over many cells; a needle does not. */
static inline void terrain_despike(float *h, int n, float step) {
    for (int i = 0; i < n * n; i++) {
        int x = i % n, z = i / n;
        float m = h[i];
        if (x > 0     && h[i - 1] + step < m) m = h[i - 1] + step;
        if (z > 0     && h[i - n] + step < m) m = h[i - n] + step;
        h[i] = m;
    }
    for (int i = n * n - 1; i >= 0; i--) {
        int x = i % n, z = i / n;
        float m = h[i];
        if (x + 1 < n && h[i + 1] + step < m) m = h[i + 1] + step;
        if (z + 1 < n && h[i + n] + step < m) m = h[i + n] + step;
        h[i] = m;
    }
}

/* Thermal erosion: material above the talus angle slides to lower neighbours.
   Not hydraulic erosion -- no water transport, no sediment, no river carving.
   ponytail: 4-neighbour thermal pass; upgrade to droplet/hydraulic only if the
   valleys need to read as water-carved. */
static inline void terrain_erode(float *h, float *scratch, int n, float cell,
                                 int iterations, float talus) {
    (void)cell;
    for (int it = 0; it < iterations; it++) {
        for (int i = 0; i < n * n; i++) scratch[i] = h[i];
        for (int z = 0; z < n; z++) {
            for (int x = 0; x < n; x++) {
                int i = z * n + x;
                const int nb[4][2] = { {1,0}, {-1,0}, {0,1}, {0,-1} };
                for (int k = 0; k < 4; k++) {
                    int nx = x + nb[k][0], nz = z + nb[k][1];
                    if (nx < 0 || nz < 0 || nx >= n || nz >= n) continue;
                    int j = nz * n + nx;
                    float drop = scratch[i] - scratch[j];
                    if (drop > talus) {
                        float move = (drop - talus) * 0.25f;
                        h[i] -= move;
                        h[j] += move;
                    }
                }
            }
        }
    }
}

#endif
