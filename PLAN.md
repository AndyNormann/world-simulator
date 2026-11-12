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

## Milestones

**T — terrain nailed (now).** 8800 first for cheap iteration, then 17600
(`TERRAIN_N` 1184) once it looks right. Deliverables: archetypes, ranges with
passes, rivers that reach the sea, coastline variety, biome colour, and the loop
tools: `tools/worlds.sh` (40 worlds + stats), `tools/mapsheet.py` (one image),
`game worldmap --seed N`. Acceptance: the Q15 invariant list as asserts over a
seed sweep, plus "a person can look at the sheet and tell the archetypes apart".

**M1 — the world exists as data.** Coarse field + region grid + sites/routes
graph; 30 polities; economy ticking; cast records; 500 years headless in ~2 s;
`game history --seed N --years 500` printing a readable chronicle. Acceptance:
deterministic (same seed → same chronicle hash), invariants asserted (no
negative stocks, every polity death has a cause, trade converges), and the
chronicle is *readable*.

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
