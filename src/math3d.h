#ifndef MATH3D_H
#define MATH3D_H
/* Minimal column-major 4x4 math for OpenGL-style clip space.
   Column-major: element (row r, col c) lives at m[c*4+r]. */

#include <math.h>

static inline void mat4_mul(float *r, const float *a, const float *b) {
    /* r = a * b */
    for (int c = 0; c < 4; c++) {
        for (int row = 0; row < 4; row++) {
            float s = 0.0f;
            for (int k = 0; k < 4; k++) s += a[k*4+row] * b[c*4+k];
            r[c*4+row] = s;
        }
    }
}

static inline void mat4_perspective(float *m, float fov_y, float aspect, float z_near, float z_far) {
    float f = 1.0f / tanf(fov_y * 0.5f);
    for (int i = 0; i < 16; i++) m[i] = 0.0f;
    m[0] = f / aspect;
    m[5] = f;
    m[10] = (z_far + z_near) / (z_near - z_far);
    m[11] = -1.0f;
    m[14] = (2.0f * z_far * z_near) / (z_near - z_far);
}

static inline void mat4_lookat(float *m, float ex, float ey, float ez,
                               float cx, float cy, float cz) {
    /* up is always (0,1,0); looking straight up/down is handled by a fallback. */
    float fx = cx - ex, fy = cy - ey, fz = cz - ez;
    float fl = sqrtf(fx*fx + fy*fy + fz*fz);
    if (fl < 1e-5f) { fx = 0; fy = 0; fz = -1; fl = 1; }
    fx /= fl; fy /= fl; fz /= fl;

    float sx = -fz, sy = 0.0f, sz = fx;          /* right = cross(fwd, up) */
    float sl = sqrtf(sx*sx + sy*sy + sz*sz);
    /* ponytail: fwd parallel to up (straight down/up) makes `right` zero.
       Fall back to world X instead of dividing by ~0 and producing NaNs. */
    if (sl < 1e-5f) { sx = 1.0f; sy = 0.0f; sz = 0.0f; sl = 1.0f; }
    sx /= sl; sy /= sl; sz /= sl;

    float ux = sy*fz - sz*fy, uy = sz*fx - sx*fz, uz = sx*fy - sy*fx;

    /* Column-major: the view matrix's ROWS are the basis vectors, so each
       basis vector occupies a column of the array (m[0],m[4],m[8] is row 0).
       Writing the basis down column 0 instead transposes the rotation -- which
       is only invisible for axis-aligned yaw and shows up as the world
       spinning around the camera at every other angle. */
    m[0] = sx;  m[4] = sy;  m[8]  = sz;  m[12] = -(sx*ex + sy*ey + sz*ez);
    m[1] = ux;  m[5] = uy;  m[9]  = uz;  m[13] = -(ux*ex + uy*ey + uz*ez);
    m[2] = -fx; m[6] = -fy; m[10] = -fz; m[14] =  (fx*ex + fy*ey + fz*ez);
    m[3] = 0.0f; m[7] = 0.0f; m[11] = 0.0f; m[15] = 1.0f;
}

/* transform point by column-major matrix and apply the perspective divide,
   writing NDC xyz */
static inline void mat4_point(const float *m, float x, float y, float z,
                              float *ox, float *oy, float *oz) {
    float cx = m[0]*x + m[4]*y + m[8]*z  + m[12];
    float cy = m[1]*x + m[5]*y + m[9]*z  + m[13];
    float cz = m[2]*x + m[6]*y + m[10]*z + m[14];
    float cw = m[3]*x + m[7]*y + m[11]*z + m[15];
    if (cw != 0.0f) { cx /= cw; cy /= cw; cz /= cw; }
    *ox = cx; *oy = cy; *oz = cz;
}

#endif
