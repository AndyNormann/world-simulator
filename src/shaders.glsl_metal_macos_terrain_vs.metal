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

struct vs_params
{
    float4x4 mvp;
    float4 u_wind;
    float4 u_sunv;
};

struct main0_out
{
    float3 v_col [[user(locn0)]];
    float3 v_nrm [[user(locn1)]];
    float v_depth [[user(locn2)]];
    float2 v_suv [[user(locn3)]];
    float3 v_wpos [[user(locn4)]];
    float2 v_flow [[user(locn5)]];
    float3 v_bounce [[user(locn6)]];
    float v_water [[user(locn7)]];
    float4 gl_Position [[position]];
};

struct main0_in
{
    float3 pos [[attribute(0)]];
    float3 nrm [[attribute(1)]];
    float3 col [[attribute(2)]];
    float2 anim [[attribute(3)]];
    float2 flow [[attribute(4)]];
    float4 b0 [[attribute(5)]];
    float4 b1 [[attribute(6)]];
    float4 b2 [[attribute(7)]];
    float4 b3 [[attribute(8)]];
    float4 b4 [[attribute(9)]];
    float4 b5 [[attribute(10)]];
};

vertex main0_out main0(main0_in in [[stage_in]], constant vs_params& _34 [[buffer(1)]])
{
    main0_out out = {};
    float3 p = in.pos;
    if (in.anim.x > 2.5)
    {
        float _73 = (in.anim.y * 6.283185482025146484375) + (_34.u_wind.w * 0.100000001490116119384765625);
        float2 _83 = p.xz + (float2(cos(_73), sin(_73)) * 7.0);
        p = float3(_83.x, p.y + ((sin((_34.u_wind.w * 9.0) + (in.anim.y * 43.0)) * fast::clamp(in.flow.x, -1.0, 1.0)) * 0.85000002384185791015625), _83.y);
    }
    else
    {
        if (in.anim.x < (-1.5))
        {
        }
        else
        {
            if (in.anim.x < 0.0)
            {
                float3 _115 = p;
                float2 _117 = _115.xz + (_34.u_wind.xy * ((_34.u_wind.w * 0.800000011920928955078125) + (in.anim.y * 40.0)));
                float3 _388 = _115;
                _388.x = _117.x;
                _388.z = _117.y;
                p = _388;
            }
            else
            {
                if (in.anim.x > 0.0)
                {
                    float _138 = sin((in.anim.y * 6.283185482025146484375) + (_34.u_wind.w * 1.7000000476837158203125));
                    float2 _156 = p.xz + (_34.u_wind.xy * (((((0.85000002384185791015625 * _138) + 0.300000011920928955078125) * _34.u_wind.z) * in.anim.x) * in.anim.x));
                    p = float3(_156.x, p.y - ((((abs(_138) * 0.3499999940395355224609375) * _34.u_wind.z) * in.anim.x) * in.anim.x), _156.y);
                }
            }
        }
    }
    spvUnsafeArray<float3, 8> _243 = spvUnsafeArray<float3, 8>({ float3(in.b0.x, in.b0.y, in.b0.z), float3(in.b0.w, in.b1.x, in.b1.y), float3(in.b1.z, in.b1.w, in.b2.x), float3(in.b2.y, in.b2.z, in.b2.w), float3(in.b3.x, in.b3.y, in.b3.z), float3(in.b3.w, in.b4.x, in.b4.y), float3(in.b4.z, in.b4.w, in.b5.x), float3(in.b5.y, in.b5.z, in.b5.w) });
    spvUnsafeArray<float3, 8> bs = _243;
    float2 sd = _34.u_sunv.xz;
    float _252 = length(_34.u_sunv.xz);
    float2 _256;
    if (_252 > 9.9999997473787516355514526367188e-05)
    {
        _256 = sd / float2(_252);
    }
    else
    {
        _256 = float2(1.0, 0.0);
    }
    sd = _256;
    float3 bal = float3(0.0);
    float bw = 0.0;
    for (int k = 0; k < 8; k++)
    {
        float _284 = float(k) * 0.785398185253143310546875;
        float _294 = 0.300000011920928955078125 + fast::max(dot(sd, float2(cos(_284), sin(_284))), 0.0);
        bal += (bs[k] * _294);
        bw += _294;
    }
    out.v_bounce = (bal / float3(bw)) * fast::clamp(_34.u_sunv.y, 0.0, 1.0);
    bool _322 = in.anim.x < 0.5;
    bool _335;
    if (_322)
    {
        bool _327 = length(in.flow) > 0.5;
        bool _334;
        if (!_327)
        {
            _334 = in.anim.x < (-1.5);
        }
        else
        {
            _334 = _327;
        }
        _335 = _334;
    }
    else
    {
        _335 = _322;
    }
    out.v_water = float(_335);
    out.v_col = in.col;
    out.v_nrm = in.nrm;
    out.v_wpos = in.pos;
    out.v_flow = in.flow;
    float4 _358 = _34.mvp * float4(p, 1.0);
    float _361 = _358.w;
    out.v_depth = _361;
    out.v_suv = ((_358.xy / float2(fast::max(_361, 9.9999997473787516355514526367188e-05))) * 0.5) + float2(0.5);
    out.gl_Position = _358;
    return out;
}

