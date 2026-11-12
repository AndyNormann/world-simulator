# Working on this repo (notes for agents and for future me)

A single-file sokol/Metal terrain viewer. `src/main.c` is the game; `src/terrain.h`
and `src/math3d.h` are the deterministic world; `src/shaders.glsl` is the shader
source and `src/shaders.h` is generated from it.

## The four commands

```sh
tools/render.sh shot --seed 7 --cam 0,180,0 --yaw 45 --time 17.5 -o build/shots/a.png
tools/render.sh replay replays/smoke.txt
tools/test.sh          # math_test + terrain_test + replays/smoke.txt, no GPU
tools/shaders.sh       # regenerate src/shaders.h — human-triggered only
```

`shot` and `replay` never open a window: they create a Metal device, render into
an offscreen 4x-MSAA texture, read it back, and write a PNG. `dump` does not even
create a device — it runs the sim and the asserts.

Every shot also writes `<name>.small.png` (900px wide) and `<name>.hm.png`
(the heightfield, which is often the thing under test).

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

Assertable fields: `cam_x cam_y cam_z yaw pitch hour trees shadowed_pct tris
chunks_drawn`. `tris` and `chunks_drawn` need rendering, so they are skipped in
`dump` mode. `#` starts a comment.

A failing assert prints `file:line: field is X, expected OP Y (t=Zs)` and exits 1.

## Build

`./build.sh` (game), `./build.sh test` (unit tests, ASan+UBSan),
`./build.sh debug` (game with sanitizers). macOS only, Metal only — the GL,
D3D11 and X11 paths were deleted, not disabled.

## Vendored

See `libs/VENDORED.md`. sokol and sokol-shdc are pinned together; bumping one
alone can silently produce a header that does not match the headers.
