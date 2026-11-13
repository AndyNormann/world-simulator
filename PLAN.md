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

### 3. Climate — partly done (treeline)
Aspect (south-facing dry and warm, north-facing cold and retaining snow), a real
wind march for the rain shadow instead of the current single upwind probe,
a treeline separate from the snowline, and dithered/steepness-blended biome
transitions instead of hard bands. Riparian green along rivers.
- Accept: biome entropy up without violating latitudinal order; a north and a
  south shot that are distinguishable without the sun.

### 4. Coasts (1 session)
Wave exposure decides the profile: west-facing cliffs and stacks, east-facing
beaches and tidal flats; estuaries where a river meets the sea; a delta where a
slow river does.
- Why: the coast is where a player arrives and where ports and sea trade live.

### 5. Hydrology detail (1 session, needs 1)
Meanders and floodplain widening on low-gradient reaches, lake basins with a
real level and an outflow, waterfall markers where a river crosses a steep step
(they are natural chokepoints and should be visible from the map), endorheic
basins in dry interiors.

### 6. Terrain exports a geography (1 session, needs 1–3)
Name what already exists: basins, passes (with width and height), valley mouths,
harbors, arable area per region. `src/sim.c` should consume *those* instead of
re-probing cells, so the economy is built on geography the player can see and
walk to — and so "cut the pass" is a thing that can be done.

### 7. Scale: coarse field + streamed regions (2–3 sessions, only when it bites)
PLAN rule 4, still unbuilt: ~256² always-resident coarse field plus per-region
64² fine fields generated on demand, `bake_ambient` per region instead of over
the world. Trigger: a world larger than 17600, or needing the memory back.

### 8. Beyond the heightfield (2 sessions, after M2/M3)
Scree, boulders, dead trees and rock outcrops as generated decor — they are also
climb affordances, so this is partly art direction and partly level design.
Real overhangs/arches/caves need geometry with collision; a heightfield cannot
express them, and the demon's climbing makes them valuable.

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

**M2 — one place.** Records → geometry: a generated city, walkable, with roofs
and alleys; `game shot --settlement 3` byte-identical twice.

**M3 — the player.** Procedural character, movement/climb/crouch, one stealth
kill, discovery-driven alert, XP from the event queue.

**M4 — the rest of the fantasy.** Disease tree, zombies, anchors, banishment and
the fast-forward through the same tick loop.

## Rules that must not be broken by accident

1. Anything indexed by world cell is a bug (see #4).
2. Bots emit state; only the warm tier touches agents.
3. The tick is deterministic and separable from rendering; replay/asserts apply.
4. Player consequence goes through the event queue; local physics does not.
5. Dehydration must be invisible: the numbers persist, the representation thins.
