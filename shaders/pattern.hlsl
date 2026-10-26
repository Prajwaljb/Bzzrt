// Test patterns + statistics overlay for D3D12 / D3D11 (compiled at runtime with D3DCompile).
// Keep in sync with shaders/pattern.frag and src/common/patterns.cpp.

cbuffer Params : register(b0) {
    float2 gResolution;
    float2 gOverlayPos;
    float2 gOverlaySize;
    float gMoveX;
    uint gPattern;
    uint gFlags;
    uint gFrame;
    float2 gImageSize;
    float4 gPad;
};

Texture2D<float4> gOverlay : register(t0);
Texture2D<float4> gImage : register(t1);
SamplerState gLinear : register(s0);

static const float BAR_W = 64.0;
static const int COUNTER_BITS = 12;
static const float CELL = 24.0;
static const float GAP = 4.0;

struct VSOut {
    float4 pos : SV_Position;
};

VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float3 patternColor(float2 p) {
    float w = gResolution.x;
    float h = gResolution.y;
    if (gPattern == 0) return float3(0.5, 0.5, 0.5);
    if (gPattern == 1) return float3(0.2, 0.2, 0.2);
    if (gPattern == 2) return float3(1.0, 1.0, 1.0);
    if (gPattern == 3) return float3(0.0, 0.0, 0.0);
    if (gPattern == 4) return (0.35 + 0.65 * p.x / w).xxx;
    if (gPattern == 5) {
        int c = ((int)(p.x) / 64 + (int)(p.y) / 64) & 1;
        return ((float)c).xxx;
    }
    if (gPattern == 6) {
        float period = w + BAR_W;
        float wx = gMoveX;
        float bx = gMoveX + period * 0.5;
        if (bx >= w) bx -= period;
        if (p.x >= wx && p.x < wx + BAR_W) return float3(1.0, 1.0, 1.0);
        if (p.x >= bx && p.x < bx + BAR_W) return float3(0.0, 0.0, 0.0);
        return float3(0.5, 0.5, 0.5);
    }
    if (gPattern == 7) {
        if ((gFlags & 4) == 0) return float3(0.2, 0.0, 0.2);
        float s = min(w / gImageSize.x, h / gImageSize.y);
        float2 d = gImageSize * s;
        float2 o = (gResolution - d) * 0.5;
        float2 uv = (p - o) / d;
        if (any(uv < 0.0) || any(uv >= 1.0)) return float3(0.0, 0.0, 0.0);
        return gImage.SampleLevel(gLinear, uv, 0).rgb;
    }
    return float3(0.5, 0.5, 0.5);
}

float4 PSMain(VSOut i) : SV_Target {
    float2 p = i.pos.xy;
    float3 c = patternColor(p);

    if ((gFlags & 2) != 0) {
        float cw = (float)COUNTER_BITS * (CELL + GAP) + GAP;
        float ch = CELL + 2.0 * GAP;
        float2 r0 = float2(gResolution.x - cw - 16.0, 16.0);
        float2 q = p - r0;
        if (q.x >= 0.0 && q.y >= 0.0 && q.x < cw && q.y < ch) {
            c = float3(0.0, 0.0, 0.0);
            float lx = q.x - GAP;
            float ly = q.y - GAP;
            if (lx >= 0.0 && ly >= 0.0 && ly < CELL) {
                int idx = (int)(lx / (CELL + GAP));
                float cx = lx - (float)idx * (CELL + GAP);
                if (idx < COUNTER_BITS && cx < CELL) {
                    uint bit = (gFrame >> (uint)(COUNTER_BITS - 1 - idx)) & 1;
                    c = bit != 0 ? float3(1.0, 1.0, 1.0) : float3(0.15, 0.15, 0.15);
                }
            }
        }
    }

    if ((gFlags & 1) != 0) {
        float2 o = p - gOverlayPos;
        if (o.x >= 0.0 && o.y >= 0.0 && o.x < gOverlaySize.x && o.y < gOverlaySize.y) {
            float4 t = gOverlay.Load(int3((int2)o, 0));
            c = lerp(c, t.rgb, t.a);
        }
    }
    return float4(c, 1.0);
}
