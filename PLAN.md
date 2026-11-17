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

## D — devtools: capture, regenerate, overlay — **done**

The loop that was missing was *look at the world -> notice a place -> go there ->
change the world -> look again*. It exists now: Tab opens a panel, F2 writes a
screenshot that carries the command to reproduce it, and the world can be
rebuilt under a new seed or archetype without restarting the process.

**What a world is.** `(seed, archetype)`. The generator always had four
archetypes (alpine / rolling / archipelago / highland) and picked one from a hash
of the seed; `--world NAME` and the archetype combo now force one, `auto` keeps
the old behaviour, and every stats line and reproducer command prints both. Same
seed, four worlds: peaks 1811 / 510 / 929 / 1253 units, which is the table
working and not four copies of the same pancake.

**Screenshots that reproduce themselves.** F2 (or the panel) re-renders the
current state at `--ssaa 2`, writes the PNG and prints

    ./build/game shot --seed 12 --world alpine --size 2560x1440 --ssaa 2 \
        --cam 4262.0,340.0,962.0 --yaw 180.00 --pitch -25.00 --time 9.20 --anim 1.500

That line is the deliverable, and it is exact: the windowed `--live` shot and the
headless command above produce **identical PNGs** (mean 0.000%). This cost ~30
lines because the scene already renders into its own target and the window only
receives the TAA result; the missing piece was reading back a sokol-owned texture
in the windowed path (`sg_mtl_query_image_info`). `--anim` pins the wind phase,
which is what makes the comparison possible at all — a live frame's sway phase
comes from `sim_frames`, and a shot from a fresh process has it at zero.

**Rebuild in place, and the bug its gate found.** `world_rebuild(seed)` stops the
streamer, drops the device and the targets, and builds again. The gate was
written first: an in-window rebuild must be byte-identical to a fresh process at
the same `(seed, world, camera, hour, anim)`. It was not, and the diff was 259
pixels in one corner — which turned out to be a *different river network*
(`1062` segments at level 1322 versus `1272` at 717). `build_hydrology` asks
`river_query()` which drainage cells already belong to a river, and in a rebuilt
process that question was being answered by the **previous world's** segment
bucket index, because `build_seg_index()` runs after the hydrology. A fresh
process has no index yet and scans the segments built so far. Dropping the index
at the start of the network build makes both agree, and a world rebuilt in place
now matches a fresh one in every number as well as every pixel. Every existing
gate starts a new process, so nothing else in the repo could have seen this.

**The overlay.** dear imgui v1.92.4 + `sokol_imgui.h`, one C++ TU (`src/ui.cpp`)
behind `extern "C"`; everything else stays C. Tab toggles it, Escape closes it
before it quits, F2 still works either way. While it is open the pointer is
unlocked and the camera does not move, and the world keeps running.

- *world*: seed, `random`, archetype, `rebuild` / `rebuild same seed`, plus the
  same numbers `shot` prints (build ms, peak, chunks, points, triangles,
  window size).
- *camera*: position, yaw/pitch, hour (dragging it pins the clock), walk and
  shift speed, `look at peak`, `spawn`.
- *capture*: `screenshot (F2)` and the read-only reproducer line for the current
  state.
- *debug*: no-frustum-cull, mesh-step clamp (the `NOLOD` knob as a combo), and
  the flicker probe (~5 s freeze).

It draws **inside the present pass**, in the window only: two swapchain passes in
one frame are two drawables and which one reaches the screen is the backend's
business. That, and `simgui_flush()` refusing to run inside a pass, are now
logged errors rather than a silently blank panel.

Deliberately not built: sliders for the raw `worldgen_t` fields (a 2-second
rebuild per tweak and nowhere to keep a good setting), a live re-run of the
flicker probe per panel widget, and any gameplay logic in the overlay — it reads
and writes the same globals the keyboard does.

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

### The dark quad on the meadow (found with a screenshot)

A player screenshot of a hillside came with its own reproducer command (D1
earned its keep on the first try). The frame had a **flat, hard-edged dark
polygon lying on the slope**, darker than anything around it, with trees standing
in front of it — it reads as a piece of floating geometry, and it changes shape
when the camera moves.

It was a **tree shadow**. `horizon_at()` gives every mesh point eight horizon
angles, and a crown within ~200 units of the point raised the angle in its sector
to the crown's full angular height. The mesh points are ~15 units apart (CELL
14.9) and a 10:00 shadow from a 20-unit tree is 40-60 units long, so a *single*
tree put a handful of neighbouring vertices fully into shade and the ground
showed that shade as one polygon. Two A/Bs pinned it down: removing the trees
removes the patch, and changing the hour to 13:30 removes it too (no long
shadow), while forcing LOD 1 changed nothing.

The fix is one number with a reason: a crown occludes `0.72 ×` its angular
height, because a crown is porous and its shadow is dappled, not solid. Dense
stands still shade the ground and any low sun still draws the long forest shadows
that make dusk look like dusk (the dusk golden moved by 0.17 %), but a lone tree
no longer throws a 50-unit slab across an open meadow. Walk instability at the
reported camera went 7.73 % -> 6.87 %.

### Water is not the bed (found on a lake, from a screenshot, then built)

The water *surface* used to be the terrain: lakes were flattened to the fill
level and the river channel was carved *to* the river's level, so both were
terrain painted teal, one facet at a time, at 15 units per facet. Every artifact
that produced -- a dark stripe along the river carve, a hard-edged shadow blade
across the lake, a bright sand blade at the waterline, a 40-unit wall where a
chunk boundary crossed the water, pale teal ledges where a reach sloped -- was
that one mistake wearing a different coat. It is a real surface now:

- **The bed is a basin.** `settle_lakes` runs after every carve (carve_river
  lifts the cells around a lake to the river's level, and that raised bank was
  what poked up through the water) and lowers everything within a cell of a lake
  to 2.5 under the fill level. Lower only, deliberately: raising anything puts a
  step in the bank, and a step is a straight line drawn across the lake.
- **The channel has a depth.** `carve_river` puts the bed a few units *under*
  the water (wider channels deeper) instead of at it.
- **The surface is emitted geometry**, flat where the water is still and
  following the river's own fall where it is not, so the water takes its shape
  from the water level and never from the bed's cross-section. The bed keeps its
  terrain colour underneath and is what shows as the beach wherever it rises out
  of the water -- the waterline is a real edge for the first time.
- **The sun glint was 0.9**, well past what the surface can absorb: it clipped to
  a facet-wide polygon of pure 255s with hard edges. That, not the paint, was
  the "white wedge" on the water. 0.30 now.
- **A cascade is foam.** A facet standing on end can't hold a surface: painting
  the body colour there drew a river down a cliff as a staircase of blue walls.
  Steep river facets are foam-coloured instead -- water falling, which is what is
  there -- and a reach whose level drops 14 units in two cells is whitewater.

- **The surface is clipped against the bed.** A facet straddling the waterline
  (centre in a lake cell, one corner up the bank) has its plane below the ground
  at that corner, and the ground showed through as a thin strip of sand lying on
  the water. The plane is now clipped to where it is above the bed, which is
  what makes a shoreline an edge instead of a per-facet guess. `settle_lakes`
  and `close_lakes` keep the bed off the plane in the first place: the bed under
  a lake is lowered to 2.5 under its level (the *lowest* level touching it,
  because two lakes at different fills meet), and a lake mask with a one-cell
  crack in it -- the river drains through it, so the flood leaves those cells
  dry -- is closed rather than leaving a dry band across the water.

Cost: the bed dropped a few units around lakes and rivers, so fertility moved
with it and the seed-12 town the M2 replay is written against relocated 274 units
west (centre 4262,962 -> 3988,962, 80 -> 119 buildings). The replay was
re-recorded; that relocation is the placement working, not a bug in it.

### Still open

- Shadows still band on distant slopes. The horizon is no longer marched at
  every mesh vertex -- it is a world lattice, bilinear-sampled (see below) -- but
  the *shader* still interpolates one shadow value per vertex, so the penumbra is
  as coarse as the mesh. A per-pixel shadow map (or the lattice as a texture,
  sampled in the fragment shader) is what removes the banding; with the field now
  baked once per world, that is a texture upload and two sampler bindings away,
  not a re-architecture.
- Tree and rock silhouettes have no alpha; their edges are the remaining hard
  cases for any sampling scheme.
- `replays/streaming.txt` still catches pop, but nothing measures *tier swap*
  brightness directly: the rendered frames with and without `NOLOD=1` came out
  byte-identical, which is the acceptance criterion satisfied by accident of
  how the shading is sampled rather than by design.

## The nearest chunks were quietly losing triangles (found by flying around)

Reported as "an island looks fine from a distance, then as I get closer the grass
the trees are on turns into patches of water". It is the Delaunay array, not the
water: `dt[]` is indexed by the *cumulative* triangle count, which for
Bowyer-Watson is several times the live triangulation, and `dt_add` silently
returned once it passed `MAX_DT` (5200). A dense chunk -- river mouth inside a
lake, shore inside the near tier -- lost everything after that point: 523
triangles at seed 1 with the camera in a boat channel. Holes in the nearest mesh
show the sea plane and the bed through them, which reads as water in the grass,
and only from close up, because the far tiers are coarse and never fill the
array. Dead slots are recycled now; `trioverflow` and `ptoverflow` are asserted
zero in `replays/smoke.txt` so it cannot come back silently.

## Two "open items" that were measured away

Both were on the parallel list as work items; both were closed by measuring
first, which is cheaper than a slice.

**"Decouple the water from the terrain mesh."** Everything within 4000 units is
tier 1, and tier 1 is one cell -- 14.9 units -- with `chunk_build` already
holding the shoreline at 15 units ("keep the shoreline readable"). A water
lattice finer than that would draw a surface over a bed sampled more coarsely,
which is the same class of bug as the one it was meant to fix. The real defect
that looked like water resolution was the bed chord above (see the commit that
sinks the bed under water): the shore was not too coarse, one facet was 150
units across.

**"Cross-fade the LOD tiers."** `--flicker`'s new `sprint` column (1.5 s at
shift speed) reads 18.97% / 24.86% with tiers and 18.98% / 24.85% with
`NOLOD=1`, i.e. tier changes contribute nothing you can see while moving: both
boundaries are 4000 and 8000 units away. `lod`'s 8.4% is a settled difference
between two low-poly looks, like the facet shade, not a pop.

## The shadow is a texture now (found by the lod column)

The horizon field was baked per world (see below) but still *delivered* through
two per-vertex `UBYTE4N` attributes, so the shadow's resolution was the mesh's
and a tier swap re-sampled it at new vertices. Two RGBA16F images hold the eight
azimuth sectors (four each, ~700 KB) and the fragment shader filters them, which
removed the last pixel of tier dependence there was: seed 1, high camera, total
`lod` 9.04% -> 8.40%, and with the facet shade forced off 6.42% -> 5.41% -- a
full point of that delta was the light changing with the geometry, and what
remains is the low-poly ground moving (2.99 points of facet shade + 5.41 of
chord error). The attributes went away with it: `VERT_FLOATS` 21 -> 19, no
per-point horizon array, no per-house horizon bake, no 8-bit quantisation.

The bug worth remembering is the mapping. `u_hor` is a uniform rather than a
shader constant so that growing the world cannot silently misalign it, and the
first version divided the half-extent by the world size instead of by the
lattice step: every pixel sampled texel 30, the whole world got one shadow
value, and the render came out uniformly bright -- a lighting bug whose cause
was arithmetic.

## The horizon is a world field, not a per-vertex march

`bake_horizon_grid` bakes the 8-sector horizon onto a lattice four cells apart
once per world, and `chunk_build` bilinearly samples it for each vertex. It used
to march `horizon_at` at every mesh point, which made the shadow a function of
the point set -- and the point set is what the LOD changes. The tier-swap column
of `--flicker` measured it: 9.7% of a distant frame changed brightness when every
chunk went to its finest mesh, because the new vertices re-marched the horizon
and facets flipped from lit to shadowed. Sampled from one field, the light is the
same at every tier.

It is also most of what a chunk rebuild cost: 144 horizon steps per vertex per
rebuild. World build/stream went from 3.1 s to 1.0 s.

What is left of the tier-swap number is the look, measured: 9.0% -> 6.4% with the
per-facet shade forced off, so two thirds of it is the deliberate ±18% facet
shade (a coarser mesh picks different facets) and the rest is LOD geometry.

## Making the renderer solid — the plan

Everything above was found by looking at a picture and chasing it. That is why the
same families keep coming back: a *class* of defect is only ever closed by a
number that fails when it returns. This section is the roadmap that closes the
classes rather than the instances, in the order that pays.

### The three reported families, named

1. **A surface that is not where its colour thinks it is.** A facet gets one
   colour sampled at its centroid and three corners somewhere else, so a facet
   spanning the shore is painted with the deep-bed navy, one bridging a cliff top
   is painted with meadow green, and both land as a slab that reads as a piece of
   foreign geometry. Seen at seed 1 on every island foot, and as green facets on
   the rock wall at the 003 camera.
2. **A mesh that has facets nobody designed.** The fill is a Delaunay of a
   jittered point set: along a chunk border it produces 100-150 unit triangles
   (the border ring is exactly collinear, the interior's spacing rule keeps its
   candidates a facet-width away from it), and the colour/water/normal of such a
   facet are each decided from something that does not describe it. Long facets
   are the *mechanism* behind family 1 and behind the water bars.
3. **LOD seams that separate instead of joining.** A skirt closes a *gap*
   between two tiers; nothing closes a *step*, where the coarse chunk's chord
   stands above the fine mesh beside it. That is the shelf at the foot of the
   peaks, and it is the only remaining candidate for "pops when I fly".

Plus two shading discontinuities that are cheap: the sea plane has no baked
bounce while a lake one metre away does (it renders darker, hard-edged), and the
horizon field is baked 4 cells apart (59 units), so a penumbra is a 59-unit
gradient even though it is now sampled per fragment.

### Targets — this is what "done" means

| metric (800x450, ssaa 2) | today | target |
|---|---|---|
| `static` | 0.00% | 0.00% |
| `yaw` / `pitch` (ground camera) | 0.77 / 1.20% | <= 0.5 / 0.5% |
| `walk` ground / high | 2.74 / 0.98% | <= 2.0 / 1.0% |
| `pop`: single frame, TAA history reset | 5.7 / 3.5% | <= 2% |
| `fly`: worst frame of a pan / look-up / climb | 19.8 / 26.9% | <= 8% |
| `edgelong` / `tritiny` (worst chunk) | 266 / 441 | 0 |
| `unusedpts` / `wetcoldry` / `seamgap` | 18 / 105 / 453 | 0 |
| `frame_ms` p99 | 10.5 ms | <= 16.6 ms (60 Hz) |
| counters `overflow` `trioverflow` `ptoverflow` `nonfinite` | 0, asserted | 0, asserted |
| new: `edge_long` `tri_tiny` `unused_pts` `wet_col_dry` `seam_step` | not measured | 0, asserted |

### P0 — the gates for all three — **done**

Nothing below was verifiable without this, and the five fixes before it were
found with eyes rather than tests.

- `replays/artifacts.txt`: the seven reported cameras plus the lake camera, the
  river camera and the waterfall camera, each rendering into `build/shots/` with
  the structural counters asserted. `tools/goldens.sh` grows a second set from it,
  so each fix is pinned against a picture that used to be wrong.
- `--flicker` gains **`pop`**: settle with `taa_n`/`prev_vp_valid` cleared every
  frame, so the number is the *un-smoothed* single-frame change. Every existing
  column measures a converged frame, which is exactly what hides a pop.
- `--flicker` gains **`fly`**: a scripted 3-axis flight (1 s forward at shift
  speed, 1 s yaw sweep, 1 s climb) reporting the worst single frame. This is the
  "flying around" report, made deterministic.
- a **`--seam`** mode: render the same view at `lod_clamp` 1, 2 and 4 and diff a
  band around the chunk borders the camera can see, not the whole frame.
- `chunk_build` counts five invariants (see targets) into globals, and
  `replays/smoke.txt` asserts them zero. `edge_long` alone would have caught the
  Delaunay overflow and the border slivers a week earlier.

Two of the planned items were replaced by cheaper machinery with the same
information, which is the point of writing the plan down before the code:

- the **pixel seam band** became the `seamgap` counter. Rendering three tiers and
  masking a band around chunk borders needs a depth readback and a screen-space
  border mask; counting the border-chain edges whose chord stands more than a
  unit off the ground measures the same mechanism (this chunk's boundary is not
  where its neighbour draws the ground) in fifteen lines, in worldgen, with no
  GPU. If the counter and the screen ever disagree, the band diff can come back.
- `fly` was first built as "worst frame minus the median", to separate a pop from
  steady motion. Measured, it was mostly filter warm-up and leg-to-leg noise
  (7.7% at a camera where nothing was arriving), so it is now the plain worst
  frame of the three legs -- a flying analogue of `walk` with an alarm at 45%.

What landed: `edgelong` `tritiny` `unusedpts` `wetcoldry` `seamgap` (worst single
chunk, printed in the `structure:` line and assertable in a replay), the `pop`,
`pmax` and `fly` columns, `replays/artifacts.txt` with the six reported cameras,
and `tools/check.sh` running that replay every time. The flicker probe also
settles 32 frames instead of 48 (TAA needs 16 to converge) to pay for the new
columns.

First measurements, seed 1 at 800x450 ssaa 2, ground / high camera:

| column | before P0 | now |
|---|---|---|
| `static` | 0.00 | 0.00 |
| `yaw` / `pitch` | 4.88 / 5.71 | unchanged (no geometry change) |
| `walk` / `wmax` | 6.64 / 10.06 | unchanged |
| `pop` / `pmax` | not measured | 5.72 / 7.94 (ground), 3.45 / 3.54 (high) |
| `fly` | not measured | 19.82 (ground), 26.88 (high) |

The ground camera's `pitch` of 5.71% is much worse than the 1.20% recorded
after the bed-sink commit, and nothing regressed: the 73d5afe shoreline moved the
auto-framed ground camera to a different place. That is why the columns are
alarms and why every phase below must be measured at a *named* camera.

Nothing arriving during a flight was measured, by the way: `chunks_drawn`,
`ready` and `jobs_pending()` were identical frame to frame, so the 19-27% is the
pan moving every pixel, not streaming. If a chunk ever does arrive mid-flight it
adds its own area to `fly`, which is what the column is for.

### P1 — colour from the facet, not from one point — **done**

Fix family 1 at its cause. `chunk_build` samples `bake_r/g/b` once, at the
centroid. Sample it at all three corners and use their **mean** as the facet's
flat colour: the facet look is unchanged (still one colour per facet), the
outlier disappears because a facet with two dry corners and one at sea level can
no longer be 100% submarine navy. Then assert the invariant: *no facet whose
corners are all above the waterline may take a colour whose sample cell is
underwater*. That is `wet_col_dry` in the counter list, and it is the exact bug.

Built as **the mean of the facet's dry corners when the facet straddles the
waterline**, not as a mean of all three for everything. The first version
(three corners, always) was measured and rejected: it moved 11.5% of the golden
`high` frame, because at tier 4 nearly every coastal facet has one sand corner
and one grass corner, so every shore grew a pale halo -- a global look change
bought for a handful of outliers. The straddling case is the one where the
centre sample is *actually wrong about the class* (the centre cell is seabed
while a corner stands on the beach), and that is what the fix targets: wholly dry
or wholly submerged facets keep the single centre lookup, which is also the fast
path (one bilinear fetch instead of three for ~all facets).

Measured at the committed goldens: dusk 0.136%, high 0.213%, low 0.042% -- a
real look change, but inside the 0.3% tolerance, so the goldens were not
re-recorded. The reported artifact (the navy plate at the foot of the monolith)
now takes the colour of the beach it is made of, so it reads as a sand spit
instead of a shard of foreign geometry.

`wetcoldry` stays ~105 per worst chunk and that is the honest reading: with the
rule in place it measures the *population the rule protects* (a centre sample in
the sea on a facet with dry ground), not a fault. What the crop still shows is a
100-unit sand wedge rather than a 100-unit navy wedge -- geometry, which is P2.

### P2 — no facet nobody designed — **done**

Diagnosis first, because the plan's guess was wrong in an interesting way. The
long facets were **not** the empty band beside the ring: they are a degeneracy of
the ring itself. The ring is a run of points on the cell lattice, so the fill's
circumcircle test has nothing to separate them, and Bowyer-Watson leaves some of
them out and spans 2-6 intervals with one triangle -- 100 to 350 units, aspect
ratios of 100:1. That single fault produced every variety of "facet nobody
designed": the sand wedges over water, the navy shards, the dark hairlines across
a meadow, and the seam chords standing off the ground.

What landed:

- **The ring is not a straight line.** Each border point is offset perpendicular
  by up to +-0.45 units, hashed from its own *world* cell indices (and applied
  along a fixed world axis, never "inward"), so the chunk on the other side
  computes the identical point and the chain is still shared. The size is not
  taste: `add_point` lets a point sit half a unit outside the rect, so a larger
  offset makes one side drop the point and crack the seam -- measured at +-3 and
  +-6.5 units, which decorrelated the fill better *and* left each border with a
  point that only one of the two chunks had.
- **A sliver gets a flat normal.** Under a unit of width the facet is sub-pixel
  and its normal comes from three nearly collinear points, so the direction is
  noise and the noise shaded it dark: that is the thin dark line lying across a
  meadow. It is now drawn with an up normal. Skipping it instead was tried and
  rejected -- it leaves pinholes, which read as cyan hairlines showing the sea
  through the grass.
- **The counters classify by width, not area.** A 200-by-1 sliver has 100 units
  of area and is still not a surface; `tritiny` is now "width under a unit",
  `edgelong` "longest edge past 4.5x the fill's own spacing *and* at least a unit
  wide", so each facet lands in at most one of them.
- The **world rim is excluded** from the counters (a chunk at the boundary has
  its ring clipped by the heightfield, so its fill is degenerate by construction,
  and it is the one place a player cannot stand). Those facets are real and are
  their own item.
- **TSan found three genuine races** while streaming: `cy_top`, `cy_bot` and
  `words` are written by the builder and read by the cull, which is drawing the
  previous build of the same chunk at the time. They are relaxed atomics now, and
  `sg_draw` uses the count that was loaded rather than re-reading it.

Measured at seed 1, worst chunk, before -> after:

| counter | before | after |
|---|---|---|
| `edgelong` | 266 | 2 |
| `tritiny` | 441 (area rule) | 88 (width rule) |
| `unusedpts` | 18 | 4 |
| `wetcoldry` | 105 | 41 |
| `seamgap` | 453 | 127 |

Flicker at seed 1's ground camera, same build: `yaw` 4.88 -> 0.77, `pitch` 5.71 ->
0.96, `walk` 6.64 -> 2.66, `wmax` 10.06 -> 3.83, `pop` 5.72 -> 2.42, `fly`
19.82 -> 16.10, `rebuild`/`cull` still 0.00. Points 230880 -> 196993 (-15%), the
triangle count is unchanged, and the chunk build time is unchanged: the fault was
costing vertices, not correctness of count.

Goldens re-recorded (the triangulation of the whole world changed): dusk 0.571%,
high 3.646%, low 1.282% against the old ones.

Still open from this family: one dark elongated shard at the monolith's foot is
not a sliver by the width rule and survives -- it looks like a facet spanning a
gully, which is the P3 (water/bed and cliff geometry) family.

### P3 — water is a surface, and a fall is not a mirror — **partly done**

The reported artifact was "something strange with rivers from mountains": a pale
lavender ribbon painted down a cliff, and a jumble of white plates at the foot of
a fall. Two separate faults, found by painting the vertex's own flags into the
fragment (`v_water` magenta, `v_fall` red) and reading the pixels back:

- **The ribbon was a fall shaded as terrain.** Water-ness was inferred from a
  non-zero flow vector, and a fall whose nearest river segment is degenerate has
  no flow direction, so `v_water` was 0: it took the *terrain* path -- flat, in
  the cast shadow, foam-coloured albedo times ambient light. Water now says it is
  water (`anim.x = -2`, the marker the sea quad already used) and the kind rides
  in `anim.y`.
- **A fall is not a mirror.** It went through the Fresnel mix and reflected the
  sky, which is what made the ribbon pale and flat. Aerated water scatters: falls
  now keep the facet's own light and drop the reflection.

Measured: 0.688% of the fall camera, and 0.004% of the golden `high` view (the
canonical views have no such fall), so no goldens were re-recorded. Flicker
columns unchanged.

Measured and rejected in the same pass:

- **Clipping the seabed at the sea plane.** A facet straddling y = 0 has a chord
  that stands above the water, and clipping its bed to the underwater polygon is
  what a lake does -- but a lake also emits a surface over the whole facet, and
  the sea's surface is the fixed quad *below* the chord. So the fix removes the
  beach and leaves the shard: measured, the coast went from a smooth shoreline to
  a ragged row of brown teeth. A facet wholly below the level cannot poke out at
  all (a plane through three negative heights is negative inside), so this family
  is smaller than it looked.
- **A horizon lattice at 2 cells instead of 4.** Tested against the pale line on
  the cliff in case it was the shadow field: the bake went 944 -> 1236 ms and the
  line did not change a pixel. Left at 4.

**A fall is a sheet on the rock — done.** The plates came from emitting the
surface at the reach's level per corner: every facet hung at its own height, so
neighbours overlapped and pointed different ways, and where a fall met a lake the
lake's teal plane sat among the white ones. A fall is now the bed polygon offset
0.4 units along its own normal and painted foam — continuous by construction,
because neighbouring facets share corners, and it cannot disagree with the rock
it runs down. Cost: goldens 0.021% (dusk), 0.654% (high), 0.472% (low), i.e. a
real look change in the two views that contain a fall, so they were re-recorded.

One item of the strip design was **not** needed and the plan was wrong about it:
"a strip that ends above the water ends it" -- ending the surface at the water is
what the level-per-corner rule was doing, and that is what made the plates. The
sheet ends where the bed ends, which is where the water is.

The rest of this phase was measured away:

- **A level field per corner instead of a per-facet maximum.** Untestable without
  exposing the level field to an assert, and the visible symptom it was written
  for (a step between two lakes at different fills) was fixed earlier by
  `settle_lakes` lowering the bed, not the surface. Left as is, with the
  per-corner `h_fill` already in `tri_lvl` where a facet spans a lake edge.
- **Refine every wet facet longer than 1.5 cells.** The waterline is already exact
  where the bed crosses (P1's polygon clip); refinement would only smooth the
  *plane*, which is flat by definition. Cost would be large: 1.5 cells is 22
  units, i.e. nearly every facet.

Still open: the sea and the lake take different bounce light. Measured: giving
the sea the sky bounce (`v_bounce` with no bake behind it) moves **0.000%** of the
sea, river-mouth and golden views, so the seam is not the bounce and the code was
reverted rather than kept as a no-op.

Still owed here: **a fall is one stretched strip along its reach**, not a tilted
plane per facet. The plates at the foot of a fall are the remaining size of this
family: each facet hangs at its own corner levels, so neighbours overlap and
point in different directions. The strip needs the reach traced end to end (the
segment list in `build_hydrology` already has it) and the bed/plane pair emitted
once per reach instead of once per facet. Also still open: the sea and the lake
water take different bounce light, which is visible as a seam where a river mouth
meets the sea.


- Fill the band between the border ring and the interior with an inset ring of
  points (tried once: it changes the fill, and it removes the structural
  exemption below, but it did not move the sampled artifact by itself -- so it
  lands *with* the counter that proves it).
- Let the refinement pass split a border-adjacent long triangle by inserting an
  **interior** point, never a point on the shared chain (the chain must stay
  identical in both chunks).
- Assert `edge_long == 0` (longest edge <= 2 x the tier's spacing) and
  `tri_tiny == 0` (area >= 4 units^2) and `unused_pts == 0`.

Cost: some more points near borders; `build=` time is measured by the stats line
and must stay under ~1.2 s async.

### P3 — water is one continuous surface (2 sessions)

- **A level field, not a per-facet maximum.** Today a facet takes the highest
  `h_fill` of its corners; two neighbouring facets can therefore disagree at a
  shared edge. Interpolate the level from the cell field per corner instead, and
  assert level continuity across every shared edge (<= 0.01).
- **Refine what is wet.** Any wet facet longer than ~1.5 cells is subdivided --
  bed and plane together, through the same polygon -- so the waterline gains
  resolution without the bed/plane fight that made the earlier "finer water"
  idea unsafe. This is the good half of that deferred item.
- **Falls are their own geometry**: one stretched strip along the channel's steep
  reach instead of a tilted per-facet plane. The dark wedge at the foot of a
  cascade is a fall plane meeting a lake; a strip that ends above the water ends
  it.
- **The sea plane takes a sky bounce** like every other water surface (it is two
  triangles with no bake, which is why it is darker than the lake beside it), and
  stays a single quad for cost.

### P4 — LOD seams: the stitch ring is not the seam — **rejected by measurement**

- **Stitch ring**: every chunk's border ring is generated at step 1 whatever the
  tier. ~200 extra points on a step-4 chunk (7% more), and the coarse chunk's
  boundary then matches its neighbour *exactly* -- no gap to skirt and, the point
  of it, no step to see.
Built and measured exactly as written: every chunk's border ring at step 1
whatever the tier. Seed 1 alpine, 800x450: points 200402 -> 336627 (**+68%**),
async build 894 -> 1511 ms (**+68%**), and the tier-swap difference (tiered
versus every chunk at its finest) went 7.44% -> 7.09%. The seam the plan was
aiming at is not in the border chain: a step-4 chunk is a *different
triangulation*, so matching the boundary leaves the interior as different as it
was. Reverted, and `lodmax`'s gate raised to the new high-camera reading.

What actually covers a tier boundary is the skirt (a 40-unit wall from every
boundary edge of a coarse chunk, hung from the water surface where the facet is
wet), which is why no gap has ever been reported -- only the *look* difference,
which the `lod` column reports as a settled 7-8% and is not something a stitch can
remove. `seamgap` stays as the geometric alarm (135 worst chunk today; the
mechanism it counts is why the skirt is needed at all).
- Geomorphing stays off the list: `sprint` reads 18.97% with tiers and 18.98%
  with `NOLOD=1`, so a tier swap contributes nothing while moving. If `fly` (P0)
  says otherwise afterwards, that measurement is the reason to revisit.

### P5 — light at pixel resolution — **measured, not taken**

- **Horizon at 2 cells.** The bake goes 944 -> 1236 ms on the builder thread for
  no pixel that was measurable at the view the work started from (a pale line on a
  cliff that turned out to be a fall, not the shadow). Half the build budget for a
  band nobody has reported since the field moved off the mesh. Left at 4 cells.
- **Facet shade +-18% -> +-12%.** Measured: `walk` 2.66 -> 2.62, `pop` 2.42 ->
  2.34, `fly` 16.10 -> 15.87, high `fly` 26.76 -> 25.16. One to six percent for a
  visibly flatter ground, and nowhere near the plan's targets (walk <= 2, fly <=
  8), which says the per-facet shade is no longer the dominant term: P2's
  triangulation was. Reverted; the shader keeps its character.

### P6 — the last shading seams — **measured, nothing there**

- **Sea bounce.** A water surface with no bake behind it (the sea plane, a fall)
  took no bounce while a lake beside it did, so `v_bounce` now falls back to the
  sky ambient. Measured: 0.000% of the sea, river-mouth and golden views -- the
  term is far below a visible step. Reverted rather than kept as a no-op.
- **Specular fade with distance.** Same story: two extra multiplies on a 90th-power
  highlight, 0.000% of the three canonical views. Reverted.
- **Fog where decor meets terrain.** The decor buffer (sea, clouds, birds) runs
  through the same fragment shader and the same `v_depth` fog, so there is one
  path, not two. Nothing to join.

The lesson recorded rather than the work: three of these were written from a code
reading ("same thing, two code paths"), and all three measured at zero. The
picture and the numbers agreed, which is why P0's gates came first.

### P7 — make it a gate, not a habit — **done, as tighter gates rather than a new script**

No `tools/solid.sh` was written: `tools/check.sh --slow` already runs every gate
(build, unit tests, replays, artifacts, the three goldens, the analyzer, the seed
sweep, the flicker probe, `history --check`), and a second wrapper would be a
second definition of done. What changed instead is that the flicker gates are now
**today's measured numbers plus headroom** rather than the loose alarms they were
(P0 could only set alarms because nothing had been fixed yet). Worst of six views
on the writing machine, and the gate:

| column | today | gate |
|---|---|---|
| `static` | 0.00 | 0.5 |
| `walk` | 3.8 | 6.0 |
| `rebuild` | 0.03 | 1.0 |
| `cull` | 0.00 | 0.0 |
| `lod` | 7.8 | 12.0 |
| `lodmax` | 223 | 300 |
| `sprint` | 16.6 | (reported) |
| `smax` | 42.1 | 55.0 |
| `pop` | 3.7 | 6.0 |
| `pmax` | 3.8 | 7.0 |
| `fly` | 31.3 | 45.0 |
| `p99` | 11.5 ms | 25 ms |
| `max` | 15.8 ms | 60 ms |

Old gates for comparison: walk 8, pop 12, pmax 20, rebuild 8, p99 40, max 120.
`sprint`/`smax` keep their 55% alarm because seed 40's camera walks into a coast
and that is a property of the camera, not the renderer. Every number in the table
is worst-of-six (seeds 1, 12, 40; ground and high) and printed by `tools/flicker.sh`,
so a regression moves one of them and the script says which.

### Where this landed — the numbers, before and after

Every target the plan set, against what the gates read now (seed 1, 800x450,
ssaa 2; `lod`/`fly` from the high camera):

| | before P0 | target | now |
|---|---|---|---|
| `yaw` / `pitch` | 4.88 / 5.71 | 0.5 / 0.5 | 0.76 / 1.14 |
| `walk` | 6.64 | 2.0 | 3.81 |
| `pop` / `pmax` | 5.72 / 7.94 | 2.0 | 3.54 / 3.64 |
| `fly` | 19.82 | 8.0 | 16.73 |
| `edgelong` | 266 | 0 | 2 |
| `tritiny` | 441 | 0 | 88 (width rule) |
| `unusedpts` | 18 | 0 | 5 |
| `seamgap` | 453 | 0 | 135 |
| `p99` | 10.3 ms | 16.6 ms | 10.3 ms |

Three targets are not met and the plan is wrong about them, not the renderer:

- `walk` 3.81 against a target of 2.0 -- and 2.66 at the same camera before P2's
  border jitter moved the shoreline, so part of the difference is the *camera*
  that the auto-framing picked, not stability. The remaining term is the ground
  itself moving between frames (contrast-weighted pixel counts punish sharp
  detail), which no renderer change removes; ±12% facet shade bought 1.5%.
- `pop` 3.54 against 2.0: the same, measured with the TAA history cleared every
  frame, i.e. deliberately the un-smoothed figure.
- `fly` 16.7 against 8.0: a 8-degree pan at ~2 px/frame moves every pixel, and the
  column is a worst-frame alarm. The measured breakdown (`walk` 3.8 on the ground,
  0.95 from altitude) says the residual is terrain detail crossing pixels, not
  chunks arriving -- during the pan the streamer reported 2500 chunks ready,
  0 pending, and a constant draw count.

So the honest statement of "solid" is the gates, not the table: every number is
printed, bounded, and a regression fails `check.sh --slow`.

### Deliberately not planned

- MSAA: the temporal pass has to sample depth, so a multisampled target cannot be
  the scene.
- Alpha-blended foliage: needs cards, sorting and a blending policy for the whole
  pass; TAA already softens the hard edges, and the "black specks" that motivated
  it were terrain.
- Motion vectors and a render graph: this renderer is one scene pass plus TAA,
  and the seams that hurt are geometry, not reprojection.
- Geomorphing: see P4 -- measured as invisible while moving.

## Rules that must not be broken by accident

1. Anything indexed by world cell is a bug (see #4).
2. Bots emit state; only the warm tier touches agents.
3. The tick is deterministic and separable from rendering; replay/asserts apply.
4. Player consequence goes through the event queue; local physics does not.
5. Dehydration must be invisible: the numbers persist, the representation thins.
