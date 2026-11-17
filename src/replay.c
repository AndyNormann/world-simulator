#include "game.h"

void replay_die(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s:%d: ", replay_name, replay_line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

void advance(float seconds) {
    int n = (int)lroundf(seconds * 60.0f);
    if (n < 0) n = 0;
    for (int i = 0; i < n; i++) sim_step(1.0f / 60.0f);
    replay_now += (double)n / 60.0;
}

void key_by_name(const char *name, int down) {
    int code = -1;
    if (!strcmp(name, "W")) code = SAPP_KEYCODE_W;
    else if (!strcmp(name, "A")) code = SAPP_KEYCODE_A;
    else if (!strcmp(name, "S")) code = SAPP_KEYCODE_S;
    else if (!strcmp(name, "D")) code = SAPP_KEYCODE_D;
    else if (!strcmp(name, "Q")) code = SAPP_KEYCODE_Q;
    else if (!strcmp(name, "E")) code = SAPP_KEYCODE_E;
    else if (!strcmp(name, "SHIFT")) code = SAPP_KEYCODE_LEFT_SHIFT;
    else replay_die("unknown key '%s'", name);
    keys[code] = down;
}

/* Render-derived counters cannot be asserted in `dump` mode, which never calls
   render(): they would be zero and every script would need two variants. */
int key_needs_render(const char *field) {
    return !strcmp(field, "tris") || !strcmp(field, "chunks_drawn");
}

float key_state(const char *field) {
    if (!strcmp(field, "cam_x")) return cam_x;
    if (!strcmp(field, "cam_y")) return cam_y;
    if (!strcmp(field, "cam_z")) return cam_z;
    if (!strcmp(field, "yaw")) return yaw;
    if (!strcmp(field, "pitch")) return pitch;
    if (!strcmp(field, "hour")) return day_hours;
    if (!strcmp(field, "chunks_drawn")) return (float)chunks_drawn;
    if (!strcmp(field, "trees")) return (float)tree_count;
    if (!strcmp(field, "shadowed_pct")) return (float)shadowed_pct();
    if (!strcmp(field, "tris")) return (float)mesh_tris;
    if (!strcmp(field, "rivers")) return (float)river_count;
    if (!strcmp(field, "land")) return land_pct;
    if (!strcmp(field, "masses")) return (float)land_masses;
    if (!strcmp(field, "townmasses")) return (float)land_town_masses;
    if (!strcmp(field, "viable")) return (float)land_viable;
    if (!strcmp(field, "relief")) return world_relief;
    /* structural health: geometry dropped for lack of budget, or a field that
       generated NaN. Both are silent in the frame and must never be non-zero. */
    if (!strcmp(field, "overflow")) return (float)mesh_overflow;
    if (!strcmp(field, "nonfinite")) return (float)world_nonfinite;
    if (!strcmp(field, "trioverflow")) return (float)tri_overflow;
    if (!strcmp(field, "ptoverflow")) return (float)pt_overflow;
    /* structural counts, worst chunk: a long facet, a sliver, a point nobody
       used, a seabed colour on dry ground, a seam chord off the ground */
    if (!strcmp(field, "edgelong")) return (float)c_edge_long;
    if (!strcmp(field, "tritiny")) return (float)c_tri_tiny;
    if (!strcmp(field, "unusedpts")) return (float)c_unused_pts;
    if (!strcmp(field, "wetcoldry")) return (float)c_wet_col_dry;
    if (!strcmp(field, "seamgap")) return (float)c_seam_gap;
    /* how badly a facet lies about the ground under it, and how close a chunk
       came to the vertex budget: both print today and neither was gated */
    if (!strcmp(field, "grounderr")) return c_ground_err;
    if (!strcmp(field, "groundfar")) return c_ground_err_far;
    if (!strcmp(field, "chunkverts")) return (float)c_chunk_verts;
    if (!strcmp(field, "relit")) return (float)c_relit;
    /* sokol warnings. The listed benign one is filtered in game_logger; this
       must be zero, so a validation warning fails the cheap gate. */
    if (!strcmp(field, "warned")) return (float)gfx_warn;
    if (!strcmp(field, "points")) return (float)mesh_pts;
    /* async build, the number that says whether a world got more expensive */
    if (!strcmp(field, "buildms")) return build_async_ms;
    /* the whole heightfield, so a seed sweep can catch a world that generated
       flat, empty or nonsensical without rendering anything */
    if (!strcmp(field, "height_min") || !strcmp(field, "height_max")) {
        float lo = 1e9f, hi = -1e9f;
        for (int i = 0; i < N * N; i++) {
            if (heights[i] < lo) lo = heights[i];
            if (heights[i] > hi) hi = heights[i];
        }
        return strcmp(field, "height_min") == 0 ? lo : hi;
    }
    replay_die("assert: unknown field '%s'", field);
    return 0.0f;
}

/* `assert field OP value` with OP one of > < >= <= == != */
void replay_assert(const char *rest) {
    char field[64], op[4];
    double want;
    if (sscanf(rest, "%63s %3s %lf", field, op, &want) != 3)
        replay_die("assert: expected `assert <field> <op> <value>`");
    float got = key_state(field);
    if (!render_ok && key_needs_render(field)) {
        printf("%s:%d: skip assert %s (no rendering in this mode)\n",
               replay_name, replay_line, field);
        return;
    }
    int ok = 0;
    if (!strcmp(op, ">"))  ok = got >  want;
    else if (!strcmp(op, "<"))  ok = got <  want;
    else if (!strcmp(op, ">=")) ok = got >= want;
    else if (!strcmp(op, "<=")) ok = got <= want;
    else if (!strcmp(op, "==")) ok = fabsf(got - (float)want) < 1e-4f;
    else if (!strcmp(op, "!=")) ok = fabsf(got - (float)want) >= 1e-4f;
    else replay_die("assert: unknown operator '%s'", op);
    if (!ok) replay_die("%s is %.2f, expected %s %g (t=%.2fs)", field, got, op, want, replay_now);
}

/* `seed N` only makes sense before anything has been simulated or rendered,
   and the world is built in init(); so the value is pre-scanned by the caller
   and a seed line anywhere else is an error. */
void replay_run(const char *path, int render_enabled) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "replay: cannot open %s\n", path); exit(1); }
    replay_name = path;
    char line[512];
    int executed = 0;
    while (fgets(line, sizeof line, f)) {
        replay_line++;
        char *hash = strchr(line, '#');      /* comments, including trailing */
        if (hash) *hash = 0;
        char *cmd = strtok(line, " \t\r\n");
        if (!cmd) continue;
        char *rest = strtok(NULL, "\r\n");
        if (!rest) rest = (char*)"";
        while (*rest == ' ' || *rest == '\t') rest++;

        if (!strcmp(cmd, "seed")) {
            if (executed) replay_die("seed must be the first command");
            continue;                        /* applied before init(), see main */
        }
        executed++;
        if (!strcmp(cmd, "wait")) {
            advance((float)atof(rest));
        } else if (!strcmp(cmd, "time")) {
            day_hours = (float)atof(rest);
            time_fixed = 1;
        } else if (!strcmp(cmd, "cam")) {
            if (sscanf(rest, "%f,%f,%f", &cam_x, &cam_y, &cam_z) != 3)
                replay_die("cam: expected x,y,z");
            move_dur = 0.0f;
        } else if (!strcmp(cmd, "look")) {
            float y = 0, p = 0;
            if (sscanf(rest, "%f %f", &y, &p) != 2) replay_die("look: expected yaw pitch");
            yaw = tyaw = y * 3.14159265f / 180.0f;
            pitch = tpitch = p * 3.14159265f / 180.0f;
            have_view = 1;
        } else if (!strcmp(cmd, "move")) {
            char over[32] = {0};
            float dur = 0.0f;
            int n = sscanf(rest, "%f,%f,%f over %fs", &move_x1, &move_y1, &move_z1, &dur);
            if (n == 4) {
                move_x0 = cam_x; move_y0 = cam_y; move_z0 = cam_z;
                move_dur = dur > 0.0f ? dur : 0.0f;
                move_t = 0.0f;
            } else if (n == 3) {
                cam_x = move_x1; cam_y = move_y1; cam_z = move_z1;
                move_dur = 0.0f;
            } else {
                replay_die("move: expected x,y,z [over seconds]");
            }
            (void)over;
        } else if (!strcmp(cmd, "key")) {
            char name[32], action[32] = {0};
            float secs = 0.0f;
            int n = sscanf(rest, "%31s %31s %f", name, action, &secs);
            if (n < 2) replay_die("key: expected KEY down|up|hold [seconds]");
            if (!strcmp(action, "down")) key_by_name(name, 1);
            else if (!strcmp(action, "up")) key_by_name(name, 0);
            else if (!strcmp(action, "hold")) {
                if (n < 3) replay_die("key: `hold` needs a duration");
                key_by_name(name, 1);
                advance(secs);
                key_by_name(name, 0);
            } else replay_die("key: unknown action '%s'", action);
        } else if (!strcmp(cmd, "shot")) {
            if (!*rest) replay_die("shot: expected a path");
            if (!render_enabled) continue;       /* `dump` mode */

            capture(rest);
        } else if (!strcmp(cmd, "assert")) {
            replay_assert(rest);
        } else {
            replay_die("unknown command '%s'", cmd);
        }
    }
    fclose(f);
    printf("replay ok: %s (%d commands, t=%.2fs)\n", path, executed, replay_now);
}

/* --- entry point -----------------------------------------------------------
   No sokol_app main() (SOKOL_NO_ENTRY): the window is one mode among four.
   `shot` and `replay` never create a window or a drawable, so they can run with
   nothing on screen; `dump` does not even create a Metal device. */
sapp_desc game_desc(int argc, char *argv[]);
