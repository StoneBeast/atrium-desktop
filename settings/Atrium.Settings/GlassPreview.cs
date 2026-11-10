using System.Numerics;
using System.Runtime.Intrinsics;
using System.Runtime.CompilerServices;

namespace Atrium.Settings;

// CPU port of the glass pixel shader, used by the live specimen in the settings window.
//
// Source of truth: native/core/render/shaders/glass.hlsl (PS) for the per-pixel maths and
// ParamsFor() in native/core/bar.cpp for the parameter mapping. When either changes, change
// this file too. Differences from the GPU path, all deliberate:
//   - fwidth(d) is taken from the analytic gradient (|n.x| + |n.y|) instead of quad derivatives;
//   - the blur mip level is offset by log2(scale) so a bar drawn at 62 % blurs 62 % as far
//     (on screen the lod is relative to device pixels, here to the scaled-down specimen);
//   - the "screen" is the specimen photo (Photo.cs) with a CPU-built mip chain.

// The photo behind the glass, level 0 plus a box-filtered mip chain (what GenerateMips gives the shader).
internal sealed class Backdrop
{
    public readonly int Width, Height;
    public readonly byte[][] Mips;    // BGRA8, as the native renderer's UNORM texture
    public readonly int[] MipW, MipH;
    public byte[] Base => Mips[0];

    private Backdrop(int w, int h, byte[][] mips, int[] mw, int[] mh)
    {
        Width = w; Height = h; Mips = mips; MipW = mw; MipH = mh;
    }

    // The photo for a pw x ph pixel viewport onto an art of artW x artH DIP whose top-left
    // visible point is (x0, y0) DIP, at ppd pixels per DIP.
    public static Backdrop Create(int pw, int ph, double artW, double artH, double x0, double y0, double ppd, Photo? photo = null)
    {
        var l0 = new byte[pw * ph * 4];
        (photo ?? Photo.Default).Paint(l0, pw, ph, (float)artW, (float)artH, (float)x0, (float)y0, (float)ppd);

        var mips = new List<byte[]> { l0 };
        var mw = new List<int> { pw };
        var mh = new List<int> { ph };
        while (mw[^1] > 1 || mh[^1] > 1)
        {
            var src = mips[^1];
            int sw = mw[^1], sh = mh[^1];
            int w = Math.Max(1, (sw + 1) / 2), h = Math.Max(1, (sh + 1) / 2);
            var dst = new byte[w * h * 4];
            void Row(int y)
            {
                int y0r = Math.Min(2 * y, sh - 1), y1r = Math.Min(2 * y + 1, sh - 1);
                for (int x = 0; x < w; x++)
                {
                    int x0r = Math.Min(2 * x, sw - 1), x1r = Math.Min(2 * x + 1, sw - 1);
                    int a = (y0r * sw + x0r) * 4, b = (y0r * sw + x1r) * 4, c = (y1r * sw + x0r) * 4, d = (y1r * sw + x1r) * 4;
                    int o = (y * w + x) * 4;
                    dst[o] = (byte)((src[a] + src[b] + src[c] + src[d] + 2) / 4);
                    dst[o + 1] = (byte)((src[a + 1] + src[b + 1] + src[c + 1] + src[d + 1] + 2) / 4);
                    dst[o + 2] = (byte)((src[a + 2] + src[b + 2] + src[c + 2] + src[d + 2] + 2) / 4);
                    dst[o + 3] = 255;
                }
            }
            if (w * h > 4096) Parallel.For(0, h, Row);
            else for (int y = 0; y < h; y++) Row(y);
            mips.Add(dst); mw.Add(w); mh.Add(h);
        }

        return new Backdrop(pw, ph, mips.ToArray(), mw.ToArray(), mh.ToArray());
    }
}

// GlassParams of glass.hlsl, in pixels of the target bitmap.
internal readonly record struct GlassSpec(
    float Radius, float Edge, float Refract, float Dispersion, float BlurLod, float BlurMix,
    float TintMix, Vector3 Tint, float Highlight, float Shadow, float Inset, float Rim = 1,
    float WaveX = 0, float WaveWidth = 0, float WaveHeight = 0)
{
    public const double InsetDip = 14; // kInsetDip in bar.cpp: room for the drop shadow
    public int RiseInset => (int)MathF.Ceiling(WaveHeight); // extra room above the base shape

    // Mirror of ParamsFor() in bar.cpp. `scale` = target pixels per real DIP of the bar.
    public static GlassSpec For(BarSettings s, double scale)
    {
        float tr = ((s.Tint >> 16) & 0xFF) / 255f, tg = ((s.Tint >> 8) & 0xFF) / 255f, tb = (s.Tint & 0xFF) / 255f;
        return new GlassSpec(
            Radius: (float)(s.CornerRadius * scale),
            Edge: (float)(s.Edge * scale),
            Refract: (float)(s.Refraction * scale),
            Dispersion: (float)s.Dispersion,
            BlurLod: Math.Max(0f, (float)(1.0 + 4.5 * s.Blur + Math.Log2(scale))),
            BlurMix: (float)Math.Min(1.0, s.Blur * 4.0),
            TintMix: (float)(1.0 - s.Transparency),
            Tint: new Vector3(tr, tg, tb),
            Highlight: (float)s.Highlight,
            Shadow: (float)s.Shadow,
            Inset: (float)Math.Round(InsetDip * scale));
    }
}

// One preview tile in stage DIP; the optional last tile is the bar below the widgets.
internal readonly record struct GlassTile(double X, double Y, double W, double H, double Radius);
internal sealed record GlassOverlay(int Width, int Height, byte[] Pixels, float Luma);

internal static class GlassRenderer
{
    // Each moving card owns transparent glass and its shadow, without pixels from the card in front.
    // scale > 0: tiles of a scaled-down desktop (the widget map), the glass scaled with them as DrawTiles does.
    public static GlassOverlay[] CardOverlays(Backdrop bg, BarSettings material, double rs, IReadOnlyList<GlassTile> tiles, double scale = 0)
    {
        var result = new GlassOverlay[tiles.Count];
        for (int i = 0; i < tiles.Count; i++)
        {
            var t = tiles[i];
            var spec = scale > 0 ? GlassSpec.For(material, scale * rs) with { Radius = (float)(t.Radius * rs) }
                                 : GlassSpec.For(material, rs) with { Radius = (float)(t.Radius * rs), Rim = 0.12f };
            int inset = (int)spec.Inset, w = (int)Math.Round(t.W * rs), h = (int)Math.Round(t.H * rs);
            int pw = w + 2 * inset, ph = h + 2 * inset;
            var px = new byte[pw * ph * 4];
            DrawOverlay(bg, px, (int)Math.Round(t.X * rs) - inset, (int)Math.Round(t.Y * rs) - inset, pw, ph, spec);
            result[i] = new GlassOverlay(pw, ph, px, MeanLuma(px, pw, inset, inset, inset + w, inset + h));
        }
        return result;
    }
    private static readonly Vector2 Light = new(-0.6f, -0.8f);

    public static float[] DrawTiles(Backdrop bg, byte[] pixels, BarSettings material, double scale, double rs,
                                    IReadOnlyList<GlassTile> tiles, GlassTile? bar)
    {
        Buffer.BlockCopy(bg.Base, 0, pixels, 0, pixels.Length);
        var lumas = new float[tiles.Count];
        var spec = GlassSpec.For(material, scale * rs);
        int inset = (int)spec.Inset;
        for (int i = 0; i < tiles.Count + (bar.HasValue ? 1 : 0); i++)
        {
            var t = i < tiles.Count ? tiles[i] : bar!.Value;
            // Keep the same DIP-to-pixel arithmetic as GlassSpec.For with this tile's radius.
            var shape = spec with { Radius = (float)((t.Radius / scale) * (scale * rs)) };
            int x = (int)Math.Round(t.X * rs), y = (int)Math.Round(t.Y * rs);
            int w = (int)Math.Round(t.W * rs), h = (int)Math.Round(t.H * rs);
            Draw(bg, pixels, x - inset, y - inset, w + 2 * inset, h + 2 * inset, shape);
            if (i < tiles.Count)
                lumas[i] = MeanLuma(pixels, bg.Width, Math.Clamp(x, 0, bg.Width - 1), Math.Clamp(y, 0, bg.Height - 1),
                                   Math.Clamp(x + w, 1, bg.Width), Math.Clamp(y + h, 1, bg.Height));
        }
        return lumas;
    }

    // Draws the glass window (winW x winH px, top-left at winX,winY; shape inset by p.Inset)
    // over `dst`, which must already hold the backdrop (bg.Base). BGRA8, opaque.
    public static void Draw(Backdrop bg, byte[] dst, int winX, int winY, int winW, int winH, GlassSpec p)
    {
        int y0 = Math.Max(0, -winY), y1 = Math.Min(winH, bg.Height - winY);
        if (y1 <= y0 || winW <= 0) return;
        Parallel.For(y0, y1, ly => Row(bg, dst, bg.Width, winX, winY, false, winX, winY, winW, winH, p, ly));
    }

    // The same glass alone: `dst` is the window (winW x winH), cleared to transparent, and gets
    // premultiplied BGRA (the shadow is black at its strength) to lay over the backdrop.
    public static void DrawOverlay(Backdrop bg, byte[] dst, int winX, int winY, int winW, int winH, GlassSpec p)
    {
        int y0 = Math.Max(0, -winY), y1 = Math.Min(winH, bg.Height - winY);
        if (y1 <= y0 || winW <= 0) return;
        Parallel.For(y0, y1, ly => Row(bg, dst, winW, 0, 0, true, winX, winY, winW, winH, p, ly));
    }

    [MethodImpl(MethodImplOptions.AggressiveOptimization)]
    private static void Row(Backdrop bg, byte[] dst, int stride, int dstX, int dstY, bool overlay, int winX, int winY, int winW, int winH,
                            GlassSpec p, int ly)
    {
        float W = winW, H = winH, inset = p.Inset;
        float bx = W * 0.5f - inset, by = (H - p.RiseInset) * 0.5f - inset;
        float r = MathF.Min(p.Radius, MathF.Min(bx, by));
        var corner = CornerProfile.For(bx * 2, by * 2, r);
        float py = ly + 0.5f, cy = py - (H + p.RiseInset) * 0.5f;
        float shadowK = 1f / MathF.Max(inset * 0.35f, 1f);
        float edge = MathF.Max(p.Edge, 1f);
        float blurStep = MathF.Pow(2f, p.BlurLod) * 0.75f;
        float sheenT = Sat(1 - (py - inset - p.RiseInset) / MathF.Max(2 * by, 1f));
        float sheen = p.Highlight * 0.045f * sheenT * sheenT;
        int xs = Math.Max(0, -winX), xe = Math.Min(winW, bg.Width - winX);
        int rowBase = (dstY + ly) * stride + dstX;
        bool needSharp = p.BlurMix < 1f, needSoft = p.BlurMix > 0f;

        for (int lx = xs; lx < xe; lx++)
        {
            float px = lx + 0.5f, cx = px - W * 0.5f;
            float y = WaveY(cx, cy, by, p);
            float d = Sd(cx, y, bx, by, r);
            float ds = Sd(cx, WaveY(cx, cy - inset * 0.25f, by, p), bx, by, r);
            float shadowA = d >= 0 ? p.Shadow * MathF.Exp(-MathF.Max(ds, 0) * shadowK) : 0;
            int di = (rowBase + lx) * 4;

            if (d > 1.5f)
            {
                if (shadowA > 0.002f)
                {
                    if (overlay) { dst[di + 3] = To8(shadowA); continue; }
                    float k = 1 - shadowA;
                    dst[di] = (byte)(dst[di] * k + 0.5f);
                    dst[di + 1] = (byte)(dst[di + 1] * k + 0.5f);
                    dst[di + 2] = (byte)(dst[di + 2] * k + 0.5f);
                }
                continue;
            }

            const float e = 1f;
            float yr = WaveY(cx + e, cy, by, p), yl = WaveY(cx - e, cy, by, p);
            float yd = WaveY(cx, cy + e, by, p), yu = WaveY(cx, cy - e, by, p);
            float gx = Sd(cx + e, yr, bx, by, r) - Sd(cx - e, yl, bx, by, r);
            float gy = Sd(cx, yd, bx, by, r) - Sd(cx, yu, bx, by, r);
            float gl = MathF.Sqrt(gx * gx + gy * gy);
            float nx = 0, ny = 0;
            if (gl > 1e-5f) { nx = gx / gl; ny = gy / gl; }

            float aa = MathF.Max(MathF.Abs(nx) + MathF.Abs(ny), 1e-3f); // fwidth(d)
            float edgeDistance = corner.Edge(MathF.Abs(cx) - bx + corner.Reach, MathF.Abs(y) - by + corner.Reach);
            if (p.WaveHeight > 0 && d >= -2)
            {
                // The deformed outline needs the implicit field's screen-space gradient.
                float ex = corner.Edge(MathF.Abs(cx + e) - bx + corner.Reach, MathF.Abs(yr) - by + corner.Reach) -
                           corner.Edge(MathF.Abs(cx - e) - bx + corner.Reach, MathF.Abs(yl) - by + corner.Reach);
                float ey = corner.Edge(MathF.Abs(cx) - bx + corner.Reach, MathF.Abs(yd) - by + corner.Reach) -
                           corner.Edge(MathF.Abs(cx) - bx + corner.Reach, MathF.Abs(yu) - by + corner.Reach);
                aa = MathF.Max((MathF.Abs(ex) + MathF.Abs(ey)) / 2, 1e-3f);
            }
            float cov = p.WaveHeight > 0 && d < -2 ? 1 : Sat(0.5f - edgeDistance / aa);
            if (cov <= 0)
            {
                if (overlay) { dst[di + 3] = To8(shadowA); continue; }
                float k = 1 - shadowA;
                dst[di] = (byte)(dst[di] * k + 0.5f);
                dst[di + 1] = (byte)(dst[di + 1] * k + 0.5f);
                dst[di + 2] = (byte)(dst[di + 2] * k + 0.5f);
                continue;
            }

            float depth = MathF.Max(-d, 0);
            float t = Sat(1 - depth / edge);
            float profile = 1 - MathF.Sqrt(Sat(1 - t * t)); // circular lens cross-section
            float ox = -nx * p.Refract * profile, oy = -ny * p.Refract * profile;

            float ix = px + winX, iy = py + winY; // position in the backdrop
            Vector3 sharp = default, soft = default;
            if (needSharp)
            {
                if (ox == 0 && oy == 0)
                    sharp = Tex(bg, ix, iy, 0);
                else
                {
                    sharp.X = Tex(bg, ix + ox * (1 + p.Dispersion), iy + oy * (1 + p.Dispersion), 0).X;
                    sharp.Y = Tex(bg, ix + ox, iy + oy, 0).Y;
                    sharp.Z = Tex(bg, ix + ox * (1 - p.Dispersion), iy + oy * (1 - p.Dispersion), 0).Z;
                }
            }
            if (needSoft)
            {
                float qx = ix + ox, qy = iy + oy;
                soft = Tex(bg, qx, qy, p.BlurLod) * 0.4f
                     + (Tex(bg, qx + blurStep, qy, p.BlurLod) + Tex(bg, qx - blurStep, qy, p.BlurLod)
                      + Tex(bg, qx, qy + blurStep, p.BlurLod) + Tex(bg, qx, qy - blurStep, p.BlurLod)) * 0.15f;
            }
            Vector3 col = sharp + (soft - sharp) * p.BlurMix;

            // Vibrancy, then tint.
            float luma = Vector3.Dot(col, new Vector3(0.2126f, 0.7152f, 0.0722f));
            col = (new Vector3(luma) + (col - new Vector3(luma)) * (1f + 0.22f * p.Highlight)) * (1f + 0.04f * p.Highlight);
            col += (p.Tint - col) * p.TintMix;

            // Light: crisp rim line, soft glow inside it, faint sheen from the top.
            float lit = Sat(nx * Light.X + ny * Light.Y);
            float back = Sat(-(nx * Light.X + ny * Light.Y));
            float rimLine = 1 - Smoothstep(0f, 1.6f, depth);
            float glow = Sat(1 - depth / (edge * 1.6f));
            float add = p.Highlight * p.Rim * rimLine * (0.22f + 0.78f * lit + 0.35f * back)
                      + p.Highlight * glow * glow * lit * 0.16f + sheen;
            col += new Vector3(add);

            // Depth: a light shade where the lens bends the most.
            float band = Smoothstep(0f, edge * 0.5f, depth) * (1 - Smoothstep(edge * 0.5f, edge * 1.3f, depth));
            col *= 1 - 0.05f * band * (1 - lit);

            col = Vector3.Clamp(col, Vector3.Zero, Vector3.One);
            float a = cov + (1 - cov) * shadowA;
            if (overlay)
            {
                dst[di] = To8(col.Z * cov);
                dst[di + 1] = To8(col.Y * cov);
                dst[di + 2] = To8(col.X * cov);
                dst[di + 3] = To8(a);
                continue;
            }
            float inv = 1 - a;
            dst[di] = To8(col.Z * cov + dst[di] / 255f * inv);
            dst[di + 1] = To8(col.Y * cov + dst[di + 1] / 255f * inv);
            dst[di + 2] = To8(col.X * cov + dst[di + 2] / 255f * inv);
        }
    }

    // Mean brightness (0..1) of a rectangle of a BGRA frame; picks light or dark text on "auto".
    public static float MeanLuma(byte[] bgra, int stride, int x0, int y0, int x1, int y1)
    {
        double sum = 0; int n = 0;
        for (int y = y0; y < y1; y += 2)
            for (int x = x0; x < x1; x += 3)
            {
                int i = (y * stride + x) * 4;
                sum += 0.0722 * bgra[i] + 0.7152 * bgra[i + 1] + 0.2126 * bgra[i + 2];
                n++;
            }
        return n == 0 ? 0.5f : (float)(sum / n / 255.0);
    }

    // Same C3 bump as glass.hlsl; x and y are relative to the base shape's centre.
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static float WaveY(float x, float y, float by, GlassSpec p)
    {
        if (p.WaveHeight <= 0) return y;
        float u = (x - p.WaveX) / MathF.Max(p.WaveWidth, 1);
        float w = Sat(1 - u * u), lift = p.WaveHeight * w * w * w * w;
        return y + (by - y) * lift / (MathF.Max(2 * by, 1) + lift);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static float Sd(float x, float y, float bx, float by, float r)
    {
        var corner = CornerProfile.For(bx * 2, by * 2, r);
        r = corner.Reach;
        float qx = MathF.Abs(x) - bx + r, qy = MathF.Abs(y) - by + r;
        if (MathF.Min(qx, qy) <= 0) return MathF.Max(qx, qy) - r;
        if (r <= 0) return MathF.Sqrt(qx * qx + qy * qy);
        float n = MathF.Sqrt(qx * qx + qy * qy), vx = 0, vy = 0, fx = 0, fy = 0;
        for (int i = 0; i < 2; i++)
        {
            vx = qx / n; vy = qy / n;
            float a = corner.A;
            float xx = vx * vx, yy = vy * vy, dx = a + (1 - a) * xx, dy = a + (1 - a) * yy;
            fx = 2 * vx * xx * (2 * a + (1 - a) * xx) / (dx * dx);
            fy = 2 * vy * yy * (2 * a + (1 - a) * yy) / (dy * dy);
            n *= 1 + (xx * xx / dx + yy * yy / dy - 1) / (vx * fx + vy * fy);
        }
        return (n - r) * (vx * fx + vy * fy) / MathF.Sqrt(fx * fx + fy * fy);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static float Sat(float v) => v < 0 ? 0 : v > 1 ? 1 : v;

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static float Smoothstep(float a, float b, float x)
    {
        float t = Sat((x - a) / (b - a));
        return t * t * (3 - 2 * t);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static byte To8(float v) => (byte)(Sat(v) * 255f + 0.5f);

    // Bg.SampleLevel(Linear, ...) with trilinear filtering and clamped addressing.
    [MethodImpl(MethodImplOptions.AggressiveOptimization)]
    private static Vector3 Tex(Backdrop bg, float x, float y, float lod)
    {
        int maxL = bg.Mips.Length - 1;
        if (lod < 0) lod = 0;
        if (lod >= maxL) return Bilerp(bg, maxL, x, y);
        int l0 = (int)lod;
        float f = lod - l0;
        var a = Bilerp(bg, l0, x, y);
        if (f < 1e-3f) return a;
        return a + (Bilerp(bg, l0 + 1, x, y) - a) * f;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static Vector3 Bilerp(Backdrop bg, int level, float x, float y)
    {
        var m = bg.Mips[level];
        int w = bg.MipW[level], h = bg.MipH[level];
        float s = 1f / (1 << level);
        x = x * s - 0.5f; y = y * s - 0.5f;
        float fx = MathF.Floor(x), fy = MathF.Floor(y);
        float tx = x - fx, ty = y - fy;
        int x0 = (int)fx, y0 = (int)fy;
        int x1 = Math.Clamp(x0 + 1, 0, w - 1), y1 = Math.Clamp(y0 + 1, 0, h - 1);
        x0 = Math.Clamp(x0, 0, w - 1); y0 = Math.Clamp(y0, 0, h - 1);
        int i00 = (y0 * w + x0) * 4, i10 = (y0 * w + x1) * 4, i01 = (y1 * w + x0) * 4, i11 = (y1 * w + x1) * 4;
        var a = Pixel(m, i00);
        var b = Pixel(m, i10);
        var c = Pixel(m, i01);
        var d = Pixel(m, i11);
        a += (b - a) * tx;
        c += (d - c) * tx;
        var color = (a + (c - a) * ty) * (1f / 255f);
        return new Vector3(color.Z, color.Y, color.X);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static Vector4 Pixel(byte[] pixels, int offset)
    {
        uint bgra = Unsafe.ReadUnaligned<uint>(ref pixels[offset]);
        return Vector128.ConvertToSingle(Vector128.WidenLower(Vector128.WidenLower(Vector128.CreateScalarUnsafe(bgra).AsByte()))).AsVector4();
    }
}

// Runs the newest posted job on the thread pool, one at a time; jobs posted meanwhile replace
// each other, so a dragged slider never queues stale frames.
internal sealed class LatestWorker<T> where T : class
{
    private readonly Action<T> _run;
    private readonly object _lock = new();
    private T? _next;
    private bool _busy;
    private bool _stopped;

    public LatestWorker(Action<T> run) => _run = run;

    public void Post(T job)
    {
        lock (_lock)
        {
            if (_stopped) return;
            _next = job;
            if (_busy) return;
            _busy = true;
        }
        Task.Run(Loop);
    }

    public void Stop()
    {
        lock (_lock) { _stopped = true; _next = null; }
    }

    private void Loop()
    {
        while (true)
        {
            T job;
            lock (_lock)
            {
                if (_next == null) { _busy = false; return; }
                job = _next;
                _next = null;
            }
            try { _run(job); }
            catch (Exception e) { System.Diagnostics.Debug.WriteLine("preview render failed: " + e); }
        }
    }
}

// Two BGRA buffers: the worker renders into one and swaps, the UI thread copies from the other.
internal sealed class FrameBuffers
{
    private readonly object _lock = new();
    private byte[] _front, _back;
    public int Width { get; }
    public int Height { get; }
    // What came with the front buffer: its mean luminance, where it goes (DIP; an overlay's
    // top-left in its frame) and its pixels per DIP.
    public readonly record struct Placement(float Luma, double X, double Y, double Scale);
    private Placement _placed = new(0.5f, 0, 0, 1);

    public FrameBuffers(int w, int h)
    {
        Width = w; Height = h;
        _front = new byte[w * h * 4];
        _back = new byte[w * h * 4];
    }

    public byte[] Back => _back;

    public void Publish(float luma, double x = 0, double y = 0, double scale = 1)
    {
        lock (_lock) { (_front, _back) = (_back, _front); _placed = new Placement(luma, x, y, scale); }
    }

    // The front buffer's pixels and its placement, read together (the next frame may be published meanwhile).
    public Placement WriteFront(Stream target)
    {
        lock (_lock)
        {
            target.Write(_front, 0, _front.Length);
            return _placed;
        }
    }
}
