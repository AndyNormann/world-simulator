/* Self-check for src/math3d.h. Build+run: ./build.sh test */
#include "../src/math3d.h"
#include <assert.h>
#include <stdio.h>

#define NEAR(a, b) (fabsf((a) - (b)) < 1e-4f)

static void test_perspective_maps_near_far(void) {
    float p[16];
    mat4_perspective(p, 1.0f, 16.0f/9.0f, 0.1f, 100.0f);
    /* reversed-Z: near maps to 1, far to 0, and the order is monotonic in
       between (which is the property the depth test depends on) */
    float x, y, z;
    mat4_point(p, 0, 0, -0.1f, &x, &y, &z);
    assert(NEAR(z, 1.0f));
    mat4_point(p, 0, 0, -100.0f, &x, &y, &z);
    assert(NEAR(z, 0.0f));
    float znear_z, zmid_z, zfar_z;
    mat4_point(p, 0, 0, -0.1f, &x, &y, &znear_z);
    mat4_point(p, 0, 0, -1.0f, &x, &y, &zmid_z);
    mat4_point(p, 0, 0, -100.0f, &x, &y, &zfar_z);
    assert(znear_z > zmid_z && zmid_z > zfar_z);
    /* a point one unit off-axis at distance 1 maps to NDC x = f/aspect */
    mat4_point(p, 0.1f, 0, -0.1f, &x, &y, &z);
    assert(NEAR(x, (1.0f/tanf(0.5f)) / (16.0f/9.0f)));
}

static void test_lookat_places_camera_at_origin(void) {
    float v[16];
    mat4_lookat(v, 0, 0, 5, 0, 0, 0);   /* looking down -z from z=+5 */
    float x, y, z;
    mat4_point(v, 0, 0, 5, &x, &y, &z); /* camera position -> origin */
    assert(NEAR(x, 0) && NEAR(y, 0) && NEAR(z, 0));
    mat4_point(v, 0, 0, 0, &x, &y, &z); /* target is 5 ahead, i.e. z = -5 */
    assert(NEAR(x, 0) && NEAR(y, 0) && NEAR(z, -5.0f));
}

static void test_lookat_straight_down_is_finite(void) {
    /* this exact case used to produce NaN and render nothing */
    float v[16];
    mat4_lookat(v, 0, 30, 0, 0, 0, 0);
    for (int i = 0; i < 16; i++) assert(isfinite(v[i]));
    float x, y, z;
    mat4_point(v, 0, 0, 0, &x, &y, &z);
    assert(isfinite(x) && isfinite(y) && isfinite(z));
    assert(NEAR(z, -30.0f));   /* ground straight below at distance 30 */
}

static void test_view_then_projection_is_finite(void) {
    float v[16], p[16], mvp[16], x, y, z;
    mat4_lookat(v, 0, 25, -30, 0, 20, 0);
    mat4_perspective(p, 1.0f, 1280.0f/720.0f, 0.1f, 400.0f);
    mat4_mul(mvp, p, v);
    for (int i = 0; i < 16; i++) assert(isfinite(mvp[i]));
    mat4_point(mvp, 0, 0, 0, &x, &y, &z);
    assert(isfinite(x) && isfinite(y) && isfinite(z));
}

static void test_mul_identity(void) {
    float i[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    float m[16] = {1,2,3,4, 5,6,7,8, 9,10,11,12, 13,14,15,16};
    float r[16];
    mat4_mul(r, i, m);
    for (int k = 0; k < 16; k++) assert(NEAR(r[k], m[k]));
    mat4_mul(r, m, i);
    for (int k = 0; k < 16; k++) assert(NEAR(r[k], m[k]));
}

static void test_world_x_appears_left_when_looking_along_plus_z(void) {
    /* Documents the handedness that caused inverted strafing/mouse: with the
       camera looking along +z, world +x is on the LEFT of the screen. */
    float v[16], x, y, z;
    mat4_lookat(v, 0, 0, 0, 0, 0, 1);
    mat4_point(v, 1, 0, 5, &x, &y, &z);
    assert(x < 0.0f);
    mat4_point(v, -1, 0, 5, &x, &y, &z);
    assert(x > 0.0f);
    /* and looking along -z, world +x is on the right */
    mat4_lookat(v, 0, 0, 0, 0, 0, -1);
    mat4_point(v, 1, 0, 5, &x, &y, &z);
    assert(x > 0.0f);
}

static void test_lookat_arbitrary_angle(void) {
    /* The bug that survived all axis-aligned tests: at a general yaw the
       transposed rotation made the world orbit the camera. Check a target that
       is off every axis maps straight ahead, and the eye maps to the origin. */
    float v[16], x, y, z, d;
    mat4_lookat(v, 5, 3, -7, 1, 2, 3);
    float dx = 1-5, dy = 2-3, dz = 3+7;
    d = sqrtf(dx*dx + dy*dy + dz*dz);
    mat4_point(v, 1, 2, 3, &x, &y, &z);   /* target: straight ahead, on -z */
    assert(NEAR(x, 0) && NEAR(y, 0) && NEAR(z, -d));
    mat4_point(v, 5, 3, -7, &x, &y, &z);  /* eye: view-space origin */
    assert(NEAR(x, 0) && NEAR(y, 0) && NEAR(z, 0));
}

static void test_lookat_right_handed_at_yaw(void) {
    /* A point offset along the camera's right vector must land at ndc x > 0. */
    float v[16], x, y, z;
    for (int i = -6; i <= 6; i++) {
        float yaw = i * 0.2f, pitch = 0.15f;
        float fx = sinf(yaw)*cosf(pitch), fy = sinf(pitch), fz = cosf(yaw)*cosf(pitch);
        float rx = -fz, rz = fx;                    /* right = cross(fwd, up) */
        float rl = sqrtf(rx*rx + rz*rz); rx /= rl; rz /= rl;
        mat4_lookat(v, 0, 10, 0, fx, 10 + fy, fz);
        mat4_point(v, rx*3.0f, 10, rz*3.0f, &x, &y, &z);  /* 3 units right */
        if (!(x > 0.0f)) { printf("  right-handed check failed at yaw=%.2f (ndc x=%.3f)\n", yaw, x); }
        assert(x > 0.0f);
        assert(NEAR(y, 0));   /* same height as eye -> on the horizon */
    }
}

static void test_panning_keeps_horizon_level(void) {
    /* A point at eye height sits at view-space y = 0, so it must project onto
       the horizon (ndc y == 0) for EVERY yaw and pitch combination. */
    float proj[16], view[16], mvp[16];
    mat4_perspective(proj, 1.0f, 16.0f/9.0f, 0.1f, 400.0f);
    for (int i = -8; i <= 8; i++) {
        float yaw = i * 0.125f;
        float fx = sinf(yaw), fz = cosf(yaw);
        mat4_lookat(view, 0, 30, 0, fx, 30.0f, fz);
        mat4_mul(mvp, proj, view);
        float x, y, z;
        mat4_point(mvp, 0, 30, 200, &x, &y, &z);
        assert(fabsf(y) < 1e-3f);
    }
}

int main(void) {
    test_mul_identity();
    test_perspective_maps_near_far();
    test_lookat_places_camera_at_origin();
    test_lookat_straight_down_is_finite();
    test_view_then_projection_is_finite();
    test_world_x_appears_left_when_looking_along_plus_z();
    test_lookat_arbitrary_angle();
    test_lookat_right_handed_at_yaw();
    test_panning_keeps_horizon_level();
    printf("math3d: all checks passed\n");
    return 0;
}
