#pragma clang diagnostic ignored "-Wmissing-prototypes"

#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct sky_params
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
    float4 u_hor;
};

struct main0_out
{
    float4 frag [[color(0)]];
    float4 frag_lin [[color(1)]];
};

struct main0_in
{
    float2 v_uv [[user(locn0)]];
};

static inline __attribute__((always_inline))
float3 sky_col(thread const float& y, constant sky_params& _27)
{
    return mix(_27.u_horizon.xyz, _27.u_zenith.xyz, float3(powr(fast::clamp(y, 0.0, 1.0), 0.85000002384185791015625)));
}

static inline __attribute__((always_inline))
float hash1(thread const float3& p)
{
    return fract(sin(dot(p, float3(12.98980045318603515625, 78.233001708984375, 37.71900177001953125))) * 43758.546875);
}

static inline __attribute__((always_inline))
float dither(thread const float2& p)
{
    float3 param = float3(p, 1.0);
    return (hash1(param) - 0.5) * 0.0039215688593685626983642578125;
}

fragment main0_out main0(main0_in in [[stage_in]], constant sky_params& _27 [[buffer(0)]], float4 gl_FragCoord [[position]])
{
    main0_out out = {};
    float3 _118 = fast::normalize((_27.u_cam_fwd.xyz + (_27.u_cam_right.xyz * ((((in.v_uv.x * 2.0) - 1.0) * _27.u_cam_fwd.w) * _27.u_cam_right.w))) + (_27.u_cam_up.xyz * (((in.v_uv.y * 2.0) - 1.0) * _27.u_cam_fwd.w)));
    float _121 = _118.y;
    float param = sqrt(fast::max(_121, 0.0));
    float _133 = fast::max(dot(_118, _27.u_sun.xyz), 0.0);
    float3 c = sky_col(param, _27) + (_27.u_sun_col.xyz * ((smoothstep(0.99954998493194580078125, 0.99975001811981201171875, _133) * _27.u_sunglow.x) + (powr(_133, 24.0) * _27.u_sunglow.y)));
    if (_27.u_sky.y > 0.00999999977648258209228515625)
    {
        float3 _167 = _118 * 260.0;
        float3 param_1 = floor(_167);
        c += ((float3(0.920000016689300537109375, 0.949999988079071044921875, 1.0) * ((step(0.997500002384185791015625, hash1(param_1)) * smoothstep(0.449999988079071044921875, 0.0, length((fract(_167) - float3(0.5)).xy))) * smoothstep(0.0199999995529651641845703125, 0.20000000298023223876953125, _121))) * _27.u_sky.y);
    }
    if (_27.u_load.w > 0.5)
    {
        c *= 0.2800000011920928955078125;
        float2 _221 = gl_FragCoord.xy * _27.u_load.yz;
        float _224 = _221.y;
        bool _231 = (_224 > 0.04500000178813934326171875) && (_224 < 0.061999998986721038818359375);
        bool _238;
        if (_231)
        {
            _238 = _221.x > 0.300000011920928955078125;
        }
        else
        {
            _238 = _231;
        }
        bool _245;
        if (_238)
        {
            _245 = _221.x < 0.699999988079071044921875;
        }
        else
        {
            _245 = _238;
        }
        if (_245)
        {
            c = float3(0.12999999523162841796875, 0.14000000059604644775390625, 0.1500000059604644775390625);
        }
        bool _262;
        if (_231)
        {
            _262 = _221.x > 0.300000011920928955078125;
        }
        else
        {
            _262 = _231;
        }
        bool _273;
        if (_262)
        {
            _273 = _221.x < (0.300000011920928955078125 + (0.4000000059604644775390625 * _27.u_load.x));
        }
        else
        {
            _273 = _262;
        }
        if (_273)
        {
            c = float3(0.959999978542327880859375, 0.930000007152557373046875, 0.86000001430511474609375);
        }
    }
    float2 param_2 = gl_FragCoord.xy;
    out.frag = float4(fast::clamp(c + float3(dither(param_2)), float3(0.0), float3(1.0)), 1.0);
    out.frag_lin = float4(0.0);
    return out;
}

