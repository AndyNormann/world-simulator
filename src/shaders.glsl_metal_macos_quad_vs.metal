#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct main0_out
{
    float2 v_uv [[user(locn0)]];
    float4 gl_Position [[position]];
};

vertex main0_out main0(uint gl_VertexIndex [[vertex_id]])
{
    main0_out out = {};
    float _18 = float((int(gl_VertexIndex) << 1) & 2);
    float _21 = float(int(gl_VertexIndex) & 2);
    out.v_uv = float2(_18, 1.0 - _21);
    out.gl_Position = float4((float2(_18, _21) * 2.0) - float2(1.0), 0.0, 1.0);
    return out;
}

