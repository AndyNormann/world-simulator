#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct main0_out
{
    float4 frag_color [[color(0)]];
};

struct main0_in
{
    float2 v_uv [[user(locn0)]];
};

fragment main0_out main0(main0_in in [[stage_in]], texture2d<float> src_tex [[texture(0)]], sampler smp [[sampler(0)]])
{
    main0_out out = {};
    out.frag_color = float4(src_tex.sample(smp, in.v_uv).xyz, 1.0);
    return out;
}

