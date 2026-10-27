// Test patterns + statistics overlay for Metal (compiled at runtime with newLibraryWithSource).
// Keep in sync with shaders/pattern.frag, shaders/pattern.hlsl and src/common/patterns.cpp.
#include <metal_stdlib>
using namespace metal;

// Same 64-byte layout as vrr::ShaderParams.
struct Params {
    float2 resolution;
    float2 overlayPos;
    float2 overlaySize;
    float moveX;
    uint pattern;
    uint flags;
    uint frame;
    float2 imageSize;
    float4 pad;
};

constant float BAR_W = 64.0;
constant int COUNTER_BITS = 12;
constant float CELL = 24.0;
constant float GAP = 4.0;

struct VOut {
    float4 pos [[position]];
};

// Full-screen triangle without vertex buffers.
vertex VOut vsMain(uint id [[vertex_id]]) {
    VOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

static float3 patternColor(float2 p, constant Params& pc, texture2d<float> image, sampler smp) {
    float w = pc.resolution.x;
    float h = pc.resolution.y;
    if (pc.pattern == 0) return float3(0.5);
    if (pc.pattern == 1) return float3(0.2);
    if (pc.pattern == 2) return float3(1.0);
    if (pc.pattern == 3) return float3(0.0);
    if (pc.pattern == 4) return float3(0.35 + 0.65 * p.x / w);
    if (pc.pattern == 5) {
        int c = (int(p.x) / 64 + int(p.y) / 64) & 1;
        return float3(float(c));
    }
    if (pc.pattern == 6) {
        float period = w + BAR_W;
        float wx = pc.moveX;
        float bx = pc.moveX + period * 0.5;
        if (bx >= w) bx -= period;
        if (p.x >= wx && p.x < wx + BAR_W) return float3(1.0);
        if (p.x >= bx && p.x < bx + BAR_W) return float3(0.0);
        return float3(0.5);
    }
    if (pc.pattern == 7) {
        if ((pc.flags & 4u) == 0u) return float3(0.2, 0.0, 0.2);
        float s = min(w / pc.imageSize.x, h / pc.imageSize.y);
        float2 d = pc.imageSize * s;
        float2 o = (pc.resolution - d) * 0.5;
        float2 uv = (p - o) / d;
        if (any(uv < 0.0) || any(uv >= 1.0)) return float3(0.0);
        return image.sample(smp, uv, level(0)).rgb;
    }
    return float3(0.5);
}

fragment float4 fsMain(VOut in [[stage_in]], constant Params& pc [[buffer(0)]],
                       texture2d<float> overlay [[texture(0)]], texture2d<float> image [[texture(1)]],
                       sampler smp [[sampler(0)]]) {
    float2 p = in.pos.xy;
    float3 c = patternColor(p, pc, image, smp);

    if ((pc.flags & 2u) != 0u) {
        float cw = float(COUNTER_BITS) * (CELL + GAP) + GAP;
        float ch = CELL + 2.0 * GAP;
        float2 r0 = float2(pc.resolution.x - cw - 16.0, 16.0);
        float2 q = p - r0;
        if (q.x >= 0.0 && q.y >= 0.0 && q.x < cw && q.y < ch) {
            c = float3(0.0);
            float lx = q.x - GAP;
            float ly = q.y - GAP;
            if (lx >= 0.0 && ly >= 0.0 && ly < CELL) {
                int idx = int(lx / (CELL + GAP));
                float cx = lx - float(idx) * (CELL + GAP);
                if (idx < COUNTER_BITS && cx < CELL) {
                    uint bit = (pc.frame >> uint(COUNTER_BITS - 1 - idx)) & 1u;
                    c = bit != 0u ? float3(1.0) : float3(0.15);
                }
            }
        }
    }

    if ((pc.flags & 1u) != 0u) {
        float2 o = p - pc.overlayPos;
        if (o.x >= 0.0 && o.y >= 0.0 && o.x < pc.overlaySize.x && o.y < pc.overlaySize.y) {
            float4 t = overlay.read(uint2(o));
            c = mix(c, t.rgb, t.a);
        }
    }
    return float4(c, 1.0);
}
