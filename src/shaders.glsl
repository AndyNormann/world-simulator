// Shaders for the terrain viewer, in sokol-shdc annotated GLSL.
//
// Generate src/shaders.h with tools/shaders.sh; never edit the header by hand.
//
// The palette block is 15 vec4s written by build_palette() and uploaded as one
// blob to uniform block slot 0; the vertex block is mvp + wind + sun, uploaded
// to slot 1. Both match PAL_VECS and VS_BYTES in main.c -- if you add a member
// here, add it there in the same position, std140 order is the wire format.

@vs terrain_vs
layout(location=0) in vec3 pos;
layout(location=1) in vec3 nrm;
layout(location=2) in vec3 col;
layout(location=3) in vec2 anim;
layout(location=4) in vec2 flow;
layout(location=5) in vec4 sh0;
layout(location=6) in vec4 sh1;
layout(location=7) in vec4 b0;
layout(location=8) in vec4 b1;
layout(location=9) in vec4 b2;
layout(location=10) in vec4 b3;
layout(location=11) in vec4 b4;
layout(location=12) in vec4 b5;

layout(binding=1) uniform vs_params {
    mat4 mvp;
    vec4 u_wind;
    vec4 u_sunv;
};

out vec3 v_col;
out vec3 v_nrm;
out float v_depth;
out vec2 v_suv;
out vec3 v_wpos;
out vec2 v_flow;
out vec4 v_sh0;
out vec4 v_sh1;
out vec3 v_bounce;
out float v_water;

void main(){
    vec3 p = pos;
    float w = anim.x;
    if (w > 2.5) {
        // wing tips ride a flap, the whole bird drifts a little: |x| makes both
        // wings move together without a per-vertex side attribute
        float fl = sin(u_wind.w * 9.0 + anim.y * 43.0);
        p.y += fl * clamp(flow.x, -1.0, 1.0) * 0.85;
        float ph = anim.y * 6.2831853 + u_wind.w * 0.10;
        p.xz += vec2(cos(ph), sin(ph)) * 7.0;
    } else if (w < -1.5) {
        // flat water: no sway, no drift
    } else if (w < 0.0) {
        p.xz += u_wind.xy * (u_wind.w * 0.8 + anim.y * 40.0);
    } else if (w > 0.0) {
        float ph = anim.y * 6.2831853 + u_wind.w * 1.7;
        float sn = sin(ph);
        p.xz += u_wind.xy * ((0.85 * sn + 0.30) * u_wind.z * w * w);
        p.y  -= abs(sn) * 0.35 * u_wind.z * w * w;
    }
    // Bounce: each sector of surrounding ground is weighted by how much sun
    // falls on it, so the face away from the sun is filled with the colour of
    // whatever is lit on the sunny side -- green off the meadow, white off the
    // snowfield, cool off the river. The constant 0.30 term stands for the sky
    // bounce, without it the sum dies when the sun is overhead.
    vec3 bs[8] = vec3[](vec3(b0.x,b0.y,b0.z), vec3(b0.w,b1.x,b1.y),
                        vec3(b1.z,b1.w,b2.x), vec3(b2.y,b2.z,b2.w),
                        vec3(b3.x,b3.y,b3.z), vec3(b3.w,b4.x,b4.y),
                        vec3(b4.z,b4.w,b5.x), vec3(b5.y,b5.z,b5.w));
    vec2 sd = u_sunv.xz; float sl = length(sd);
    sd = sl > 1e-4 ? sd / sl : vec2(1.0, 0.0);
    vec3 bal = vec3(0.0); float bw = 0.0;
    for (int k = 0; k < 8; k++) {
        float a = float(k) * 0.78539816;
        float ww = 0.30 + max(dot(sd, vec2(cos(a), sin(a))), 0.0);
        bal += bs[k] * ww; bw += ww;
    }
    v_bounce = (bal / bw) * clamp(u_sunv.y, 0.0, 1.0);
    v_water = (anim.x < 0.5 && (length(flow) > 0.5 || anim.x < -1.5)) ? 1.0 : 0.0;
    v_col=col; v_nrm=nrm; v_wpos=pos; v_flow=flow; v_sh0=sh0; v_sh1=sh1;
    vec4 cp=mvp*vec4(p,1.0); v_depth=cp.w;
    v_suv=cp.xy/max(cp.w,1e-4)*0.5+0.5;
    gl_Position=cp;
}
@end

// The palette block and the sky/colour helpers, shared by both fragment
// shaders. Declared once: this used to be copy-pasted, so every new uniform had
// to be added in three places and a miss only showed up as a black frame.
@fs terrain_fs
layout(binding=0) uniform fs_params {
    vec4 u_sun;
    vec4 u_sky;
    vec4 u_sun_col;
    vec4 u_sky_amb;
    vec4 u_gnd_amb;
    vec4 u_fill;
    vec4 u_zenith;
    vec4 u_horizon;
    vec4 u_sunglow;
    vec4 u_cam_fwd;
    vec4 u_cam_right;
    vec4 u_cam_up;
    vec4 u_cam_pos;
    vec4 u_fog;
    vec4 u_load;
};

in vec3 v_col;
in vec3 v_nrm;
in float v_depth;
in vec2 v_suv;
in vec3 v_wpos;
in vec2 v_flow;
in vec4 v_sh0;
in vec4 v_sh1;
in vec3 v_bounce;
in float v_water;
out vec4 frag;

vec3 sky_col(float y){
    return mix(u_horizon.rgb, u_zenith.rgb, pow(clamp(y, 0.0, 1.0), 0.85));
}
float hash1(vec3 p){
    return fract(sin(dot(p, vec3(12.9898, 78.233, 37.719))) * 43758.5453);
}
float dither(vec2 p){
    return (hash1(vec3(p, 1.0)) - 0.5) / 255.0;
}

void main(){
    vec3 n = normalize(v_nrm);
    vec3 L = u_sun.xyz;
    float ndl = max(dot(n, L), 0.0) * 0.65 + 0.35;
    // shadow from the baked horizon: the sun is occluded when it sits below
    // the skyline in its own azimuth sector. Interpolating between the two
    // neighbouring sectors is what makes this continuous instead of stepping
    // every 45 degrees.
    float hs[8];
    hs[0]=v_sh0.x; hs[1]=v_sh0.y; hs[2]=v_sh0.z; hs[3]=v_sh0.w;
    hs[4]=v_sh1.x; hs[5]=v_sh1.y; hs[6]=v_sh1.z; hs[7]=v_sh1.w;
    for (int i = 0; i < 8; i++) hs[i] *= 1.6;
    float su = u_sky.x * 1.2732395;
    int k0 = int(floor(su)) & 7;
    int k1 = (k0 + 1) & 7;
    float occ = mix(hs[k0], hs[k1], fract(su));
    float vis = smoothstep(occ - 0.03, occ + 0.03, u_sun.w);
    vec3 amb = mix(u_gnd_amb.rgb, u_sky_amb.rgb, clamp(n.y * 0.5 + 0.5, 0.0, 1.0));
    vec3 c = v_col * (amb + u_sun_col.rgb * ndl * 1.05 * vis);
    c += v_col * u_fill.rgb * max(0.0, -dot(n, L));
    // bounce light: coloured, unshadowed, and added rather than multiplied by
    // the surface colour -- a green meadow must be able to light a brown rock
    c += v_bounce * u_sun_col.rgb * 0.30;
    if (v_water > 0.5 && length(v_flow) > 0.5) {
        float f = dot(v_wpos.xz, v_flow) * 0.10 - u_sky.z * 1.3;
        float lake = step(1.5, length(v_flow));
        c += vec3(0.10, 0.13, 0.13) * (0.5 + 0.5 * sin(f)) * (1.0 - lake);
        float g = hash1(floor(vec3(v_wpos.xz * 3.5, floor(u_sky.z * 2.0))));
        c += vec3(0.22) * step(0.9965, g) * (1.0 - smoothstep(220.0, 700.0, v_depth));
    }
    // Water: a Fresnel mix between the body colour and the sky the surface
    // sees. The sky comes from the same function that draws the backdrop, so
    // the sea cannot drift out of tune with the sky above it, and at a grazing
    // angle it becomes a mirror -- which is what makes a sea read as water
    // rather than as blue paint.
    if (v_water > 0.5) {
        vec3 V = normalize(v_wpos - u_cam_pos.xyz);
        float t = u_sky.z;
        vec2 q = v_wpos.xz;
        vec3 N = normalize(vec3(0.050 * sin(q.x * 0.31 + t * 1.7)
                             + 0.032 * sin(q.y * 0.57 - t * 2.3),
                               1.0,
                               0.050 * sin(q.y * 0.29 - t * 1.5)
                             + 0.032 * sin(q.x * 0.63 + t * 1.9)));
        float ndv = max(dot(N, -V), 0.0);
        float fres = 0.02 + 0.50 * pow(1.0 - ndv, 5.0);
        vec3 R = reflect(V, N);
        vec3 refl = sky_col(sqrt(max(R.y, 0.0)));
        float glint = pow(max(dot(R, u_sun.xyz), 0.0), 90.0) * (1.0 - u_sky.y);
        c = c * (1.0 - fres) + refl * fres * 0.90 + u_sun_col.rgb * glint * 0.9;
    }
    c = clamp(c * 1.15, 0.0, 1.0);
    c = mix(c, c * c * (3.0 - 2.0 * c), 0.55);
    // Aerial perspective: linear in distance to a full 1.0, so the last thing
    // in the world is the sky and not a silhouette of terrain. Linear because
    // at this facet size an exponential falloff is not readable; swap it in if
    // a longer draw distance ever makes the gradient itself visible.
    float fog = clamp((v_depth - u_fog.x) / (u_fog.y - u_fog.x), 0.0, 1.0);
    vec3 ray = normalize(u_cam_fwd.xyz
               + u_cam_right.xyz * ((v_suv.x * 2.0 - 1.0) * u_cam_fwd.w * u_cam_right.w)
               + u_cam_up.xyz    * ((v_suv.y * 2.0 - 1.0) * u_cam_fwd.w));
    // in-scattered sunlight in the haze, which is what a shaft of god rays is
    // made of minus the shadowing -- free here because the ray and the sun are
    // already in hand, and no second pass or occlusion buffer is needed
    vec3 haze = sky_col(sqrt(max(ray.y, 0.0)));
    haze += u_sun_col.rgb * pow(max(dot(ray, u_sun.xyz), 0.0), 8.0) * 0.30 * (1.0 - u_sky.y);
    vec3 o = mix(c, haze, fog) + dither(gl_FragCoord.xy);
    frag = vec4(clamp(o, 0.0, 1.0), 1.0);
}
@end

// Sky: gradient plus sun/moon disc and stars, drawn before the terrain with
// depth writes off. The ray is rebuilt from the camera basis so the sun stays
// put in the world when you turn.
@vs sky_vs
out vec2 v_uv;
void main(){
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    v_uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 1.0, 1.0);
}
@end

@fs sky_fs
layout(binding=0) uniform sky_params {
    vec4 u_sun;
    vec4 u_sky;
    vec4 u_sun_col;
    vec4 u_sky_amb;
    vec4 u_gnd_amb;
    vec4 u_fill;
    vec4 u_zenith;
    vec4 u_horizon;
    vec4 u_sunglow;
    vec4 u_cam_fwd;
    vec4 u_cam_right;
    vec4 u_cam_up;
    vec4 u_cam_pos;
    vec4 u_fog;
    vec4 u_load;
};

in vec2 v_uv;
out vec4 frag;

vec3 sky_col(float y){
    return mix(u_horizon.rgb, u_zenith.rgb, pow(clamp(y, 0.0, 1.0), 0.85));
}
float hash1(vec3 p){
    return fract(sin(dot(p, vec3(12.9898, 78.233, 37.719))) * 43758.5453);
}
float dither(vec2 p){
    return (hash1(vec3(p, 1.0)) - 0.5) / 255.0;
}

void main(){
    float th = u_cam_fwd.w, asp = u_cam_right.w;
    vec3 ray = normalize(u_cam_fwd.xyz
               + u_cam_right.xyz * ((v_uv.x * 2.0 - 1.0) * th * asp)
               + u_cam_up.xyz    * ((v_uv.y * 2.0 - 1.0) * th));
    vec3 c = sky_col(sqrt(max(ray.y, 0.0)));
    float sd = max(dot(ray, u_sun.xyz), 0.0);
    c += u_sun_col.rgb * (smoothstep(0.99955, 0.99975, sd) * u_sunglow.x
                        + pow(sd, 24.0) * u_sunglow.y);
    if (u_sky.y > 0.01) {
        vec3 sp = ray * 260.0;
        vec3 cell = floor(sp);
        vec3 fr = fract(sp) - 0.5;
        float st = step(0.9975, hash1(cell)) * smoothstep(0.45, 0.0, length(fr.xy))
                 * smoothstep(0.02, 0.20, ray.y);
        c += vec3(0.92, 0.95, 1.0) * st * u_sky.y;
    }
    // Loading: until every chunk is uploaded the world is drawn only as sky,
    // so the first thing on screen is a deliberate frame instead of terrain
    // assembling itself. u_load = (progress, 1/w, 1/h, active).
    if (u_load.w > 0.5) {
        c *= 0.28;
        vec2 q = gl_FragCoord.xy * u_load.yz;
        float y = q.y;
        if (y > 0.045 && y < 0.062 && q.x > 0.30 && q.x < 0.70)
            c = vec3(0.13, 0.14, 0.15);
        if (y > 0.045 && y < 0.062 && q.x > 0.30 && q.x < 0.30 + 0.40 * u_load.x)
            c = vec3(0.96, 0.93, 0.86);
    }
    frag = vec4(clamp(c + dither(gl_FragCoord.xy), 0.0, 1.0), 1.0);
}
@end

@program terrain terrain_vs terrain_fs
@program sky sky_vs sky_fs
