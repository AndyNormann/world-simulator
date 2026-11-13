#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct taa_params
{
    float4 cf;
    float4 cr;
    float4 cu;
    float4 eye;
    float4x4 prev_vp;
    float4 pz;
};

struct main0_out
{
    float4 frag_color [[color(0)]];
};

struct main0_in
{
    float2 v_uv [[user(locn0)]];
};

fragment main0_out main0(main0_in in [[stage_in]], constant taa_params& _50 [[buffer(0)]], texture2d<float> cur_tex [[texture(0)]], texture2d<float> hist_tex [[texture(1)]], texture2d<float> depth_tex [[texture(2)]], sampler smp [[sampler(0)]], sampler smpn [[sampler(2)]])
{
    main0_out out = {};
    float4 _25 = cur_tex.sample(smp, in.v_uv);
    float3 _26 = _25.xyz;
    float4 _35 = depth_tex.sample(smpn, in.v_uv);
    float _38 = _35.x;
    float3 outc = _26;
    bool _44 = _38 > 0.0;
    bool _58;
    if (_44)
    {
        _58 = _50.eye.w > 0.0;
    }
    else
    {
        _58 = _44;
    }
    if (_58)
    {
        float2 _83 = float2((in.v_uv.x * 2.0) - 1.0, 1.0 - (in.v_uv.y * 2.0)) - _50.pz.zw;
        float4 _122 = _50.prev_vp * float4(_50.eye.xyz + (((_50.cf.xyz + (_50.cr.xyz * _83.x)) + (_50.cu.xyz * _83.y)) * _38), 1.0);
        float _124 = _122.w;
        if (_124 > 0.0)
        {
            float _136 = ((_122.x / _124) * 0.5) + 0.5;
            float _143 = 0.5 - ((_122.y / _124) * 0.5);
            bool _147 = _136 > 0.0;
            bool _153;
            if (_147)
            {
                _153 = _136 < 1.0;
            }
            else
            {
                _153 = _147;
            }
            bool _159;
            if (_153)
            {
                _159 = _143 > 0.0;
            }
            else
            {
                _159 = _153;
            }
            bool _165;
            if (_159)
            {
                _165 = _143 < 1.0;
            }
            else
            {
                _165 = _159;
            }
            if (_165)
            {
                float3 lo = _26;
                float3 hi = _26;
                for (int y = -1; y <= 1; y++)
                {
                    for (int x = -1; x <= 1; x++)
                    {
                        float3 _208 = cur_tex.sample(smp, (in.v_uv + float2(float(x) * _50.cr.w, float(y) * _50.cu.w))).xyz;
                        lo = fast::min(lo, _208);
                        hi = fast::max(hi, _208);
                    }
                }
                outc = mix(_26, fast::clamp(hist_tex.sample(smp, float2(_136, _143)).xyz, lo, hi), float3(_50.eye.w));
            }
        }
    }
    out.frag_color = float4(outc, 1.0);
    return out;
}

