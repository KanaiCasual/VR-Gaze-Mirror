// VR Gaze Mirror 2.0 - core: the shader of the one draw. The indicator looks are those of 1.x, drawn into
// the same pass that crops and scales the picture.
#pragma once

namespace gaze_mirror {

    constexpr char ShaderSource[] = R"_(
cbuffer C : register(b0)
{
    float4 sourceRect;   // u0, v0, du, dv in the source texture.
    float4 output;       // x = width / height of the output, y = source is linear light, z = ring on.
    float4 bounds;       // Output uv rectangle the ring can touch (x0, y0, x1, y1).
    // RingConstants:
    float2 center;       // In OUTPUT uv.
    float2 aspect;       // Scales output uv deltas into fractions of the EYE IMAGE height.
    float radius;
    float thickness;
    float feather;
    float fillOpacity;
    float4 color;        // rgb + opacity.
    float style;         // 0 ring, 1 glow, 2 dot, 3 spotlight, 4 bubble, 5 solid, 6 heatmap, 7 ghost.
    float shadowOpacity;
    float trailCount;
    float heatGain;
    float glowWidth;
    float tailOpacity;
    float premultiply;   // 1 = screen-like blending.
    float solidity;
    float glowStrength;
    float stretch;       // Width / height of the ring (1 = round).
    float2 padding;
    float4 trail[32];    // xy = position (output uv), z = weight. For ghost, trail[1] is the tip of the tail.
};
Texture2DArray source : register(t0);
SamplerState smooth : register(s0);

struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID)
{
    V o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float3 encode(float3 c)
{
    c = saturate(c);
    return lerp(12.92 * c, 1.055 * pow(c, 1.0 / 2.4) - 0.055, step(0.0031308, c));
}
float3 decode(float3 c)
{
    return lerp(c / 12.92, pow((c + 0.055) / 1.055, 2.4), step(0.04045, c));
}

// From a point of the indicator to a pixel, in fractions of the eye image height, with the ring's stretch taken out
// (so a round ring of `radius` in this space is an ellipse `stretch` times as wide on the picture).
float2 fromCenter(float2 uv, float2 at)
{
    float2 d = (uv - at) * aspect;
    d.x /= max(stretch, 0.01);
    return d;
}

float sdTaperedCapsule(float2 p, float2 pb, float ra, float rb)
{
    float h = dot(pb, pb);
    float2 q = float2(dot(p, float2(pb.y, -pb.x)), dot(p, pb)) / h;
    q.x = abs(q.x);
    float b = ra - rb;
    float2 c = float2(sqrt(max(h - b * b, 0.0)), b);
    float k = c.x * q.y - c.y * q.x;
    float m = dot(c, q);
    float n = dot(q, q);
    if (k < 0.0) return sqrt(h * n) - ra;
    else if (k > c.x) return sqrt(h * (n + 1.0 - 2.0 * q.y)) - rb;
    return m - ra;
}

float4 ghost(float2 uv)
{
    float2 p = fromCenter(uv, center);
    float2 tip = fromCenter(trail[1].xy, center);
    float tipRadius = radius * 0.03;
    float tailLength = length(tip);
    float glowReach = max(glowWidth, 0.0001);
    float2 tailDir = tailLength > 0.00001 ? tip / tailLength : float2(1.0, 0.0);
    float a = dot(p, tailDir);
    float b = dot(p, float2(-tailDir.y, tailDir.x));
    float deform = saturate(tailLength / (2.5 * radius));
    float squash = 1.0 - 0.12 * deform;
    float2 q = float2(a > 0.0 ? a / (1.0 + 0.35 * deform) : a / (1.0 - 0.08 * deform), b / squash);
    float dHead = length(q) - radius;
    float dDrop = dHead;
    float along = 0.0;
    float stretch = 0.0;
    float headRadius = radius * squash;
    if (tailLength > headRadius - tipRadius + 0.0005) {
        float dTail = sdTaperedCapsule(p, tip, headRadius, tipRadius);
        float k = radius * 0.35;
        float h = saturate(0.5 + 0.5 * (dTail - dHead) / k);
        dDrop = lerp(dTail, dHead, h) - k * h * (1.0 - h);
        along = saturate((a - radius) / max(tailLength - radius, 0.0001));
        stretch = saturate((tailLength - radius) / radius);
    }
    float facing = -a / max(length(p), 0.00001);
    float dissolve = saturate((tailLength - 0.5 * radius) / (1.5 * radius));
    float stroke = lerp(1.0, smoothstep(-0.8, 0.45, facing), dissolve);
    float halfThickness = thickness * 0.5;
    float edgeSoftness = max(min(feather, thickness) * 0.5, 0.00002);
    float ring = (1.0 - smoothstep(halfThickness - edgeSoftness, halfThickness + edgeSoftness, abs(dHead))) * stroke;
    float ringGlow = exp(-abs(dHead) / glowReach) * stroke;
    float outsideHead = smoothstep(-halfThickness, halfThickness, dHead);
    float behind = smoothstep(-0.35, 0.45, -facing);
    float tailFade = pow(1.0 - along, 0.8) * stretch * outsideHead * behind;
    float tailSoft = max(glowReach, 0.006);
    float tailBody = (1.0 - smoothstep(-tailSoft, tailSoft * 0.5, dDrop)) * tailFade * tailOpacity;
    float tailGlow = exp(-max(dDrop, 0.0) / tailSoft) * tailFade;
    float inside = (1.0 - smoothstep(-feather, feather, dHead)) * fillOpacity;
    float alpha = max(max(ring, tailBody) * color.a, max(ringGlow * glowStrength, tailGlow * 0.35) * color.a);
    return float4(color.rgb, max(alpha, inside));
}

float blobWithTrail(float2 uv, bool bubble)
{
    float result = 0.0;
    int count = (int)trailCount;
    [loop] for (int i = 0; i < 32; i++) {
        if (i >= count) break;
        float w = trail[i].z;
        float r = radius * (0.35 + 0.65 * w);
        float d = length(fromCenter(uv, trail[i].xy));
        float outer = 1.0 - smoothstep(r, r + feather, d);
        float inner = color.a;
        if (bubble) {
            float t = saturate(d / r);
            inner = (i == 0) ? lerp(fillOpacity, color.a, t * t * t) : lerp(fillOpacity, color.a, 0.35);
        }
        result = max(result, inner * outer * w * w);
    }
    return result;
}

float4 heatmap(float2 uv)
{
    float heat = 0.0;
    int count = (int)trailCount;
    [loop] for (int i = 0; i < 32; i++) {
        if (i >= count) break;
        float x = length(fromCenter(uv, trail[i].xy)) / max(radius, 0.0001);
        heat += trail[i].z * exp(-2.0 * x * x);
    }
    heat = saturate(heat * heatGain);
    float3 cold = float3(0.0, 0.25, 1.0);
    float3 mild = float3(0.0, 1.0, 0.3);
    float3 warm = float3(1.0, 0.85, 0.0);
    float3 hot = float3(1.0, 0.08, 0.0);
    float3 rgb = heat < 0.33 ? lerp(cold, mild, heat / 0.33)
               : heat < 0.66 ? lerp(mild, warm, (heat - 0.33) / 0.33)
                             : lerp(warm, hot, (heat - 0.66) / 0.34);
    return float4(rgb, color.a * smoothstep(0.03, 0.3, heat));
}

float4 withShadow(float fg, float shadow)
{
    float alpha = fg + shadow * (1.0 - fg);
    return float4(color.rgb * (fg / max(alpha, 0.0001)), alpha);
}

// The indicator's colour and coverage at this pixel (straight alpha).
float4 shade(float2 uv)
{
    float dist = length(fromCenter(uv, center));
    float shadowWidth = feather * 3.0 + 0.0015;
    if (style < 0.5) {
        float halfThickness = thickness * 0.5;
        float edge = abs(dist - radius);
        float edgeSoftness = max(min(feather, thickness) * 0.5, 0.00002);
        float ring = 1.0 - smoothstep(halfThickness - edgeSoftness, halfThickness + edgeSoftness, edge);
        float disc = 1.0 - smoothstep(radius, radius + feather, dist);
        float shadow = 1.0 - smoothstep(halfThickness, halfThickness + edgeSoftness + shadowWidth, edge);
        return withShadow(max(ring * color.a, disc * fillOpacity), shadow * shadowOpacity);
    } else if (style < 1.5) {
        float x = dist / max(radius, 0.0001);
        return float4(color.rgb, color.a * exp(-2.0 * x * x));
    } else if (style < 2.5) {
        float core = 1.0 - smoothstep(radius, radius + feather, dist);
        float shadow = 1.0 - smoothstep(radius, radius + feather + shadowWidth, dist);
        return withShadow(core * color.a, shadow * shadowOpacity);
    } else if (style > 3.5 && style < 4.5) {
        return float4(color.rgb, blobWithTrail(uv, true));
    } else if (style > 4.5 && style < 5.5) {
        return float4(color.rgb, blobWithTrail(uv, false));
    } else if (style > 5.5 && style < 6.5) {
        return heatmap(uv);
    } else if (style > 6.5) {
        return ghost(uv);
    }
    float outside = smoothstep(radius, radius + max(feather, radius), dist);
    return float4(0.0, 0.0, 0.0, color.a * outside);
}

float4 ps(V i) : SV_Target
{
    float3 scene = source.Sample(smooth, float3(sourceRect.xy + i.uv * sourceRect.zw, 0)).rgb;
    bool linearLight = output.y > 0.5;
    bool touched = output.z > 0.5 && (style > 2.5 && style < 3.5 ||
                   (i.uv.x >= bounds.x && i.uv.y >= bounds.y && i.uv.x <= bounds.z && i.uv.y <= bounds.w));
    if (touched) {
        float4 ring = shade(i.uv);
        // The configured colour is what the eye sees (sRGB); composite in the space the scene is in, as 1.x did.
        float3 rgb = linearLight ? decode(ring.rgb) : ring.rgb;
        if (premultiply > 0.5) {
            // Screen-like: src * (1 - dest) + dest * (1 - alpha * solidity) - behaves like light, never hides the scene.
            scene = rgb * ring.a * (1.0 - scene) + scene * (1.0 - ring.a * solidity);
        } else {
            scene = lerp(scene, rgb, ring.a);
        }
    }
    if (linearLight) scene = encode(scene);
    return float4(scene, 1);
}

// 4 x 4 box filter for the crop tool's small picture.
float4 psSnapshot(V i) : SV_Target
{
    float2 texel = sourceRect.zw / output.zw; // zw = snapshot size here.
    float3 sum = 0;
    [unroll] for (int y = 0; y < 4; y++) {
        [unroll] for (int x = 0; x < 4; x++) {
            float2 uv = sourceRect.xy + i.uv * sourceRect.zw + (float2(x, y) - 1.5) * texel * 0.25;
            sum += source.Sample(smooth, float3(uv, 0)).rgb;
        }
    }
    float3 c = sum / 16.0;
    if (output.y > 0.5) c = encode(c);
    return float4(c, 1);
}
)_";

} // namespace gaze_mirror
