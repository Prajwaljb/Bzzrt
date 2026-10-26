#version 450
// Test patterns + statistics overlay. Keep in sync with src/win/shader_hlsl.h
// and src/common/patterns.cpp (CPU version for the KMS backend).

layout(push_constant) uniform Params {
    vec2 resolution;
    vec2 overlayPos;
    vec2 overlaySize;
    float moveX;
    uint pattern;
    uint flags;
    uint frame;
    vec2 imageSize;
    vec4 pad;
} pc;

layout(set = 0, binding = 0) uniform sampler2D overlayTex;
layout(set = 0, binding = 1) uniform sampler2D imageTex;

layout(location = 0) out vec4 outColor;

const float BAR_W = 64.0;
const int COUNTER_BITS = 12;
const float CELL = 24.0;
const float GAP = 4.0;

vec3 patternColor(vec2 p) {
    float w = pc.resolution.x;
    float h = pc.resolution.y;
    if (pc.pattern == 0u) return vec3(0.5);
    if (pc.pattern == 1u) return vec3(0.2);
    if (pc.pattern == 2u) return vec3(1.0);
    if (pc.pattern == 3u) return vec3(0.0);
    if (pc.pattern == 4u) return vec3(0.35 + 0.65 * p.x / w);
    if (pc.pattern == 5u) {
        int c = (int(p.x) / 64 + int(p.y) / 64) & 1;
        return vec3(float(c));
    }
    if (pc.pattern == 6u) {
        float period = w + BAR_W;
        float wx = pc.moveX;
        float bx = pc.moveX + period * 0.5;
        if (bx >= w) bx -= period;
        if (p.x >= wx && p.x < wx + BAR_W) return vec3(1.0);
        if (p.x >= bx && p.x < bx + BAR_W) return vec3(0.0);
        return vec3(0.5);
    }
    if (pc.pattern == 7u) {
        if ((pc.flags & 4u) == 0u) return vec3(0.2, 0.0, 0.2);
        float s = min(w / pc.imageSize.x, h / pc.imageSize.y);
        vec2 d = pc.imageSize * s;
        vec2 o = (pc.resolution - d) * 0.5;
        vec2 uv = (p - o) / d;
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThanEqual(uv, vec2(1.0)))) return vec3(0.0);
        return texture(imageTex, uv).rgb;
    }
    return vec3(0.5);
}

void main() {
    vec2 p = gl_FragCoord.xy;
    vec3 c = patternColor(p);

    if ((pc.flags & 2u) != 0u) {
        float cw = float(COUNTER_BITS) * (CELL + GAP) + GAP;
        float ch = CELL + 2.0 * GAP;
        vec2 r0 = vec2(pc.resolution.x - cw - 16.0, 16.0);
        vec2 q = p - r0;
        if (q.x >= 0.0 && q.y >= 0.0 && q.x < cw && q.y < ch) {
            c = vec3(0.0);
            float lx = q.x - GAP;
            float ly = q.y - GAP;
            if (lx >= 0.0 && ly >= 0.0 && ly < CELL) {
                int i = int(lx / (CELL + GAP));
                float cx = lx - float(i) * (CELL + GAP);
                if (i < COUNTER_BITS && cx < CELL) {
                    uint bit = (pc.frame >> uint(COUNTER_BITS - 1 - i)) & 1u;
                    c = bit != 0u ? vec3(1.0) : vec3(0.15);
                }
            }
        }
    }

    if ((pc.flags & 1u) != 0u) {
        vec2 o = p - pc.overlayPos;
        if (o.x >= 0.0 && o.y >= 0.0 && o.x < pc.overlaySize.x && o.y < pc.overlaySize.y) {
            vec4 t = texelFetch(overlayTex, ivec2(o), 0);
            c = mix(c, t.rgb, t.a);
        }
    }

    if ((pc.flags & 8u) != 0u) {
        // sRGB swap-chain format: hardware encodes, so hand it linear values.
        c = mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
    }
    outColor = vec4(c, 1.0);
}
