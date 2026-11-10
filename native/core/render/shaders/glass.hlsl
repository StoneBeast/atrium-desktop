// Atrium glass material. One signed-distance shape; Liquid and Frosted are
// parameter presets (docs/DESIGN.md, material presets).

cbuffer GlassParams : register(b0)
{
    float2 Size;        // window size in px
    float2 RoiOrigin;   // window origin inside the background ROI texture
    float2 RoiSize;     // ROI texture size
    float  Radius;      // corner radius in px
    float  Edge;        // width of the refracting rim in px
    float  Refract;     // max sample displacement in px
    float  Dispersion;  // relative channel spread of the displacement
    float  BlurLod;     // mip level used for the blurred backdrop
    float  TintMix;
    float4 Tint;
    float  Highlight;
    float  Shadow;
    float2 LightDir;    // unit vector pointing towards the light
    float  Inset;       // shadow room around the shape (sets the shadow's softness)
    float  BlurMix;     // 0 = sharp backdrop, 1 = fully blurred
    float  Fade;        // 0 = the glass as is, 1 = gone (a piece of glass fading out)
    float  Pad0;
    float4 Shape;       // the glass shape in window px: left, top, right, bottom
    float4 Ripple;      // press ripple: centre x, y (window px), ring radius, strength (0 = none)
    float4 PartRect[6]; // melting parts (PartCount > 0): rectangles in window px, left, top, right, bottom
    float4 PartRadii[6]; // their corner radii: top left, top right, bottom right, bottom left
    float  PartCount;
    float  Melt;        // width of the smooth union in px
    float  PartMix;     // 0: the parts, 1: the rounded rectangle Shape (the parts' union has seams inside where they meet)
    float  Pad1;
    float4 Tail;        // Dock label pointer: tip x, base half width, height below Shape.w, overlap inside the body
    float4 Wave;        // Dock hover: centre x, support half width, upward height, unused (px)
};

Texture2D Bg : register(t0);
SamplerState Linear : register(s0);

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut VS(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

// Shared with corner_profile.h / CornerProfile.cs. All rounded rectangles,
// including pills, retain quartic straight-edge joins (k = dk/ds = 0).
float2 cornerProfile(float2 b, float r)
{
    r = clamp(r, 0, min(b.x, b.y));
    float reach = min(r * 1.6, min(b.x, b.y));
    float v = 1 - 0.2928932188134525 * r / max(reach, 1e-12);
    float v2 = v * v;
    float a = b.x == b.y && r == b.x ? 0 : max(0.02, (2 * v2 * v2 - v2) / max(1 - v2, 1e-12));
    return float2(reach, a);
}

float cornerEdge(float2 q, float2 profile)
{
    float r = profile.x, a = profile.y;
    if (r <= 0 || min(q.x, q.y) <= 0) return max(q.x, q.y) - r;
    float2 v = q / r, v2 = v * v;
    float2 den = a + (1 - a) * v2;
    float2 derivative = 2 * v * v2 * (2 * a + (1 - a) * v2) / (den * den);
    return (dot(v2 * v2 / den, 1) - 1) * r / max(length(derivative), 1e-12);
}

// Approximate metric distance only for refraction/shading. Coverage below is
// evaluated directly from the analytic implicit equation, not this iteration.
float sdCorner(float2 q, float r, float a)
{
    if (min(q.x, q.y) <= 0)
        return max(q.x, q.y) - r;
    if (r <= 0)
        return length(q);
    float n = length(q);
    float2 v, derivative;
    [unroll] for (int i = 0; i < 2; ++i) {
        v = q / n;
        float2 v2 = v * v;
        float2 denominator = a + (1 - a) * v2;
        float2 f = v2 * v2 / denominator;
        derivative = 2 * v * v2 * (2 * a + (1 - a) * v2) / (denominator * denominator);
        n *= 1 + (f.x + f.y - 1) / dot(v, derivative);
    }
    return (n - r) * dot(v, derivative) / length(derivative);
}

float sdRoundRect(float2 p, float2 b, float r, bool edge)
{
    float2 profile = cornerProfile(b, r);
    float2 q = abs(p) - b + profile.x;
    return edge ? cornerEdge(q, profile) : sdCorner(q, profile.x, profile.y);
}

// A rounded rectangle whose corners differ (r: top left, top right, bottom right, bottom left; y down).
float sdRoundRect4(float2 p, float2 b, float4 r, bool edge)
{
    float rr = p.x < 0 ? (p.y < 0 ? r.x : r.w) : (p.y < 0 ? r.y : r.z);
    rr = min(rr, min(b.x, b.y));
    return sdRoundRect(p, b, rr, edge);
}

// Smooth minimum: two shapes melt where they come within k px of each other.
float smin(float a, float b, float k)
{
    if (a - b <= -k) return a;
    if (a - b >= k) return b;
    float x = (a - b) / k, x2 = x * x;
    // C3 at both ends, with the original blend width and k/4 centre depth.
    float p = (8 + x2 * (3 + x2 * (13 + x2 * (-11 + 3 * x2)))) / 32;
    return (a + b) * 0.5 - k * p;
}

// Signed distance to the glass shape at window px `px` (negative inside).
float sdTail(float2 px)
{
    float width = Shape.z - Shape.x;
    float margin = max(0.5, Tail.w * 0.25);
    float halfWidth = min(Tail.y, max(0.5, width * 0.5 - margin));
    float centre = clamp(Tail.x, Shape.x + halfWidth + margin, max(Shape.x + halfWidth + margin, Shape.z - halfWidth - margin));
    float2 a = float2(centre - halfWidth, Shape.w - Tail.w);
    float2 b = float2(centre + halfWidth, a.y), c = float2(Tail.x, Shape.w + Tail.z);
    float2 e0 = b - a, e1 = c - b, e2 = a - c;
    float2 v0 = px - a, v1 = px - b, v2 = px - c;
    float2 q0 = v0 - e0 * saturate(dot(v0, e0) / max(dot(e0, e0), 1e-12));
    float2 q1 = v1 - e1 * saturate(dot(v1, e1) / max(dot(e1, e1), 1e-12));
    float2 q2 = v2 - e2 * saturate(dot(v2, e2) / max(dot(e2, e2), 1e-12));
    float distance = sqrt(min(dot(q0, q0), min(dot(q1, q1), dot(q2, q2))));
    bool inside = e0.x * v0.y - e0.y * v0.x >= 0 && e1.x * v1.y - e1.y * v1.x >= 0 && e2.x * v2.y - e2.y * v2.x >= 0;
    return inside ? -distance : distance;
}

float ShapeDistance(float2 px, bool edge)
{
    // A C3 bump stretches the glass upwards while keeping its bottom fixed.
    if (Wave.z > 0) {
        float x = (px.x - Wave.x) / max(Wave.y, 1);
        float w = saturate(1 - x * x);
        float lift = Wave.z * w * w * w * w;
        float height = max(Shape.w - Shape.y, 1);
        px.y = Shape.w - (Shape.w - px.y) * height / (height + lift);
    }
    // C3 metric fields are used while shapes merge. The isolated outline
    // uses the exact implicit equation below; a blend needs a smooth field.
    edge = edge && PartCount < 0.5;
    float2 c = px - (Shape.xy + Shape.zw) * 0.5;
    float2 b = max((Shape.zw - Shape.xy) * 0.5, 1);
    float r = min(Radius, min(b.x, b.y));
    float d = sdRoundRect(c, b, r, edge);
    if (PartCount > 0.5) {
        float card = d;
        d = 1e6;
        [unroll] for (int i = 0; i < 6; ++i) {
            if (float(i) < PartCount) {
                float4 pr = PartRect[i];
                float di = sdRoundRect4(px - (pr.xy + pr.zw) * 0.5, max((pr.zw - pr.xy) * 0.5, 1), PartRadii[i], edge);
                d = Melt > 0.01 ? smin(d, di, Melt) : min(d, di);
            }
        }
        d = lerp(d, card, PartMix);
    }
    if (Tail.z > 0)
        d = min(d, sdTail(px));
    return d;
}

float Dist(float2 px) { return ShapeDistance(px, false); }

float3 bg(float2 px, float lod)
{
    return Bg.SampleLevel(Linear, (px + RoiOrigin) / RoiSize, lod).rgb;
}

float3 blurred(float2 px)
{
    // A few taps at a coarse mip hide the mip's box filter structure.
    float s = exp2(BlurLod) * 0.75;
    float3 c = bg(px, BlurLod) * 0.4;
    c += bg(px + float2(s, 0), BlurLod) * 0.15;
    c += bg(px - float2(s, 0), BlurLod) * 0.15;
    c += bg(px + float2(0, s), BlurLod) * 0.15;
    c += bg(px - float2(0, s), BlurLod) * 0.15;
    return c;
}

float4 PS(VSOut i) : SV_Target
{
    float2 px = i.uv * Size;
    float d = Dist(px);

    // Soft drop shadow outside the shape.
    float ds = Dist(px - float2(0, Inset * 0.25));
    float shadowA = Shadow * exp(-max(ds, 0) / max(Inset * 0.35, 1)) * step(0, d);

    // The implicit edge distance is exact only near the outline: where a corner
    // meets a straight side, deep inside, it jumps by hundreds of px between
    // neighbours, and fwidth would leave a half-covered line there. Past 2 px in
    // the shape is plainly covered.
    float edgeDistance = ShapeDistance(px, true);
    float aa = max(fwidth(edgeDistance), 1e-3);
    float cov = d < -2 ? 1 : saturate(0.5 - edgeDistance / aa);
    float keep = saturate(1 - Fade);
    shadowA *= keep;
    if (cov <= 0)
        return float4(0, 0, 0, shadowA);

    float e = 1.0;
    float2 g = float2(Dist(px + float2(e, 0)) - Dist(px - float2(e, 0)), Dist(px + float2(0, e)) - Dist(px - float2(0, e)));
    float gl = length(g);
    float2 n = gl > 1e-5 ? g / gl : float2(0, 0);

    float depth = max(-d, 0);
    float t = saturate(1 - depth / max(Edge, 1));
    float profile = 1 - sqrt(saturate(1 - t * t)); // circular lens cross-section
    float2 off = -n * Refract * profile;

    // Press ripple: a ring of displacement running outwards from the point that
    // was pressed, as if the glass gave way under the finger and sprang back.
    float ring = 0;
    if (Ripple.w > 0) {
        float2 dv = px - Ripple.xy;
        float dl = length(dv);
        float w = max(Ripple.z * 0.30, 6);
        float x = (dl - Ripple.z) / w;
        ring = exp(-x * x) * Ripple.w;
        float2 dir = dl > 1e-3 ? dv / dl : float2(0, 0);
        off += dir * ring * (6 + Refract * 0.35);
        off -= dv * 0.06 * Ripple.w * saturate(1 - dl / max(Ripple.z, 1)); // the dent inside the ring
    }

    float3 sharp;
    sharp.r = bg(px + off * (1 + Dispersion), 0).r;
    sharp.g = bg(px + off, 0).g;
    sharp.b = bg(px + off * (1 - Dispersion), 0).b;
    float3 soft = blurred(px + off);
    float3 col = lerp(sharp, soft, BlurMix);

    // Vibrancy: content under the glass reads a little richer and brighter,
    // as on macOS, before the tint is mixed in.
    float luma = dot(col, float3(0.2126, 0.7152, 0.0722));
    col = lerp(luma.xxx, col, 1.0 + 0.22 * Highlight) * (1.0 + 0.04 * Highlight);

    col = lerp(col, Tint.rgb, TintMix);

    // Light: a crisp specular line along the rim, strongest facing the light,
    // a softer glow just inside it, and a faint sheen from the top.
    float lit = saturate(dot(n, LightDir));
    float back = saturate(dot(n, -LightDir));
    float rimLine = 1 - smoothstep(0.0, 1.6, depth);
    float glow = saturate(1 - depth / (Edge * 1.6));
    col += Highlight * rimLine * (0.22 + 0.78 * lit + 0.35 * back);
    col += Highlight * glow * glow * lit * 0.16;
    float sheen = saturate(1 - (px.y - Shape.y) / max(Shape.w - Shape.y, 1));
    col += Highlight * 0.045 * sheen * sheen;

    // Depth: a very light shade in the band where the lens bends the most.
    float band = smoothstep(0.0, Edge * 0.5, depth) * (1 - smoothstep(Edge * 0.5, Edge * 1.3, depth));
    col *= 1 - 0.05 * band * (1 - lit);
    col += ring * 0.10 * (0.4 + Highlight);

    float4 glass = float4(saturate(col) * cov, cov) * keep;
    return glass + (1 - cov) * float4(0, 0, 0, shadowA);
}
