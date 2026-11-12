# Working on this repo (notes for agents and for future me)

A single-file sokol/Metal terrain viewer. `src/main.c` is the game; `src/terrain.h`
and `src/math3d.h` are the deterministic world; `src/shaders.glsl` is the shader
source and `src/shaders.h` is generated from it.

## The commands

```sh
tools/check.sh         # definition of done: build + tests + replays + goldens (~7s)
tools/check.sh --quick # build + unit tests + smoke replay (~3s)
tools/render.sh shot --seed 7 --cam 0,180,0 --yaw 45 --time 17.5 -o build/shots/a.png
tools/render.sh replay replays/smoke.txt
tools/test.sh          # math_test + terrain_test + replays/smoke.txt, no GPU
tools/diff.sh A.png B.png [--tol 0.3] [--heat out.png]
tools/goldens.sh       # re-record tests/golden/ — only when the change is intended
tools/shaders.sh       # regenerate src/shaders.h — human-triggered only
```

Build is a `Makefile`; `./build.sh [game|test|debug]` wraps it. `src/sokol_impl.c`
is a separate TU on purpose: a game edit rebuilds in ~1s, sokol only when
`libs/sokol` changes. `tools/compile_commands.sh` writes `compile_commands.json`
(gitignored) for clangd.

`shot` and `replay` never open a window: they create a Metal device, render into
an offscreen 4x-MSAA texture, read it back, and write a PNG. `dump` does not even
create a device — it runs the sim and the asserts.

Every shot also writes `<name>.small.png` (900px wide) and `<name>.hm.png`
(the heightfield, which is often the thing under test).

## Testing a change (this is the workflow — follow it)

You cannot see a window. Do not try to open one, and do not assume a change is
fine because it compiles. Use the headless renderer as your eyes.

**1. Cheap gate first.** After any edit to `src/`:

```sh
./build.sh            # must stay warning-clean (-Werror)
tools/test.sh         # math/terrain unit tests + the smoke replay, no GPU
```

That catches compile errors, broken terrain maths, and camera/streaming
regressions in a few seconds.

**2. Look at the thing you changed.** Render the view where the change is
visible, then read the downscaled image (never the full-size one):

```sh
tools/render.sh shot --seed 7 --cam 0,180,0 --yaw 45 --time 17.5 -o build/shots/check.png
```

Then read `build/shots/check.png.small.png` and compare it with
`build/shots/check.png.hm.png` (the heightfield) and the metrics printed on
stdout. The numbers are often the faster read: `height=`, `shadowed=`, `trees=`,
`chunks: … ready, N drawn`, `mesh:`.

**3. Pin the result with asserts, not with a picture.** If the change is a
behaviour, add or extend a replay so it fails loudly later:

```
wait 0.5
shot build/shots/x.png
assert trees > 1000
assert shadowed_pct < 60
```

Prefer a `shot` followed by asserts over asserts alone: chunk and mesh counters
only exist once something has rendered, and `dump` mode skips those asserts.

**4. Compare against before.** Two runs with the same `--seed/--cam/--yaw/--time`
produce a byte-identical PNG, so a refactor that was supposed to change nothing
is checkable:

```sh
tools/diff.sh before.png after.png          # exits 1 above 0.3% mean difference
tools/diff.sh tests/golden/dusk.png build/shots/golden-dusk.png
```

Three canonical views are recorded in `tests/golden/` and verified by
`tools/check.sh`. If your change is *meant* to move pixels, re-record with
`tools/goldens.sh` and say why in the commit message; if it is not, a diff is a
bug.

**Useful flags**

| Flag | Why |
|---|---|
| `--seed N` | fixed world; `1` and `7` are the ones the smoke tests use |
| `--cam x,y,z` | deterministic viewpoint; also cancels the auto-aim |
| `--yaw/--pitch DEG` | deterministic look direction |
| `--time H` | freeze the day clock (17.5 = late afternoon, 21 = night) |
| `--size WxH` | small renders are faster and cheaper to read |
| `--trees-only` | flat reference-grey ground, for tree silhouette work |
| `--out|-o FILE` | where the PNG goes; the `.small.png`/`.hm.png` follow it |

**Gotchas that will bite you**

- The world is built before a replay runs, so `seed` cannot be a replay command.
  Pass `--seed N`.
- `assert` on render-derived fields (`tris`, `chunks_drawn`) only after a `shot`.
- Camera auto-aims at the peak unless `--cam`, `--yaw`/`--pitch`, or a replay
  `cam`/`look` is given. `--cam` alone is enough to keep the auto-aim off.
- `tools/render.sh` always rebuilds first (`make` is cached, so this is ~0.1s
  when nothing changed). Running `./build/game` directly can render a stale
  binary — the exact failure mode background agents hit.
- A sokol `PANIC` (validation failure) now exits 1 instead of continuing. If you
  see one, the frame was never written.
- Output dirs are not created by the game; `build/shots/` exists because
  `build.sh` makes it.

## Rules that exist for a reason

- **Never hand-edit `src/shaders.h`.** Edit `src/shaders.glsl`, run
  `tools/shaders.sh`, commit both. That step precompiles the Metal bytecode, so
  a shader error is a generation-time error, not a black frame.
- **No `sapp_*` in sim code.** `sim_step()` must not touch the GPU or the clock;
  that is what makes a replay reproducible. Rendering lives in `render(w, h)`.
- **`shot` never advances the sim.** If the streamer has not caught up it pumps
  upload frames with the clock stopped. Without this, a busy machine renders a
  different camera than an idle one and the same replay produces two images.
- **Fixed 60 Hz.** `frame()` accumulates real time and runs N × `sim_step(1/60)`
  (N ≤ 5). A replay is therefore the same motion you see when playing.
- Uniform parameters are one contiguous blob; the order in `build_palette()` must
  match the std140 order in `src/shaders.glsl`. Getting this wrong is a black or
  garbage frame, not a compile error.

## Replay format

```
seed N                       must be the first line, or pass --seed
time 17.5                    freeze the clock at that hour
cam x,y,z                    teleport
look YAW PITCH               degrees, instant
move x,y,z [over S]          teleport, or smoothed over S seconds
key W down|up|hold S         W A S D Q E SHIFT
wait S                       advance the sim, no render
shot PATH                    render one frame (blocks until the world is ready)
assert FIELD OP VALUE        OP: > < >= <= == !=
```

Assertable fields: `cam_x cam_y cam_z yaw pitch hour trees rivers shadowed_pct
tris chunks_drawn`. `tris` and `chunks_drawn` need rendering, so they are skipped in
`dump` mode. `#` starts a comment.

A failing assert prints `file:line: field is X, expected OP Y (t=Zs)` and exits 1.

## Build

`./build.sh` (game), `./build.sh test` (unit tests, ASan+UBSan),
`./build.sh debug` (game with sanitizers, `-O1`: at `-O0` the horizon bake makes
streaming take minutes). macOS only, Metal only — the GL, D3D11 and X11 paths
were deleted, not disabled.

## Vendored

See `libs/VENDORED.md`. sokol and sokol-shdc are pinned together; bumping one
alone can silently produce a header that does not match the headers.
