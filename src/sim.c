/* M1: the world as data.

   Everything here is a graph of sites and routes plus one number per site.
   No per-cell state, no fine terrain in the sim: the sim may ask the coarse
   field a question (biome, river, lake) while it is *placing* a site, and
   never afterwards. That is the rule that lets the world get bigger without
   the sim getting slower.

   The point of M1 is that the numbers can produce a story: a population that
   needs food it does not grow, ore that mines where rock is, a road that can
   carry a surplus, and a polity that goes to war over both. Run it for five
   hundred years and read the chronicle -- if nothing interesting happens, the
   model is wrong, and that is cheaper to find out here than in a city. */
#include "game.h"

#define SIM_MAX_SITES  192
#define SIM_MAX_ROUTE  520
#define SIM_MAX_POLITY  40
#define SIM_CAND_STEP   24          /* cells between candidate sites (~360 units) */
#define SIM_SPACING    700.0f
#define SIM_POP_MIN     40.0f
#define SIM_POP_MAX  12000.0f

typedef struct {
    float x, z, h;
    int biome, polity, coastal, forest_near, ore_near;
    float fertility, ore, capacity, def;
    /* live state */
    float pop, food, unrest, health;
    char name[24];
} sim_site_t;

typedef struct { int a, b; float len, quality; } sim_route_t;

typedef struct {
    int capital, at_war, war_years, was_attacker, dead;
    float ore, ambition;
    char name[24];
} sim_polity_t;

static struct {
    unsigned seed, rng;
    int nsites, nroutes, npolities, year, verbose;
    sim_site_t site[SIM_MAX_SITES];
    sim_route_t route[SIM_MAX_ROUTE];
    sim_polity_t pol[SIM_MAX_POLITY];
} S;

/* --- noise and names ------------------------------------------------------ */

static float rnd(void) {
    S.rng ^= S.rng << 13; S.rng ^= S.rng >> 17; S.rng ^= S.rng << 5;
    return (S.rng >> 8) * (1.0f / 16777216.0f);
}

static unsigned hash32(unsigned x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}

/* Names are hashed from the seed and the index, not drawn during generation,
   so a place keeps its name across sessions and across an implementation
   change that reorders the site list. */
static void place_name(char *out, unsigned seed, int id, int polity) {
    static const char *a[] = { "Ash", "Bram", "Cal", "Dor", "Eld", "Fen", "Gar",
        "Hal", "Ith", "Kel", "Mar", "Nor", "Oth", "Pel", "Quin", "Ras", "Sel",
        "Tor", "Umb", "Vale", "Wren", "Yr", "Zel", "Caw", "Grim", "Holt" };
    static const char *b[] = { "fall", "ford", "mere", "wick", "stead", "hold",
        "hollow", "reach", "crag", "moor", "dale", "gate", "rock", "water",
        "field", "bury" };
    unsigned h = hash32(seed * 2654435761u + (unsigned)id * 97u + (unsigned)polity * 7u);
    snprintf(out, 24, "%s%s", a[h % 26u], b[(h >> 8) % 16u]);
}

/* --- placement ------------------------------------------------------------ */

static float site_relief(float x, float z) {
    float lo = 1e9f, hi = -1e9f;
    for (int k = 0; k < 8; k++) {
        float a = (float)k * 0.7854f, d = (k & 1) ? 420.0f : 260.0f;
        float h = surface_height(x + cosf(a) * d, z + sinf(a) * d);
        if (h < lo) lo = h;
        if (h > hi) hi = h;
    }
    return hi - lo;
}

static int near_biome(float x, float z, int want, float d) {
    for (int k = 0; k < 6; k++) {
        float a = (float)k * 1.0472f;
        float px = x + cosf(a) * d, pz = z + sinf(a) * d;
        float h = surface_height(px, pz);
        if (h < 0.0f) continue;
        float slope = 1.0f - 1.0f / sqrtf(1.0f + 0.25f);   /* ~0.11: flat enough */
        (void)slope;
        if (biome_id(px, pz, h, 0.05f, 0.5f) == want) return 1;
    }
    return 0;
}

/* A site is only planted where a person would plant one: fresh water, ground
   that grows something, wood nearby, and something to put your back against.
   The score is the site's whole fate -- capacity, fertility and unrest all come
   from here, so a badly placed town is a real problem for its polity. */
static void place_sites(void) {
    static struct { int xi, zi; float score, fert; int biome, riv, lak, forest, ore; }
        cand[4000];
    int ncand = 0;
    for (int zi = 24; zi < N - 24 && ncand < 4000; zi += SIM_CAND_STEP) {
        for (int xi = 24; xi < N - 24 && ncand < 4000; xi += SIM_CAND_STEP) {
            float x = (xi / (float)(N - 1)) * TERRAIN_WORLD - TERRAIN_WORLD * 0.5f;
            float z = (zi / (float)(N - 1)) * TERRAIN_WORLD - TERRAIN_WORLD * 0.5f;
            float h = sample_height(x, z);
            if (h < 8.0f || h > 1400.0f) continue;   /* drowned or above the tree line */
            float dhx = (sample_height(x + CELL, z) - sample_height(x - CELL, z)) / (2.0f * CELL);
            float dhz = (sample_height(x, z + CELL) - sample_height(x, z - CELL)) / (2.0f * CELL);
            float slope = 1.0f - 1.0f / sqrtf(1.0f + dhx * dhx + dhz * dhz);
            if (slope > 0.22f) continue;             /* a cliff is not a town */
            float lat = zi / (float)(N - 1);
            int biome = biome_id(x, z, h, slope, lat);
            if (biome == BIOME_ROCK || biome == BIOME_SNOW || biome == BIOME_SAND) continue;

            float rq = river_query(x, z, NULL, NULL, NULL, NULL);
            int riv = rq < 300.0f;
            int lak = lake_at(x, z) || lake_at(x + 220.0f, z) || lake_at(x - 220.0f, z) ||
                      lake_at(x, z + 220.0f) || lake_at(x, z - 220.0f);
            int forest = near_biome(x, z, BIOME_FOREST, 300.0f);
            int ore = near_biome(x, z, BIOME_ROCK, 420.0f);
            float relief = site_relief(x, z);

            float fert = (biome == BIOME_GRASS) ? 1.0f
                       : (biome == BIOME_FOREST) ? 0.72f
                       : (biome == BIOME_STEPPE) ? 0.45f : 0.40f;
            fert *= 1.0f - slope * 2.0f;
            float score = fert * 2.0f + (riv ? 1.6f : 0.0f) + (lak ? 0.8f : 0.0f)
                        + (forest ? 0.7f : 0.0f) + (ore ? 0.6f : 0.0f)
                        + fminf(relief / 600.0f, 1.0f) * 0.5f;
            if (score < 2.2f) continue;
            cand[ncand].xi = xi; cand[ncand].zi = zi;
            cand[ncand].score = score; cand[ncand].fert = fert;
            cand[ncand].biome = biome; cand[ncand].riv = riv; cand[ncand].lak = lak;
            cand[ncand].forest = forest; cand[ncand].ore = ore;
            ncand++;
        }
    }

    /* greedy by score with a spacing floor: dense clusters of good cells are
       one town, not six */
    for (int i = 0; i < ncand; i++)
        for (int j = i + 1; j < ncand; j++)
            if (cand[j].score > cand[i].score) {
                __typeof__(cand[0]) t = cand[i]; cand[i] = cand[j]; cand[j] = t;
            }

    S.nsites = 0;
    for (int i = 0; i < ncand && S.nsites < SIM_MAX_SITES; i++) {
        float x = (cand[i].xi / (float)(N - 1)) * TERRAIN_WORLD - TERRAIN_WORLD * 0.5f;
        float z = (cand[i].zi / (float)(N - 1)) * TERRAIN_WORLD - TERRAIN_WORLD * 0.5f;
        int too_close = 0;
        for (int k = 0; k < S.nsites; k++) {
            float dx = S.site[k].x - x, dz = S.site[k].z - z;
            if (dx * dx + dz * dz < SIM_SPACING * SIM_SPACING) { too_close = 1; break; }
        }
        if (too_close) continue;
        sim_site_t *s = &S.site[S.nsites];
        memset(s, 0, sizeof *s);
        s->x = x; s->z = z; s->h = sample_height(x, z);
        s->biome = cand[i].biome;
        s->fertility = cand[i].fert;
        s->forest_near = cand[i].forest;
        s->ore_near = cand[i].ore;
        s->coastal = s->h < 90.0f;
        s->def = fminf(site_relief(x, z) / 600.0f, 1.0f);
        s->ore = cand[i].ore ? (0.35f + 0.65f * cand[i].fert) : 0.10f;
        /* food from the land, work from the water, and a roof worth having */
        s->capacity = 300.0f + 2400.0f * s->fertility
                    + (cand[i].riv ? 700.0f : 0.0f)
                    + (cand[i].lak ? 350.0f : 0.0f);
        if (s->capacity > SIM_POP_MAX) s->capacity = SIM_POP_MAX;
        s->pop = 180.0f + 0.22f * s->capacity;
        s->food = 0.5f * s->pop;
        s->health = 0.72f + 0.20f * s->fertility;
        place_name(s->name, S.seed, S.nsites, 0);
        S.nsites++;
    }
}

/* Polities are a farthest-point partition of the sites: capitals spread out as
   much as the map allows, then every site belongs to the nearest capital. That
   gives borders that follow distance, which is what the map has instead of
   politics for now. */
static void place_polities(void) {
    S.npolities = 0;
    /* ~30 polities, which is a border every twenty minutes of walking: close
       enough that a polity is a place you can learn, far enough that there are
       others to fight. */
    S.npolities = S.nsites / 4;
    if (S.npolities < 1) S.npolities = 1;
    if (S.npolities > 30) S.npolities = 30;
    if (S.npolities > SIM_MAX_POLITY) S.npolities = SIM_MAX_POLITY;

    int chosen[SIM_MAX_POLITY];
    chosen[0] = 0;
    for (int k = 1; k < S.npolities; k++) {
        int best = -1; float best_d = -1.0f;
        for (int i = 0; i < S.nsites; i++) {
            float dmin = 1e18f;
            for (int j = 0; j < k; j++) {
                float dx = S.site[i].x - S.site[chosen[j]].x;
                float dz = S.site[i].z - S.site[chosen[j]].z;
                float d = dx * dx + dz * dz;
                if (d < dmin) dmin = d;
            }
            if (dmin > best_d) { best_d = dmin; best = i; }
        }
        chosen[k] = best;
    }
    for (int k = 0; k < S.npolities; k++) {
        sim_polity_t *p = &S.pol[k];
        memset(p, 0, sizeof *p);
        p->capital = chosen[k];
        p->at_war = -1;
        p->ore = 200.0f;
        p->ambition = 0.25f + 0.6f * rnd();
        place_name(p->name, S.seed, 900 + k, 1);
    }
    for (int i = 0; i < S.nsites; i++) {
        int best = 0; float best_d = 1e18f;
        for (int k = 0; k < S.npolities; k++) {
            float dx = S.site[i].x - S.site[chosen[k]].x;
            float dz = S.site[i].z - S.site[chosen[k]].z;
            float d = dx * dx + dz * dz;
            if (d < best_d) { best_d = d; best = k; }
        }
        S.site[i].polity = best;
    }
}

/* Routes are each site's three nearest neighbours. A road is only as good as
   the ground it crosses, so a range between two towns is a real cost: that is
   what makes a pass worth taking and a siege worth holding. */
static void build_routes(void) {
    S.nroutes = 0;
    for (int i = 0; i < S.nsites; i++) {
        for (int k = 0; k < 3; k++) {
            int best = -1; float best_d = 1e18f;
            for (int j = 0; j < S.nsites; j++) {
                if (j == i) continue;
                int dup = 0;
                for (int r = 0; r < S.nroutes; r++)
                    if ((S.route[r].a == i && S.route[r].b == j) ||
                        (S.route[r].a == j && S.route[r].b == i)) { dup = 1; break; }
                if (dup) continue;
                float dx = S.site[j].x - S.site[i].x, dz = S.site[j].z - S.site[i].z;
                float d = dx * dx + dz * dz;
                if (d < best_d) { best_d = d; best = j; }
            }
            if (best < 0 || S.nroutes >= SIM_MAX_ROUTE) break;
            float len = sqrtf(best_d);
            if (len > 4200.0f) continue;             /* no road over that */
            float climb = fabsf(S.site[best].h - S.site[i].h) / (len + 1.0f);
            float q = 1.0f / (1.0f + len / 2400.0f);
            q *= 1.0f - fminf(climb * 2.2f, 0.75f);
            sim_route_t *r = &S.route[S.nroutes++];
            r->a = i; r->b = best; r->len = len;
            r->quality = q < 0.15f ? 0.15f : q;
        }
    }
}

void sim_init(unsigned seed) {
    S.seed = seed;
    S.rng = hash32(seed + 0x9e3779b9u) | 1u;
    S.year = 0;
    S.verbose = 1;
    place_sites();
    place_polities();
    build_routes();
}

/* --- the tick ------------------------------------------------------------- */

static void chronicle(const char *fmt, ...) {
    if (!S.verbose) return;
    va_list ap;
    va_start(ap, fmt);
    printf("year %4d  ", S.year);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

/* Food is the engine of everything else: it decides whether a town grows,
   starves, or has a surplus worth marching on. */
static void tick_food(float weather) {
    for (int i = 0; i < S.nsites; i++) {
        sim_site_t *s = &S.site[i];
        float prod = s->pop * (0.62f + 0.72f * s->fertility) * weather;
        float need = s->pop;
        float bal = s->food + prod - need;
        if (bal >= 0.0f) {
            s->food = fminf(bal, 0.55f * need);       /* granary, not a warehouse */
        } else {
            /* buy the deficit from whoever has a surplus, over the roads that
               can carry it */
            float want = -bal;
            for (int r = 0; r < S.nroutes && want > 0.0f; r++) {
                int j = -1;
                if (S.route[r].a == i) j = S.route[r].b;
                else if (S.route[r].b == i) j = S.route[r].a;
                if (j < 0) continue;
                sim_site_t *o = &S.site[j];
                float spare = o->food - 0.4f * o->pop;
                if (spare <= 0.0f) continue;
                float cap = 900.0f * S.route[r].quality;
                float take = fminf(fminf(spare, cap), want);
                o->food -= take;
                want -= take;
            }
            if (want > 0.0f) {
                float d = want / need;
                s->pop *= 1.0f - fminf(0.45f * d, 0.30f);
                s->unrest += 0.30f * d;
                static int last_famine[SIM_MAX_SITES];
                if (d > 0.12f && S.year - last_famine[i] > 8) {
                    last_famine[i] = S.year;
                    chronicle("famine in %s: %d short, %d left", s->name,
                              (int)want, (int)s->pop);
                }
            }
            s->food = 0.0f;
        }
        if (s->pop < SIM_POP_MIN) s->pop = SIM_POP_MIN;
    }
}

static void tick_population(void) {
    for (int i = 0; i < S.nsites; i++) {
        sim_site_t *s = &S.site[i];
        float room = 1.0f - s->pop / s->capacity;
        s->pop += s->pop * 0.026f * room * (1.0f - fminf(s->unrest, 1.0f));
        if (s->pop < SIM_POP_MIN) s->pop = SIM_POP_MIN;
        if (s->pop > SIM_POP_MAX) s->pop = SIM_POP_MAX;
        s->unrest *= 0.90f;
        if (s->unrest > 0.9f) s->unrest = 0.9f;
    }
}

static void tick_ore(void) {
    for (int k = 0; k < S.npolities; k++) {
        sim_polity_t *p = &S.pol[k];
        float mined = 0.0f;
        for (int i = 0; i < S.nsites; i++) {
            if (S.site[i].polity != k) continue;
            mined += S.site[i].ore * S.site[i].pop * 0.020f;
        }
        p->ore = fminf(p->ore + mined, 4000.0f);
    }
}

/* Plague runs down the routes, which is why the graph is the sim's atom: a
   quarantine is cutting an edge, and a mountain pass is why the north lives. */
static void tick_plague(void) {
    static unsigned char seeded[SIM_MAX_SITES];
    memset(seeded, 0, sizeof seeded);
    for (int i = 0; i < S.nsites; i++) {
        sim_site_t *s = &S.site[i];
        float risk = 0.0018f * (s->pop / 2000.0f) * (1.0f + s->unrest * 2.0f)
                   * (1.6f - s->health);
        if (rnd() > risk) continue;
        long before = (long)s->pop;
        s->pop *= 0.68f + 0.14f * rnd();
        s->unrest += 0.20f;
        s->health -= 0.05f;
        if (s->health < 0.35f) s->health = 0.35f;
        chronicle("plague in %s: %ld dead", s->name, before - (long)s->pop);
        seeded[i] = 1;
        for (int r = 0; r < S.nroutes; r++) {
            int j = S.route[r].a == i ? S.route[r].b
                  : S.route[r].b == i ? S.route[r].a : -1;
            if (j < 0 || seeded[j]) continue;
            if (rnd() > 0.35f * S.route[r].quality) continue;
            sim_site_t *o = &S.site[j];
            long ob = (long)o->pop;
            o->pop *= 0.74f;
            o->unrest += 0.15f;
            if (o->pop < SIM_POP_MIN) o->pop = SIM_POP_MIN;
            chronicle("  ...and in %s: %ld dead", o->name, ob - (long)o->pop);
            seeded[j] = 1;
        }
    }
}

/* War is the ore sink. A polity marches when it has a stockpile, a grievance
   (unrest) and an appetite; the loser gives up its outermost town. */
static void tick_war(void) {
    for (int k = 0; k < S.npolities; k++) {
        sim_polity_t *p = &S.pol[k];
        if (p->at_war < 0) {
            if (p->ore < 600.0f) continue;
            if (rnd() > 0.02f + p->ambition * 0.05f) continue;
            /* nearest other polity is the neighbour it can actually reach */
            int best = -1; float best_d = 1e18f;
            for (int j = 0; j < S.npolities; j++) {
                if (j == k) continue;
                float dx = S.site[S.pol[j].capital].x - S.site[p->capital].x;
                float dz = S.site[S.pol[j].capital].z - S.site[p->capital].z;
                float d = dx * dx + dz * dz;
                if (d < best_d) { best_d = d; best = j; }
            }
            if (best < 0) continue;
            if (rnd() > 0.5f) continue;
            p->at_war = best;
            p->was_attacker = 1;
            p->war_years = 0;
            if (S.pol[best].at_war < 0) { S.pol[best].at_war = k; S.pol[best].war_years = 0; }
            chronicle("war: %s marches on %s", p->name, S.pol[best].name);
            continue;
        }
        int e = p->at_war;
        p->war_years++;
        p->ore *= 0.82f;
        S.pol[e].ore *= 0.82f;
        /* the stronger side takes a border town every other year */
        float mine = p->ore + 0.001f * S.site[p->capital].pop;
        float theirs = S.pol[e].ore + 0.001f * S.site[S.pol[e].capital].pop;
        if (p->was_attacker && (p->war_years & 1) == 0 && mine > theirs * 1.15f) {
            /* take the enemy town nearest our capital */
            int take = -1; float bd = 1e18f;
            for (int i = 0; i < S.nsites; i++) {
                if (S.site[i].polity != e) continue;
                float dx = S.site[i].x - S.site[p->capital].x;
                float dz = S.site[i].z - S.site[p->capital].z;
                float d = dx * dx + dz * dz;
                if (d < bd) { bd = d; take = i; }
            }
            if (take >= 0) {
                S.site[take].polity = k;
                S.site[take].unrest += 0.4f;
                chronicle("%s takes %s from %s", p->name, S.site[take].name, S.pol[e].name);
                /* a polity with no towns is gone, and the cause is the war
                   that took the last one -- an unexplained extinction is a bug */
                int left = 0;
                for (int i = 0; i < S.nsites; i++) if (S.site[i].polity == e) left++;
                if (!left && !S.pol[e].dead) {
                    S.pol[e].dead = 1;
                    chronicle("%s is destroyed: %s took its last town", S.pol[e].name, p->name);
                    S.pol[e].at_war = -1;
                    p->at_war = -1;
                    p->ambition = fminf(p->ambition + 0.10f, 0.95f);
                    continue;
                }
            }
        }
        if (p->war_years > 2 && (p->ore < 150.0f || rnd() < 0.30f)) {
            chronicle("peace: %s and %s stop fighting after %d years",
                      p->name, S.pol[e].name, p->war_years);
            p->at_war = -1;
            p->ambition = fminf(p->ambition + 0.05f, 0.95f);
            S.pol[e].at_war = -1;
        }
    }
}

void sim_tick(void) {
    S.year++;
    float weather = 0.80f + 0.42f * rnd();
    tick_food(weather);
    tick_population();
    tick_ore();
    tick_plague();
    tick_war();
}

/* --- report --------------------------------------------------------------- */

static float sim_total_pop(void) {
    float t = 0.0f;
    for (int i = 0; i < S.nsites; i++) t += S.site[i].pop;
    return t;
}

/* FNV over everything the tick touches: two runs of the same seed must agree,
   or the sim is not deterministic and the "return to a changed world" promise
   is a lie. */
static unsigned long sim_checksum(void) {
    unsigned long h = 1469598103934665603ul;
    for (int i = 0; i < S.nsites; i++) {
        const unsigned char *p = (const unsigned char *)&S.site[i];
        for (size_t k = 0; k < sizeof(S.site[i]); k++) { h ^= p[k]; h *= 1099511628211ul; }
    }
    for (int i = 0; i < S.nroutes; i++) h = (h ^ (unsigned)S.route[i].a) * 1099511628211ul;
    for (int i = 0; i < S.npolities; i++) {
        const unsigned char *p = (const unsigned char *)&S.pol[i];
        for (size_t k = 0; k < sizeof(S.pol[i]); k++) { h ^= p[k]; h *= 1099511628211ul; }
    }
    return h;
}

void sim_history(unsigned seed, int years, int check) {
    sim_init(seed);
    int seen[3] = { -1, -1, -1 };
    printf("world %u: %s, %d sites, %d polities, %d routes, %.0f people\n",
           seed, terrain_gen(seed).name, S.nsites, S.npolities, S.nroutes,
           sim_total_pop());
    printf("  largest:");
    for (int k = 0; k < 3 && k < S.nsites; k++) {
        int big = -1;
        for (int i = 0; i < S.nsites; i++) {
            int used = 0;
            for (int m = 0; m < k; m++) if (seen[m] == i) used = 1;
            if (used) continue;
            if (big < 0 || S.site[i].pop > S.site[big].pop) big = i;
        }
        if (big < 0) break;
        seen[k] = big;
        printf("  %s %d", S.site[big].name, (int)S.site[big].pop);
    }
    printf("\n");

    if (check) {                              /* determinism, before the story */
        sim_init(seed); S.verbose = 0;
        unsigned long c0 = sim_checksum();
        for (int y = 0; y < 200; y++) sim_tick();
        unsigned long c1 = sim_checksum();
        sim_init(seed); S.verbose = 0;
        for (int y = 0; y < 200; y++) sim_tick();
        unsigned long c2 = sim_checksum();
        int ok = (c1 == c2);
        /* a world that has died out or exploded is a broken model, not a story */
        float pop = sim_total_pop();
        int sane = pop > 1000.0f && pop < 4000000.0f;
        /* no negative stocks: a town with negative people or negative ore is a
           bug in the flow, and it would poison every later tick */
        for (int i = 0; i < S.nsites; i++) {
            if (S.site[i].pop < SIM_POP_MIN || S.site[i].food < 0.0f ||
                S.site[i].pop > SIM_POP_MAX || S.site[i].unrest < 0.0f) sane = 0;
        }
        for (int k = 0; k < S.npolities; k++) {
            if (S.pol[k].ore < 0.0f) sane = 0;
            /* every dead polity must have lost its towns to someone */
            if (S.pol[k].dead) {
                for (int i = 0; i < S.nsites; i++)
                    if (S.site[i].polity == k) sane = 0;
            }
        }
        printf("check: deterministic=%d sane=%d (c0=%lx c1=%lx pop=%.0f)\n",
               ok, sane, c0, c1, pop);
        if (!ok || !sane) exit(1);
        return;
    }

    for (int y = 0; y < years; y++) sim_tick();

    printf("\nafter %d years:\n", years);
    for (int k = 0; k < S.npolities; k++) {
        int n = 0; float pop = 0.0f;
        for (int i = 0; i < S.nsites; i++)
            if (S.site[i].polity == k) { n++; pop += S.site[i].pop; }
        if (!n) continue;
        printf("  %-12s %3d towns %8.0f people %6.0f ore %s\n",
               S.pol[k].name, n, pop, S.pol[k].ore,
               S.pol[k].at_war >= 0 ? "(at war)" : "");
    }
    printf("total %d polities, %d sites, %.0f people\n",
           S.npolities, S.nsites, sim_total_pop());
}
