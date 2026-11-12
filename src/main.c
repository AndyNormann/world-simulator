#include "game.h"

void sim_step(float step) {
    yaw += (tyaw - yaw) * (1.0f - expf(-SMOOTH_K * step));
    pitch += (tpitch - pitch) * (1.0f - expf(-SMOOTH_K * step));

    if (move_dur > 0.0f) {
        move_t += step;
        float k = move_t / move_dur;
        if (k > 1.0f) k = 1.0f;
        cam_x = move_x0 + (move_x1 - move_x0) * k;
        cam_y = move_y0 + (move_y1 - move_y0) * k;
        cam_z = move_z0 + (move_z1 - move_z0) * k;
        if (k >= 1.0f) { move_dur = 0.0f; move_t = 0.0f; }
    }

    sim_frames++;
    anim_time = (float)sim_frames / 60.0f;
    /* one in-game hour per real minute */
    if (!time_fixed) {
        day_hours += step * (24.0f / 1440.0f);
        if (day_hours >= 24.0f) day_hours -= 24.0f;
    }

    float fx = sinf(yaw) * cosf(pitch);
    float fy = sinf(pitch);
    float fz = cosf(yaw) * cosf(pitch);
    /* screen-right is cross(fwd, up) = (-cos yaw, 0, sin yaw) */
    float rx = -cosf(yaw), rz = sinf(yaw);
    float spd = (keys[SAPP_KEYCODE_LEFT_SHIFT] ? 40.0f : 12.0f) * step;
    if (keys[SAPP_KEYCODE_W]) { cam_x += fx*spd; cam_y += fy*spd; cam_z += fz*spd; }
    if (keys[SAPP_KEYCODE_S]) { cam_x -= fx*spd; cam_y -= fy*spd; cam_z -= fz*spd; }
    if (keys[SAPP_KEYCODE_A]) { cam_x -= rx*spd; cam_z -= rz*spd; }
    if (keys[SAPP_KEYCODE_D]) { cam_x += rx*spd; cam_z += rz*spd; }
    if (getenv("FLY")) { cam_x += fx * 6.0f; cam_z += fz * 6.0f; }
    if (keys[SAPP_KEYCODE_Q]) { cam_y -= spd; }
    if (keys[SAPP_KEYCODE_E]) { cam_y += spd; }

    float ground = terrain_height(cam_x, cam_z, terrain_seed) + 2.0f;
    if (cam_y < ground) cam_y = ground;
}

/* Everything that touches the GPU. Takes the size explicitly so the same code
   serves a window and an offscreen texture. */

void frame(void) {
    static uint64_t last = 0;
    uint64_t now = stm_now();
    float elapsed = last ? (float)stm_sec(now - last) : (1.0f / 60.0f);
    last = now;
    if (elapsed > 0.1f) elapsed = 0.1f;
    int steps = (int)(elapsed * 60.0f + 0.5f);
    if (steps < 1) steps = 1;
    if (steps > MAX_CATCHUP) steps = MAX_CATCHUP;
    for (int i = 0; i < steps; i++) sim_step(1.0f / 60.0f);
    render(sapp_width(), sapp_height());
}

void event(const sapp_event *ev) {
    switch (ev->type) {
    case SAPP_EVENTTYPE_KEY_DOWN:
        if (ev->key_code == SAPP_KEYCODE_ESCAPE) sapp_request_quit();
        if (ev->key_code < 512) keys[ev->key_code] = 1;
        break;
    case SAPP_EVENTTYPE_KEY_UP:
        if (ev->key_code < 512) keys[ev->key_code] = 0;
        break;
    case SAPP_EVENTTYPE_FOCUSED:
        sapp_lock_mouse(true);    /* only now is it safe to take the pointer */
        got_first_mouse = 0;
        break;
    case SAPP_EVENTTYPE_UNFOCUSED:
        sapp_lock_mouse(false);
        memset(keys, 0, sizeof(keys));   /* key held during unfocus would stick */
        break;
    case SAPP_EVENTTYPE_MOUSE_MOVE: {
        /* ponytail: locking the cursor emits one huge bogus delta as macOS
           warps the pointer to the centre. Drop the first event and clamp
           the rest, or the camera snaps/spins on its own. */
        if (!got_first_mouse) { got_first_mouse = 1; break; }
        float dx = ev->mouse_dx, dy = ev->mouse_dy;
        if (dx >  200.0f) dx =  200.0f;
        if (dx < -200.0f) dx = -200.0f;
        if (dy >  200.0f) dy =  200.0f;
        if (dy < -200.0f) dy = -200.0f;
        tyaw -= dx * 0.0025f;   /* mouse right = turn right */
        tpitch -= dy * 0.0025f;
        if (tpitch >  1.35f) tpitch =  1.35f;
        if (tpitch < -1.35f) tpitch = -1.35f;
        break;
    }
    default: break;
    }
}


void cleanup(void) {
    shutdown_gfx();
}

/* --- capture ---------------------------------------------------------------
   One frame out to a PNG plus a sidecar of numbers, blocking until the
   streamer has caught the camera up. `dt` is never advanced while waiting:
   otherwise a busy machine would render a different camera than an idle one
   and the same replay would produce two different images. */
/* Wall clock, not a frame count: a sanitizer build or a loaded machine can need
   many more pump frames for the same world, and a frame budget would then fail
   on exactly the builds that are most likely to be slow. */





int main(int argc, char *argv[]) {
    const char *cmd = (argc > 1 && argv[1][0] != '-') ? argv[1] : NULL;
    int first = cmd ? 2 : 1;              /* where the flags start */
    int dump_only = cmd && !strcmp(cmd, "dump");
    int years = 300, sim_check = 0;

    /* flag pass shared by every headless mode, plus the seed pre-scan for
       replays (the world is built before the script runs) */
    const char *replay_path = NULL;
    for (int i = first; i < argc; i++) {
        if (!strcmp(argv[i], "--seed") && i + 1 < argc) terrain_seed = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--time") && i + 1 < argc) { day_hours = (float)atof(argv[++i]); time_fixed = 1; }
        else if (!strcmp(argv[i], "--yaw") && i + 1 < argc) { yaw = tyaw = (float)atof(argv[++i]) * 3.14159265f / 180.0f; have_view = 1; }
        else if (!strcmp(argv[i], "--pitch") && i + 1 < argc) { pitch = tpitch = (float)atof(argv[++i]) * 3.14159265f / 180.0f; have_view = 1; }
        else if (!strcmp(argv[i], "--cam") && i + 1 < argc) {
            const char *v = argv[++i];
            have_view = 1;    /* --cam must win over the auto-aim, or it is a no-op */
            cam_x = (float)atof(v);
            const char *c1 = strchr(v, ',');
            if (c1) { cam_y = (float)atof(c1 + 1);
                const char *c2 = strchr(c1 + 1, ',');
                if (c2) cam_z = (float)atof(c2 + 1); }
        }
        else if (!strcmp(argv[i], "--trees-only")) trees_only = 1;
        else if ((!strcmp(argv[i], "--out") || !strcmp(argv[i], "-o")) && i + 1 < argc)
            shot_path = argv[++i];
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &gfx_w, &gfx_h) != 2) {
                fprintf(stderr, "--size: expected WxH\n"); return 1;
            }
        }
        else if (!strcmp(argv[i], "--years") && i + 1 < argc) years = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--check")) sim_check = 1;
        else if (!strcmp(argv[i], "--replay") && i + 1 < argc) replay_path = argv[++i];
    }
    if (cmd && !strcmp(cmd, "replay")) replay_path = (argc > 2) ? argv[2] : NULL;
    if (cmd && !strcmp(cmd, "dump")) replay_path = (argc > 2) ? argv[2] : NULL;

    if (!cmd) {
        if (getenv("HEADLESS")) {            /* old escape hatch, kept honest */
            fprintf(stderr, "HEADLESS is gone: use `game shot` / `game replay`\n");
            return 1;
        }
        sapp_desc d = game_desc(argc, argv);
        sapp_run(&d);
        return 0;
    }
    if (!strcmp(cmd, "worldmap")) {
        headless = 1;
        worldmap(shot_path[0] ? shot_path : "build/shots/world.png",
                 gfx_w != 1280 || gfx_h != 720 ? (gfx_w < gfx_h ? gfx_w : gfx_h) : 512);
        return 0;
    }
    if (!strcmp(cmd, "history")) {
        /* no GPU and no window: the sim is data, and data has to be readable
           from a terminal or it is not reviewable */
        headless = 1;
        build_world_data(terrain_seed);
        sim_history(terrain_seed, years, sim_check);
        return 0;
    }
    if (strcmp(cmd, "shot") && strcmp(cmd, "replay") && strcmp(cmd, "dump")) {
        fprintf(stderr, "usage: game [shot|replay FILE|dump FILE|worldmap|history] [flags]\n"
                        "  flags: --seed N --cam x,y,z --yaw DEG --pitch DEG\n"
                        "         --time HOURS --size WxH --out FILE --trees-only\n"
                        "         --years N --check   (history)\n");
        return 1;
    }
    if ((!strcmp(cmd, "replay") || !strcmp(cmd, "dump")) && !replay_path) {
        fprintf(stderr, "usage: game %s FILE\n", cmd);
        return 1;
    }

    headless = 1;
    init();
    if (!strcmp(cmd, "shot")) {
        capture(shot_path);
    } else {
        /* `seed` in the script has to land before init() builds the world, so
           read the first command before doing anything else */
        if (!replay_path) { fprintf(stderr, "replay: no file given\n"); return 1; }
        FILE *f = fopen(replay_path, "r");
        if (f) {
            char line[512];
            while (fgets(line, sizeof line, f)) {
                if (line[0] == '#' || line[0] == '\n') continue;
                unsigned s;
                if (sscanf(line, "seed %u", &s) == 1) {
                    /* too late: init() already built the world at terrain_seed */
                    fprintf(stderr, "replay: `seed` must be given as --seed %u\n", s);
                    fprintf(stderr, "        (the world is built before the script runs)\n");
                    fclose(f);
                    return 1;
                }
                break;
            }
            fclose(f);
        }
        render_ok = !dump_only;
        replay_run(replay_path, render_ok);
    }
    shutdown_gfx();
    return 0;
}

sapp_desc game_desc(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc)
            terrain_seed = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--time") == 0 && i + 1 < argc) {
            day_hours = (float)atof(argv[++i]);
            time_fixed = 1;
        }
        else if (strcmp(argv[i], "--yaw") == 0 && i + 1 < argc) {
            yaw = tyaw = (float)atof(argv[++i]) * 3.14159265f / 180.0f;
            have_view = 1;
        }
        else if (strcmp(argv[i], "--pitch") == 0 && i + 1 < argc) {
            pitch = tpitch = (float)atof(argv[++i]) * 3.14159265f / 180.0f;
            have_view = 1;
        }
        else if (strcmp(argv[i], "--trees-only") == 0) trees_only = 1;
    }
    return (sapp_desc){ .init_cb=init, .frame_cb=frame, .cleanup_cb=cleanup, .event_cb=event,
        .width=1280, .height=720, .window_title="terrain", .high_dpi=true,
        .sample_count=GFX_MSAA };
}
