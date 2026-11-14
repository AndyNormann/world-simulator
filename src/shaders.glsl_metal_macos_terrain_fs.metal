#pragma clang diagnostic ignored "-Wmissing-prototypes"
#pragma clang diagnostic ignored "-Wmissing-braces"

#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

template<typename T, size_t Num>
struct spvUnsafeArray
{
    T elements[Num ? Num : 1];
    
    thread T& operator [] (size_t pos) thread
    {
        return elements[pos];
    }
    constexpr const thread T& operator [] (size_t pos) const thread
    {
        return elements[pos];
    }
    
    device T& operator [] (size_t pos) device
    {
        return elements[pos];
    }
    constexpr const device T& operator [] (size_t pos) const device
    {
        return elements[pos];
    }
    
    constexpr const constant T& operator [] (size_t pos) const constant
    {
        return elements[pos];
    }
    
    threadgroup T& operator [] (size_t pos) threadgroup
    {
        return elements[pos];
    }
    constexpr const threadgroup T& operator [] (size_t pos) const threadgroup
    {
        return elements[pos];
    }
};

struct fs_params
{
    float4 u_sun;
    float4 u_sky;
    float4 u_sun_col;
    float4 u_sky_amb;
    float4 u_gnd_amb;
    float4 u_fill;
    float4 u_zenith;
    float4 u_horizon;
    float4 u_sunglow;
    float4 u_cam_fwd;
    float4 u_cam_right;
    float4 u_cam_up;
    float4 u_cam_pos;
    float4 u_fog;
    float4 u_load;
};

struct main0_out
{
    float4 frag [[color(0)]];
    float4 frag_lin [[color(1)]];
};

struct main0_in
{
    float3 v_col [[user(locn0)]];
    float3 v_nrm [[user(locn1)]];
    float v_depth [[user(locn2)]];
    float2 v_suv [[user(locn3)]];
    float3 v_wpos [[user(locn4)]];
    float2 v_flow [[user(locn5)]];
    float4 v_sh0 [[user(locn6)]];
    float4 v_sh1 [[user(locn7)]];
    float3 v_bounce [[user(locn8)]];
    float v_water [[user(locn9)]];
};

static inline __attribute__((always_inline))
float hash1(thread const float3& p)
{
    return fract(sin(dot(p, float3(12.98980045318603515625, 78.233001708984375, 37.71900177001953125))) * 43758.546875);
}

static inline __attribute__((always_inline))
float3 sky_col(thread const float& y, constant fs_params& _27)
{
    return mix(_27.u_horizon.xyz, _27.u_zenith.xyz, float3(powr(fast::clamp(y, 0.0, 1.0), 0.85000002384185791015625)));
}

static inline __attribute__((always_inline))
float dither(thread const float2& p)
{
    float3 param = float3(p, 1.0);
    return (hash1(param) - 0.5) * 0.0039215688593685626983642578125;
}

fragment main0_out main0(main0_in in [[stage_in]], constant fs_params& _27 [[buffer(0)]], float4 gl_FragCoord [[position]])
{
    main0_out out = {};
    float3 n = fast::normalize(in.v_nrm);
    bool _81 = in.v_water > 0.5;
    if (_81)
    {
        n = float3(0.0, 1.0, 0.0);
    }
    spvUnsafeArray<float, 8> hs;
    hs[0] = in.v_sh0.x;
    hs[1] = in.v_sh0.y;
    hs[2] = in.v_sh0.z;
    hs[3] = in.v_sh0.w;
    hs[4] = in.v_sh1.x;
    hs[5] = in.v_sh1.y;
    hs[6] = in.v_sh1.z;
    hs[7] = in.v_sh1.w;
    for (int i = 0; i < 8; i++)
    {
        hs[i] *= 1.60000002384185791015625;
    }
    float _163 = _27.u_sky.x * 1.27323949337005615234375;
    int _168 = int(floor(_163)) & 7;
    float _182 = mix(hs[_168], hs[(_168 + 1) & 7], fract(_163));
    float vis = smoothstep(_182 - 0.02999999932944774627685546875, _182 + 0.02999999932944774627685546875, _27.u_sun.w);
    if (_81)
    {
        vis = 1.0;
    }
    float3 c = ((in.v_col * (mix(_27.u_gnd_amb.xyz, _27.u_sky_amb.xyz, float3(fast::clamp((n.y * 0.5) + 0.5, 0.0, 1.0))) + (((_27.u_sun_col.xyz * ((fast::max(dot(n, _27.u_sun.xyz), 0.0) * 0.64999997615814208984375) + 0.3499999940395355224609375)) * 1.0499999523162841796875) * vis))) + ((in.v_col * _27.u_fill.xyz) * fast::max(0.0, -dot(n, _27.u_sun.xyz)))) + ((in.v_bounce * _27.u_sun_col.xyz) * 0.300000011920928955078125);
    float _293 = 1.0 - smoothstep(60.0, 400.0, (in.v_depth + ((110.0 * sin((in.v_wpos.x * 0.00310000008903443813323974609375) + 1.7000000476837158203125)) * sin((in.v_wpos.z * 0.00430000014603137969970703125) - 0.60000002384185791015625))) + ((70.0 * sin((in.v_wpos.x * 0.02119999937713146209716796875) - 2.2999999523162841796875)) * sin((in.v_wpos.z * 0.01679999940097332000732421875) + 1.10000002384185791015625)));
    bool _303;
    if (_81)
    {
        _303 = length(in.v_flow) > 0.5;
    }
    else
    {
        _303 = _81;
    }
    if (_303)
    {
        c += (((float3(0.100000001490116119384765625, 0.12999999523162841796875, 0.12999999523162841796875) * (0.5 + (0.5 * sin((dot(in.v_wpos.xz, in.v_flow) * 0.100000001490116119384765625) - (_27.u_sky.z * 1.2999999523162841796875))))) * (1.0 - step(1.5, length(in.v_flow)))) * _293);
        float3 param = floor(float3(in.v_wpos.xz * 3.5, floor(_27.u_sky.z * 2.0)));
        c += ((float3(0.2199999988079071044921875) * step(0.9965000152587890625, hash1(param))) * (1.0 - smoothstep(220.0, 700.0, in.v_depth)));
    }
    if (_81)
    {
        float3 _378 = fast::normalize(in.v_wpos - _27.u_cam_pos.xyz);
        float3 _458 = fast::normalize(float3((0.02999999932944774627685546875 * sin((in.v_wpos.x * 0.0041999998502433300018310546875) + (_27.u_sky.z * 0.20999999344348907470703125))) + (((0.0500000007450580596923828125 * sin((in.v_wpos.x * 0.310000002384185791015625) + (_27.u_sky.z * 1.7000000476837158203125))) + (0.0320000015199184417724609375 * sin((in.v_wpos.z * 0.569999992847442626953125) - (_27.u_sky.z * 2.2999999523162841796875)))) * _293), 1.0, (0.02999999932944774627685546875 * sin((in.v_wpos.z * 0.0035999999381601810455322265625) - (_27.u_sky.z * 0.17000000178813934326171875))) + (((0.0500000007450580596923828125 * sin((in.v_wpos.z * 0.2899999916553497314453125) - (_27.u_sky.z * 1.5))) + (0.0320000015199184417724609375 * sin((in.v_wpos.x * 0.62999999523162841796875) + (_27.u_sky.z * 1.89999997615814208984375)))) * _293)));
        float _471 = 0.5 * powr(1.0 - fast::max(dot(_458, -_378), 0.0), 5.0);
        float3 _476 = reflect(_378, _458);
        float param_1 = sqrt(fast::max(_476.y, 0.0));
        c = ((c * (0.980000019073486328125 - _471)) + ((sky_col(param_1, _27) * (0.0199999995529651641845703125 + _471)) * 0.89999997615814208984375)) + ((_27.u_sun_col.xyz * (powr(fast::max(dot(_476, _27.u_sun.xyz), 0.0), 90.0) * (1.0 - _27.u_sky.y))) * 0.89999997615814208984375);
    }
    float3 _519 = fast::clamp(c * 1.14999997615814208984375, float3(0.0), float3(1.0));
    c = mix(_519, (_519 * _519) * (float3(3.0) - (_519 * 2.0)), float3(0.550000011920928955078125));
    float3 _581 = fast::normalize((_27.u_cam_fwd.xyz + (_27.u_cam_right.xyz * ((((in.v_suv.x * 2.0) - 1.0) * _27.u_cam_fwd.w) * _27.u_cam_right.w))) + (_27.u_cam_up.xyz * (((in.v_suv.y * 2.0) - 1.0) * _27.u_cam_fwd.w)));
    float param_2 = sqrt(fast::max(_581.y, 0.0));
    float2 param_3 = gl_FragCoord.xy;
    out.frag = float4(fast::clamp(mix(c, sky_col(param_2, _27) + (((_27.u_sun_col.xyz * powr(fast::max(dot(_581, _27.u_sun.xyz), 0.0), 8.0)) * 0.300000011920928955078125) * (1.0 - _27.u_sky.y)), float3(fast::clamp((in.v_depth - _27.u_fog.x) / (_27.u_fog.y - _27.u_fog.x), 0.0, 1.0))) + float3(dither(param_3)), float3(0.0), float3(1.0)), 1.0);
    out.frag_lin = float4(in.v_depth, 0.0, 0.0, 0.0);
    return out;
}

