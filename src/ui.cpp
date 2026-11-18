/* The debug overlay (Tab). The only C++ in the project, because dear imgui has
   no C build; everything it touches is the C game through game.h.

   Rules this file lives by, in order of how easy they are to break:
   - it draws after the TAA/present pass, straight into the swapchain, and only
     in a window: a headless run must not be able to photograph it;
   - it owns no simulation state. It reads and writes the same globals the
     keyboard writes, so anything it can do, a replay can do too;
   - the reproducer line it shows is the screenshot's contract: whatever state
     the panels produced must be expressible as flags. */
#include "game.h"
#include "imgui.h"
#define SOKOL_IMGUI_IMPL
#include "sokol_imgui.h"

static bool alive = false;
static bool open_ = false;
static int  shot_no = 0;
static unsigned seed_edit = 1;
static int  world_edit = 0;
static char repro[512] = "";
static bool repro_dirty = true;
static bool pending = false;   /* panels built, not yet drawn into the present pass */

/* the state the reproducer line describes, in one place */
static void build_repro(void) {
    snprintf(repro, sizeof repro,
        "./build/game shot --seed %u --world %s --size %dx%d --ssaa %d "
        "--cam %.1f,%.1f,%.1f --yaw %.2f --pitch %.2f --time %.2f --anim %.3f",
        terrain_seed, terrain_gen(terrain_seed).name, gfx_w, gfx_h, gfx_ssaa,
        cam_x, cam_y, cam_z, yaw * 180.0f / 3.14159265f,
        pitch * 180.0f / 3.14159265f, day_hours, anim_time);
    repro_dirty = false;
}

static void look_at_peak(void) {
    float dx = peak_x - cam_x, dz = peak_z - cam_z, dy = peak_y - cam_y;
    yaw = tyaw = atan2f(dx, dz);
    pitch = tpitch = atan2f(dy, sqrtf(dx * dx + dz * dz));
}

static void screenshot(void) {
    char p[256];
    snprintf(p, sizeof p, "build/shots/live-%u-%03d.png", terrain_seed, shot_no++);
    live_shot(p);
}

void ui_setup(void) {
    if (headless) return;                   /* never on a capture path */
    simgui_desc_t d = {};
    d.max_vertices = 64 * 1024;
    /* silent by default, like the rest of sokol: a flush inside a pass or a
       draw outside one would otherwise just draw nothing */
    d.logger.func = [](const char *tag, uint32_t log_level, uint32_t log_item_id,
                       const char *message, uint32_t line, const char *file,
                       void *user_data) {
        (void)tag; (void)user_data;
        fprintf(stderr, "imgui[%u/%u]: %s (%s:%u)\n", log_level, log_item_id,
                message ? message : "", file ? file : "", line);
    };
    d.ini_filename = "build/imgui.ini";   /* window position, not a repo file */
    simgui_setup(&d);
    alive = true;
    seed_edit = terrain_seed;
    world_edit = terrain_force >= 0 ? terrain_force : -1;
    repro_dirty = true;
}

void ui_shutdown(void) {
    if (!alive) return;
    simgui_shutdown();
    alive = false;
    pending = false;
}

int ui_wants_mouse(void) { return open_; }

void ui_toggle(void) {
    if (!alive) return;
    open_ = !open_;
    memset(keys, 0, sizeof(keys));     /* a key held across the toggle would stick */
    sapp_lock_mouse(!open_);
    got_first_mouse = 0;
}

int ui_event(const void *ev) {
    if (!alive || !open_) return 0;
    return simgui_handle_event((const sapp_event *)ev) ? 1 : 0;
}

void ui_frame(int w, int h) {
    if (!alive) { ui_in_frame = 0; return; }
    ui_in_frame = 1;
    simgui_frame_desc_t fd = {};
    fd.width = w;
    fd.height = h;
    fd.delta_time = 1.0 / 60.0;
    fd.dpi_scale = sapp_dpi_scale();
    simgui_new_frame(&fd);
    ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(380, 560), ImGuiCond_FirstUseEver);
    ImGui::Begin("world", NULL);

    /* --- world ------------------------------------------------------- */
    if (ImGui::CollapsingHeader("world", ImGuiTreeNodeFlags_DefaultOpen)) {
        int seed_tmp = (int)seed_edit;
        if (ImGui::InputInt("seed", &seed_tmp, 1, 10)) seed_edit = (unsigned)(seed_tmp < 1 ? 1 : seed_tmp);
        ImGui::SameLine();
        if (ImGui::Button("random")) {
            /* recorded, not remembered: whatever comes up is what the reproducer
               line and the stats line will say */
            seed_edit = (unsigned)(stm_sec(stm_now()) * 1000.0) ^ (unsigned)(sim_frames * 2654435761u);
            if (seed_edit == 0) seed_edit = 1;
        }
        const char *names[WORLDGEN_COUNT + 1] = { "auto (from seed)" };
        for (int i = 0; i < WORLDGEN_COUNT; i++) names[i + 1] = worldgen_table[i].name;
        int combo = world_edit + 1;
        if (ImGui::Combo("archetype", &combo, names, WORLDGEN_COUNT + 1)) world_edit = combo - 1;
        ImGui::Text("now: %s, %d trees", terrain_gen(terrain_seed).name, tree_count);
        ImGui::Text("build %.0f ms, peak %.0f,%.0f,%.0f", build_ms,
                    peak_x, peak_y, peak_z);
        ImGui::Text("%d/%d chunks, %d drawn", nchunks_ready(), NCHUNKS, chunks_drawn);
        ImGui::Text("%d points, %d triangles", mesh_pts, mesh_tris);
        ImGui::TextDisabled("window %dx%d px, dpi %.1f", (int)(w), (int)(h), sapp_dpi_scale());
        /* Requested, not performed: destroying the imgui context inside its own
           frame is a use-after-free (it crashed on this exact button). */
        if (ImGui::Button("rebuild")) request_world_rebuild(seed_edit, world_edit);
        ImGui::SameLine();
        if (ImGui::Button("rebuild same seed")) request_world_rebuild(terrain_seed, terrain_force);
    }

    /* --- camera ------------------------------------------------------ */
    if (ImGui::CollapsingHeader("camera", ImGuiTreeNodeFlags_DefaultOpen)) {
        float p[3] = { cam_x, cam_y, cam_z };
        if (ImGui::DragFloat3("position", p, 2.0f)) { cam_x = p[0]; cam_y = p[1]; cam_z = p[2]; }
        float deg[2] = { yaw * 180.0f / 3.14159265f, pitch * 180.0f / 3.14159265f };
        if (ImGui::DragFloat2("yaw/pitch", deg, 0.2f)) {
            yaw = tyaw = deg[0] * 3.14159265f / 180.0f;
            pitch = tpitch = deg[1] * 3.14159265f / 180.0f;
        }
        float hour = day_hours;
        if (ImGui::SliderFloat("hour", &hour, 0.0f, 24.0f, "%.2f")) {
            day_hours = hour;
            time_fixed = 1;              /* dragging the clock means "keep it" */
        }
        ImGui::DragFloat("walk u/s", &speed_walk, 1.0f, 1.0f, 500.0f);
        ImGui::DragFloat("shift u/s", &speed_shift, 2.0f, 1.0f, 2000.0f);
        if (ImGui::Button("look at peak")) look_at_peak();
        ImGui::SameLine();
        if (ImGui::Button("spawn")) {
            cam_x = 0.0f; cam_y = 260.0f; cam_z = -0.34f * TERRAIN_WORLD;
            look_at_peak();
        }
    }

    /* --- capture ----------------------------------------------------- */
    if (ImGui::CollapsingHeader("capture", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Button("screenshot (F2)")) screenshot();
        ImGui::SameLine();
        if (ImGui::Button("refresh line")) repro_dirty = true;
        if (repro_dirty) build_repro();
        ImGui::InputTextMultiline("reproduce", repro, sizeof repro,
                                  ImVec2(-8, 52), ImGuiInputTextFlags_ReadOnly);
        ImGui::TextDisabled("a screenshot is the scene only; this line\n"
                            "reproduces it byte for byte");
    }

    /* --- debug ------------------------------------------------------- */
    if (ImGui::CollapsingHeader("debug")) {
        ImGui::Checkbox("no frustum cull", (bool *)&gfx_nocull);
        const char *lods[4] = { "auto", "step 1", "step 2", "step 4" };
        int lc[4] = { 0, 1, 2, 4 };
        int li = 0;
        for (int i = 0; i < 4; i++) if (lc[i] == lod_clamp) li = i;
        if (ImGui::Combo("mesh step", &li, lods, 4)) lod_clamp = lc[li];
        if (ImGui::Button("flicker probe")) flicker_probe();
        ImGui::SameLine();
        ImGui::TextDisabled("freezes ~5 s");
    }
    ImGui::End();
    /* flush() builds the draw lists and must be outside a pass; draw() must be
       inside one, so they are split across the frame boundary */
    simgui_flush();
    pending = true;      /* present_pass() calls ui_draw_pass() inside its pass */
    ui_in_frame = 0;
}

/* Called from inside the swapchain pass that presents the frame, right before
   it ends. Only draws if a panel was built since the last draw, which is what
   keeps a capture (many render() calls per shot) free of the overlay. */
void ui_draw_pass(void) {
    if (!alive || !pending) return;
    pending = false;
    simgui_draw();
    if (getenv("UIDBG")) fprintf(stderr, "ui: drew %d lists\n", ImGui::GetDrawData()->CmdListsCount);
}
