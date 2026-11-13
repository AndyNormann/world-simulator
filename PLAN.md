# PLAN — from a terrain demo to a world with a history

Written after a design interview on 2026-11-12. The point of this file is that a
future session (human or agent) inherits the *reasons*, not just the code. When
something here turns out to be wrong, change it here first.

## What the game is

A third-person action RPG in one continuous procedural world. The player is a
demon bound to a plague lord: fragile, fast, vertical, and not really killable —
dying banishes them, and the world keeps running.

The player's job is **sabotage in a living world**. Not clearing a map, not
conquest: finding the systemic weak point of a place (the iron mine behind the
army's weapons, the granary behind a siege, the road behind a trade) and
breaking it, then living with the response. The world reacts systemically —
patrols, curfews, quarantine, war — rather than by spawning a boss.

Progression is XP spent in skill trees (stealth, disease, raising the dead, and
more later), where XP is paid for **systemic consequence**, not for kills.
Rare specimens from named targets buy the disease tree's best nodes.

The world is old: it simulates history from the seed, keeps simulating while the
player plays, and fast-forwards through banishing. Coming back after thirty
years to find a different ruler, an inherited grudge and a rebuilt town is the
feature.

## Decisions locked, with the reasons

| # | Decision | Why |
|---|---|---|
| 1 | **Two clocks, no pause.** The sun keeps its 24-min days; a strategic tick of ~10 in-game days fires every ~90 s of play; death fast-forwards the same tick in bulk (10 years ≈ 365 ticks ≈ 1 s). | The only clock that makes "I broke their iron, their war went badly" resolve inside a session. Player actions are queued as *events* and resolved by the next tick, not applied globally on the spot. |
| 2 | **One continuous world, no strategic map.** Activity LOD instead: warm (the town you are in), cool (the region), frozen (a record). | A pause-and-enter structure makes the world admit it is a level, exactly where immersion matters. Dehydration is needed either way, so a second map is extra work for less. |
| 3 | **The sim's atom is a graph: settlements, sites and routes.** Not stockpiles alone (nothing to attack), not population agents (10k of them, too small to matter). | The player needs places to break. A burned bridge, a dead smith and an infected well must all propagate to something visible. |
| 4 | **No global per-cell data.** Per-cell data is streamed per region; only a coarse field and the graph are resident. | This is what "designed for large scale" means. Global per-cell arrays are O(world area) and are the wall at 35k units. |
| 5 | **Three tiers of agency, all emitting state.** Polity (~per tick: war, claims, expansion), settlement (~1 Hz when warm: building queue, job ratios, patrols), squad (per frame, warm radius only: move, flank, flee). | Bots that emit unit commands become an RTS in a trench coat. Emitting state means the visible NPCs are a *rendering of the numbers*: leaving a town changes nothing, and dehydration is free. |
| 6 | **The visible cast is bounded and promoted.** 8–15 named people per polity with roles, traits, memories, grudges; ordinary people are anonymous stock. Promotion happens by event (they killed you, survived you, found a body you left). The cast ages, dies and succeeds during the fast-forward. | Disguise needs someone specific; specimens need someone who matters; history needs holders of grudges. Promotion is the Nemesis loop without a writer. |
| 7 | **Bots don't play to win.** Drives (survive, grow, keep face, honour a grudge) scored over ~6 options. | A civ AI optimizes and reads as broken when it guts its own economy for a wonder. The world must be legible and plausible, not optimal. |
| 8 | **Response is discovery-driven.** A body in a cellar with no witness produces nothing; one in the market produces alert, patrols, curfew, and a polity asking questions. Heat travels along the routes. | Makes stealth tools mechanical. A kill counter would make loud play correct. |
| 9 | **All assets are generated in-engine from rules.** Characters are procedural (8–10 joint skeleton, CPU-evaluated, ~300–600 tris); buildings are walls + roof + openings assembled from parameters. | The sim owns state, so the visuals must follow the record. Disguise becomes a parameter swap, and a town's layout *is* its history. |
| 10 | **Town tiers by population:** hamlet ~30 buildings, town ~120, city ~400 / ~5,000 people. Construction takes ~2 in-game months and is visible. | One city doubles the current draw (76k tris). Destroyed towns must regrow over decades, not be sterilised. |
| 11 | **One region, 12–15 polities, city-state scale**, one fast vertical demon (~25 u/s, dash, leap, climb), anchors as the only fast travel. | Empire scale needs a strategic map (rejected in #2). Speed is cheaper than world size. |
| 12 | **XP = systemic deltas from the sim**, tagged by the event that caused them; the event log is also the chronicle and the alarm feed. Optionally steered by patron objectives. | Pays stealth as well as slaughter, and gives the chronicle, the reaction system and replay determinism from one structure. |

## The world generator

- **Archetypes**: the seed picks alpine / rolling / archipelago / highland from a
  weighted table (`terrain_gen()` in `src/terrain.h`). Same generator, different
  parameters. Alpine is the flagship.
- **Ranges as chains**: 1–2 chains, each a rotating frame around a wandering
  spine, with a saddle field along the crest so chains break into massifs with
  **passes**. Ranges are what make borders, choke points and battles meaningful.
- **Latitude bands** north-cold to south-warm, with moisture from coast distance
  and rain shadow from ranges. Scarcity is the cleanest source of wars that
  aren't arbitrary.
- **Rivers**: drainage networks traced from accumulation, count from the
  archetype, must reach the sea.
- **Targets** (per `tools/worlds.sh` numbers): land 22–38 %, ≥3 landmasses with
  ≥1 town each, ≥6 river systems reaching the sea, ≥8 viable town sites per
  landmass, relief p95−p5 ≥ 300 (alpine ~900–1400), ≥4 biomes in sane order,
  every coast reachable on foot (with climbing allowed).

## Terrain roadmap (after T)

T delivered the invariants: archetypes, chains with passes, rivers that reach the
sea, per-world sea level, latitude + moisture biomes, no needles, no rivers on
cliffs, and a 40-seed sweep as the gate. What it has not delivered is *shape*:
valleys that look carved rather than stacked, a north that looks like a north,
coasts that look like coasts, and geography the sim can name.

Order below is by payoff per session, and by dependency: nothing here is worth
doing out of order.

### 0. Gates first (½ session)
- Extend `world_stats()` with the metrics the plan promised and never asserted:
  **viable town sites per landmass** (the sim already scores them), landmasses
  with ≥1 town, every coast reachable from the peaks on foot, pass count, mean
  crest curvature, river length distribution, biome entropy. Print them on the
  sweep line; assert the ones PLAN already promised.
- `game worldmap --map plain|biome`: the biome palette made the map *truthful*
  and *less readable*; both views are wanted, and the judging tool must be the
  readable one.
- Add `tools/terrain.sh 1 40` printing the quality table (not just per-seed
  lines) so "looks better" has a trend.

### 1. Erosion that carves — **done, with a caveat** (landform valleys)
Real flow erosion was attempted and **removed**: D8 routing on this noise field
fragments into thousands of basins (accumulation tops out at ~9k cells of 1.4M,
because the routed-on surface is the filled one and its flats have no downhill
neighbour), so almost no water gathers and almost nothing erodes. Doing it
properly means a priority-flood hydraulic solver over many iterations — hours
per world. `carve_valleys()` gets the same shape by decree: a chamfer distance
transform from the traced rivers, then a trough whose depth is a fraction of the
local relief (capped 180 units) and whose width grows with depth, floored 12
units above the river's own water level and skipped near the coast (a trough at
sea level is a beach, not a valley). Measured: relief −6 %, land unchanged,
+0.1 s build.

### 2. Crests — **done**
The cause was the cross-section, not the along-axis noise: `(1-d)^2` is a cone.
`terrain_chain` now returns a trapezoid (linear flanks, a plateau on top) over a
1450-unit half-width, and `terrain_smooth_highs()` box-blurs only above 42 % of
the summit so ridged-noise needles get shoulders without touching low ground.
Seed 1 now reads as a forested island with a snow-capped peak; seed 40 as a range
with snow on the highest shoulder instead of a row of spires.

### 3. Climate — **done**
`biome_moisture()` is now its own function -- one number computed from interior
distance, a three-tap upwind rain-shadow march (250/600/1000 units, not one
probe), aspect (north-facing is wetter and colder) and latitude -- and it is used
two ways: `biome_id()` classifies with it for the sim, `biome_rgb()` blends
steppe→meadow→forest continuously with it for the eye. The treeline follows
latitude via `snow_line_at(lat) * 0.52` instead of a fixed fraction of the
summit. Hard colour bands were the worst offender: they read as paint-by-numbers
from above and as a stripe from the ground.

### 4. Coasts — partly done (shelter decides sand vs rock)
`biome_shelter()` samples the sea 300/700/1200 units west (the wind direction):
a sheltered bay gets sand, an exposed west-facing shore stays rock. Sea stacks,
tidal flats, estuaries and deltas are **not** done -- the first three need
generated geometry and the last two need the hydrology work below.

### 5. Hydrology detail — waterfalls + meanders done
Waterfalls: a river facet whose surface drops more than 14 units over two cells
is painted as foam with a faster ripple. Cheapest item on the list and the one
with the most gameplay value -- a fall is the best chokepoint a river offers.

Meanders: interior path points are pushed sideways by a slow sine whose
amplitude grows with the river's size and dies on steep reaches (mountain water
runs straight down the fall line, lowland water loops); endpoints are pinned so
tributaries still meet the trunk and the mouth still reaches the sea. Amplitude
is under a cell, so it reads in a close 3D frame, not on the 512-px map.

Floodplain widening, lake outflow and endorheic basins are still open.

### 6. Terrain exports a geography — **done** (coarse grid)
`build_geography()` fills a 64x64 grid (275 units/cell) of `geo_cell_t`:
height, slope, moisture, biome, distance to water, distance to the sea, and
whether the cell is a **pass** (ground rising on both sides along one axis, above
28 % of the summit). `sim.c` now asks only that grid -- no `sample_height()`, no
`river_query()` -- which is PLAN rule 4 made true for the sim before the
streaming work needs it. The map line reports `passes=`, so range shape changes
show up as a number. First effect on the sim: it now prefers a town on a pass or
a harbour, and food output had to be retuned because the new fertility
distribution was leaner.

### 7. Scale: coarse field + streamed regions (2–3 sessions, only when it bites)
PLAN rule 4, still unbuilt: ~256² always-resident coarse field plus per-region
64² fine fields generated on demand, `bake_ambient` per region instead of over
the world. Trigger: a world larger than 17600, or needing the memory back.

### 8. Beyond the heightfield — rocks done, overhangs not
Boulders, scree chips and sea stacks are in as scenery instances (`TREE_ROCK`)
in the same list, chunk assignment and shading path as trees: four triangles with
a jittered base and an off-centre apex, no wind sway. ~7k per world, on steep or
high ground, with the sea stacks only in shallow water on exposed shores. They
are scale cues *and* climb affordances. Dead trees, overhangs, arches and caves
are not done -- the last three need geometry with collision and a heightfield
cannot express them.

### Deliberately not planned
Long-run physically simulated erosion (hours per world), texture detail, glacier
simulation, and hydrology recomputed on the fine grid. Each costs more than the
game gets back at this stage.

## Milestones

**T — terrain nailed (now).** 8800 first for cheap iteration, then 17600
(`TERRAIN_N` 1184, `CHUNK_GRID` 50, buffer pool 4096, `MAX_TREES` 400k) —
**done**, with the seed sweep as the gate: `tools/sweep.sh 1 40` green, build
~2 s (608 ms heightfield + 1.34 s bakes/streamer), one shot ~1.2 s, 2500 chunks,
~112k trees on a 17600 map. Deliverables: archetypes, ranges with
passes, rivers that reach the sea, coastline variety, biome colour, and the loop
tools: `tools/worlds.sh` (40 worlds + stats), `tools/mapsheet.py` (one image),
`game worldmap --seed N`. Acceptance: the Q15 invariant list as asserts over a
seed sweep, plus "a person can look at the sheet and tell the archetypes apart".

**M1 — the world exists as data.** *Partly done (commit after `d7142e7`).*
`src/sim.c` builds a sites-and-routes graph (~100–130 towns, ~30 polities,
~3 routes per town), assigns each town a capacity, fertility and ore from the
terrain's own biome and river queries, and runs a yearly tick: harvest with a
weather roll, granary, trade over the roads, famine, population toward capacity,
ore extraction, plague that walks the routes, and wars that take border towns
until a polity is destroyed. `game history --seed N --years 500` prints the
chronicle; `--check` re-runs 200 years twice and asserts determinism, sane
population and no negative stocks. Cost: ~0.6 s for build + 400 years.

Still missing from M1 and deliberately deferred: **cast records** (M3 needs
them, the chronicle does not), and the **coarse field + region grid** split —
the sim currently queries the fine field only while *placing* sites and never
afterwards, which is the case the region split has to preserve; the split waits
until the world grows past 17600 and the O(world area) bake actually hurts.

**M2 — one place.** *Done (first slice).* Records became geometry: the sim
already knew where its towns were, how big they were and what they were called;
`place_settlement()` in `src/world.c` now asks it and builds the town — a street
grid of stone-and-timber houses with gable roofs, emitted through the same
`emit_tri` path as the trees, so chunking, frustum culling and the baked shading
come for free. `game shot --settlement 3` frames it, and it is byte-identical
run to run.

What decides the town: the site's **capacity** (30 houses at a fishing village,
130 at a capital) and the **ground** (a plot is rejected if its corners span more
than 6 units of height, if it is in the water, a lake or a river, or if the
whole row would run off the site — a constrained site comes out as 17 houses on
a spit, which is the terrain having an opinion). The town is one array of
footprints that placement, drawing and collision all read, so they cannot
disagree, and `settle_blocked()` stops the walker one axis at a time so you
slide along a wall instead of sticking to it.

Not done, deliberately: rotation off the grid, doors, interiors, windows, walls
around the town, a market square, and the *sim* reacting to the town being
entered. Houses are also only drawn at the finest LOD tier (a town is a few
pixels beyond 4000 units, and the coarse ground mesh is not the surface the
footprints are snapped to). `replays/town.txt` walks into a house and asserts
the wall stops it, which is the part that would otherwise rot silently.

**M3 — the player.** Procedural character, movement/climb/crouch, one stealth
kill, discovery-driven alert, XP from the event queue.

**M4 — the rest of the fantasy.** Disease tree, zombies, anchors, banishment and
the fast-forward through the same tick loop.

## Rendering hardening (flicker) — done

### What was actually wrong

Four separate things, in the order they were found:

1. **The readback raced the frame.** `headless_readback` blitted on its own
   `MTLCommandQueue` while sokol rendered on another; two queues have no
   ordering, so a shot could return the previous frame. This is why an early
   A/B of MSAA read "no difference" (it re-measured a stale file). The blit now
   goes on `sg_mtl_command_queue()`, one queue, submission order, no fence.
2. **`build_palette()` read and wrote past the end of three `RGB[3]` arrays**
   with `for (i < 4)` loops, clobbering `as[]` and inflating the hue maximum.
   Found by `tools/analyze.sh`; fixed.
3. **`FACET_SHADES` gave every triangle a random brightness in 0.64..1.32**, so
   every facet boundary was a 44% step: the single largest source of crawl on
   slopes. Now 0.82..1.18 (measured: ~25% less instability, visibly calmer).
4. **No temporal component.** 4x MSAA (later: 2x2 SSAA) antialiases one frame;
   a sub-pixel camera move still re-rasterises every edge.

### What the renderer does now

Scene pass (single sample, always offscreen) → **TAA** (jittered projection,
depth-free reprojection via an R32F linear-depth target, 3x3 neighbourhood
clamp, progressive weight `n/(n+1)` capped at 63/64) → present (window) or blit
(headless). Stills additionally render at `--ssaa 2` and box down.

MSAA is gone: a multisampled depth buffer cannot be sampled, and the TAA pass
needs depth. `GFX_MSAA` is 1 and the pipeline is written for it.

### Measured

`tools/flicker.sh 1 12 40` at 800x450, ssaa 2. `walk` is the mean frame-to-frame
change while moving 0.6 units per frame; ground is the auto-framed camera, high
is `cam -300,2500,600 pitch -35`.

| seed | static | walk (ground) | walk (high) |
|---|---|---|---|
| 1 | 0.00 % | 3.76 % | 1.69 % |
| 12 | 0.00 % | 1.24 % | 2.11 % |
| 40 | 0.00 % | 2.58 % | 2.08 % |

Before this pass the same walk was 6–12 % and the harness was measuring the
wrong thing: see below.

### The bug that made the first TAA pass useless

The first version reset the accumulator whenever the camera moved (`taa_n = 0`
on any change), which meant the blend weight was **zero on every frame while
walking** — TAA averaged nothing exactly when the player is moving. Now the
history survives motion: reprojection carries it, a 3x3 clamp bounds it, and
only a *teleport* (>50 units in one frame) or the loading transition clears it.

The second half was the sign of the jitter in the reconstruction ray: the
projection is jittered with `proj[8] -= jx`, so a pixel's sample ray is
`p + j`, not `p - j`. With the wrong sign the history landed up to a pixel away
from where it belonged — harmless on the ground, obvious at altitude.

### The flash when walking forward

`world_ready()` goes false while *any* chunk is rebuilding, and the renderer
treated that as "still loading": terrain not drawn, sky dimmed to 28 %, progress
bar drawn. Walking forward changes the horizontal distance the streamer keys on,
so tier boundaries are crossed and chunks rebuild constantly → the entire frame
flashed, several times a second. Going up does not change horizontal distance,
which is exactly why that motion was clean.

Two fixes:

- **Latch the loading state.** The overlay is for the first load only; after
  that, streaming is invisible by definition.
- **Keep the old mesh while the new one builds.** A tier change used to destroy
  the buffer and zero the word count on the spot, punching a hole in the terrain
  for however many frames the rebuild took. The buffer now lives until its
  replacement is uploaded, and the draw loop accepts READY *or* BUILDING
  (DONE is the one state where the published word count and the live buffer
  disagree, so it is skipped).

Measured by `--flicker`'s `rebuild` column, which forces every chunk to rebuild:
100 % of pixels changed before, 3–5 % after.

### Water and rocks (found by walking around)

Three unrelated-looking reports, three separate causes:

- **"Water under the trees."** Two things. Rivers were painted per *facet
  centroid*, and a channel is barely one facet wide on a steep reach, so a river
  came out as a scatter of teal confetti with gaps, flooding single facets up to
  a cell past the mask. The paint now takes the most-inside of a facet's three
  corners (from a per-*vertex* river sample, so it is cheaper than the
  centre-only test was), which makes the ribbon continuous. And trees were only
  kept one cell clear of a lake while the painted edge reaches ~0.7 cell past the
  mask, so a tree could stand on a painted facet in the shallows; the margin is
  two cells, measured against a real tree standing in a real lake.
- **"Flickering pieces of geometry."** Black quadrilaterals on sunlit cliffs
  were `TREE_ROCK` boulders emitted with `emit_tri(..., 0.12f)`, and that last
  argument *multiplies* the colour -- every boulder in the world was drawn at 12 %
  brightness. Now 0.92-1.08 like every other caller, and buried a third of their
  height deeper so a base ring that spans twenty units of slope does not show
  air under one side.
- **The chunk build got 4x slower for an afternoon.** Doing four river queries
  per facet to get the "most inside corner" pushed chunk builds past the point
  where the streamer could keep up -- the flicker `rebuild` column jumped to
  16 % and the golden images stopped being reproducible, which looked like a
  brand-new race. Sampling the river once per *vertex* fixed both. The `rebuild`
  number is also a health check on the streamer, not just on the mesh swap.

### The disappearing edge (found after the town work)

Reported as "terrain clips out early when I move the camera, so when it becomes
partially out of view it disappears". It was the chunk cull, and there were two
faults in one test:

1. **The box was not the chunk's box.** The cull sphere was sized from the
   columns (`CHUNK_WORLD * 0.75 + 90`) and centred at `cy_top / 2`, i.e. it
   assumed the ground under a chunk starts at zero. A chunk holding a 1500-unit
   mountain was represented by a sphere that missed its summit, so the chunk was
   thrown away while the summit was still on screen. Measured at a known-bad
   camera, the old test dropped **2.98 % of the visible frame**; now it is
   0.000 %. `chunk_t` gained `cy_bot`, and the extents are the chunk's real
   bounds.
2. **Plane distance was wrong by `sqrt(1+tan^2)`.** A side plane's normal is
   `(right - tan*fwd)/|...|`, so a box's extent along it is scaled by
   `sqrt(1+tan²)`; the test used the extent and dropped the factor, which
   under-measures the slack by ~11 % in a 1.0-radian fov. That is a one-chunk
   sliver at the frame edge — about 70 pixels, one frame, invisible in a still
   and obvious in motion. Both planes now use the plane-distance form, plus two
   render-target pixels of slack for the projection jitter the test does not
   apply.

The lesson is the same one the flicker work learned: the metric has to be the
thing the player complains about. **`--flicker` grew a `cull` column** — it
renders the frame, renders it again with the frustum test switched off
(`gfx_nocull`), and counts the pixels that differ. It must be exactly
`0.00 %`, because a chunk that is still on screen may not be thrown away, and no
frame-to-frame number can see this: the hole holds still.

### Still open

- Shadows are still per-vertex from the baked horizon, so distant slopes band.
  A chunk-local shadow texture is the upgrade path, and it wants the same
  single-sample pass this work introduced.
- Tree and rock silhouettes have no alpha; their edges are the remaining hard
  cases for any sampling scheme.
- `replays/streaming.txt` still catches pop, but nothing measures *tier swap*
  brightness directly: the rendered frames with and without `NOLOD=1` came out
  byte-identical, which is the acceptance criterion satisfied by accident of
  how the shading is sampled rather than by design.

## Rules that must not be broken by accident

1. Anything indexed by world cell is a bug (see #4).
2. Bots emit state; only the warm tier touches agents.
3. The tick is deterministic and separable from rendering; replay/asserts apply.
4. Player consequence goes through the event queue; local physics does not.
5. Dehydration must be invisible: the numbers persist, the representation thins.
