using System.Runtime.InteropServices.WindowsRuntime;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Text;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using Windows.UI;

namespace Atrium.Settings;

// The hero specimen: the glass bar rendered on the CPU (GlassPreview.cs) over the specimen photo
// (Photo.cs), with real shell icons and taskbar entries laid out by the same rules as the real bar, a loupe on
// the right end of the bar, and dimension marks for the parameter that is being edited. The photo
// is uploaded once per size; a frame repaints only the glass, as a transparent layer over it.
public sealed partial class MainWindow
{
    // The loupe: the bar's top right corner at 5x, the whole corner and the bell beside it.
    private const double LoupeZoom = 5;
    // The bar's width the preview is scaled for: a default bar (seven apps and the clock) at full size
    // once the preview is this wide (DIP), smaller below it.
    private const double HeroReferenceW = 860;

    // The bar in hero DIP (real DIP * S). Lefts are base positions; Map() is the magnification wave
    // (BarPainter::MapX: the local scale 1 + K * (1 + cos(pi x)) / 2 over Range, integrated from the
    // pointer at Centre), BarX/BarW the stretched glass the wave makes.
    internal sealed record BarGeometry(
        double S, double BaseX, double BarX, double BarY, double BarW, double BarH, double Radius, double MaxW,
        int N, double Icon, double P, double PlateR, double AppsLeft, double Pitch, double PlateY,
        bool Clock, double ClockX, double ClockW, double TimePx, double DatePx, double SepX,
        double StartX, double TrayX, double StatusX, double BellX,
        double K, double Centre, double Range,
        double LoupeX0, double LoupeY0, double LoupeW, double LoupeH, double StatusExtra = 0)
    {
        public double PlateX(int i) => BaseX + (AppsLeft + i * Pitch) * S;

        public double Map(double u)
        {
            if (K <= 0) return u;
            double x = (u - Centre) / Range;
            double area = x >= 1 ? 0.5 : x <= -1 ? -0.5 : x / 2 + Math.Sin(Math.PI * x) / (2 * Math.PI);
            return u + K * Range * area;
        }
    }

    private sealed record RenderJob(BarSettings S, BarGeometry L, double W, double H, double Rs, bool Loupe, Lazy<Photo> Photo, int PhotoGeneration);

    private const int MaxPreviewApps = 8;
    private PinVm _previewRunning = null!;
    private PinVm[] _previewPins = Array.Empty<PinVm>();

    private double _heroW, _heroH;
    private readonly ImageBrush _heroBrush = new() { Stretch = Stretch.Fill };
    private readonly ImageBrush _loupeBrush = new() { Stretch = Stretch.Fill };
    private readonly Image _glass = new() { Stretch = Stretch.Fill, IsHitTestVisible = false };
    private WriteableBitmap? _photoBmp, _glassBmp, _loupeBmp;
    private Backdrop? _photoPending; // a new backdrop for the hero's background (Interlocked)
    private volatile FrameBuffers? _heroFrames, _loupeFrames;
    private LatestWorker<RenderJob>? _renderWorker;
    private LatestWorker<BarSettings>? _swatchWorker;
    private sealed record SwatchPhoto(int Generation, Lazy<Photo> Photo);
    private LatestWorker<SwatchPhoto>? _fixedSwatchWorker;
    private int _uiPending;
    private float _luma = 0.3f;
    private bool _darkInk; // the bar's ink over the preview, with the core's hysteresis
    private string? _engaged;
    private bool _heroSized;

    // Render-thread caches.
    private Backdrop? _mainBg, _loupeBg;
    private string _mainKey = "", _loupeKey = "";

    private BarFace _face = null!, _loupeFace = null!;
    private readonly List<UIElement> _annotations = new();

    private void InitHero()
    {
        HeroFrame.Background = _heroBrush;
        Loupe.Background = _loupeBrush;
        _renderWorker = new LatestWorker<RenderJob>(RenderFrame);
        _swatchWorker = new LatestWorker<BarSettings>(job => RenderSwatch(job, SwatchCustom));
        _previewRunning = new PinVm(this, new PinnedApp("Atrium", Environment.ProcessPath!, "", ""))
        {
            Icon = new BitmapImage(new Uri(System.IO.Path.Combine(AppContext.BaseDirectory, "Assets", "atrium-256.png"))) { DecodePixelWidth = 96 },
        };

        HeroCanvas.Children.Add(_glass);
        _face = new BarFace(HeroCanvas, MaxPreviewApps);
        _loupeFace = new BarFace(LoupeCanvas, MaxPreviewApps);
        for (int i = 0; i < MaxPreviewApps; i++)
            foreach (var icon in new[] { _face.Icons[i], _loupeFace.Icons[i] })
                icon.SetBinding(Image.SourceProperty, new Microsoft.UI.Xaml.Data.Binding { Path = new PropertyPath(nameof(PinVm.Icon)) });

        // The two presets are fixed swatches; the custom one follows the current material.
        var liquid = BarSettings.Liquid();
        var frosted = BarSettings.Frosted();
        _fixedSwatchWorker = new LatestWorker<SwatchPhoto>(photo =>
        {
            RenderSwatch(liquid, SwatchLiquid, photo);
            RenderSwatch(frosted, SwatchFrosted, photo);
        });
        _fixedSwatchWorker.Post(new SwatchPhoto(Photo.Generation, Photo.Capture()));
        // A new picture: the swatches again, over it.
        PhotoChanged += () =>
        {
            _fixedSwatchWorker.Post(new SwatchPhoto(Photo.Generation, Photo.Capture()));
            _swatchSettings = _current.Clone();
            _swatchWorker?.Post(_swatchSettings);
        };
    }

    private double RasterScale => Math.Clamp(HeroFrame.XamlRoot?.RasterizationScale ?? 1.0, 1.0, 2.0);

    private void OnHeroSizeChanged(object sender, SizeChangedEventArgs e)
    {
        _heroW = e.NewSize.Width;
        _heroH = e.NewSize.Height;
        HeroCanvas.Clip = new RectangleGeometry { Rect = new Rect(0, 0, _heroW, _heroH) };
        if (!_heroSized && HeroFrame.XamlRoot != null)
        {
            _heroSized = true;
            HeroFrame.XamlRoot.Changed += (_, _) => UpdateHero();
        }
        UpdateHero();
    }

    // A card was focused or let go: remember which one so the hero can point at it.
    private void OnEngage(string title, bool on)
    {
        _engaged = on ? title : (_engaged == title ? null : _engaged);
        bool edge = _engaged is "折射强度" or "折射边缘宽度" or "色散" or "高光" or "圆角" or "阴影";
        Loupe.BorderBrush = edge ? Tokens.Brush("AccentBrush") : new SolidColorBrush(Color.FromArgb(179, 255, 255, 255));
        UpdateHero();
    }

    // ---- Layout: the same rules as the real bar, scaled to the specimen ----

    private BarGeometry? ComputeLayout(BarSettings s)
    {
        if (_heroW < 80 || _heroH < 80) return null;
        double S = Math.Min(1.0, (_heroW - 48) / (HeroReferenceW + 2 * GlassSpec.InsetDip));
        double H = s.Height, I = Math.Clamp(s.IconSize, 12, H - 16);
        double P = Math.Max(I + 4, Math.Min(H - 12, Math.Round(I * 1.55)));
        double v = Math.Floor((H - P) / 2), pad = v + 4, gap = Math.Max(4, Math.Round(I * 0.18));
        double timePx = H * 0.25, datePx = H * 0.19, sepGap = 8;
        double clockW = s.ShowClock ? Math.Max(timePx * 2.75, datePx * 5.7) + 22 : 0;
        int rightCount = (s.ShowTray ? 1 : 0) + (s.ShowStatus ? 1 : 0) + (s.ShowNotifications ? 1 : 0) + (s.ShowClock ? 1 : 0);
        double statusExtra = s.StatusStyle == "icons" ? Math.Round(I * 1.3) : 0; // three icons: about an icon wider
        double rightW = (rightCount - (s.ShowClock ? 1 : 0)) * P + (s.ShowStatus ? statusExtra : 0) + clockW + Math.Max(0, rightCount - 1) * gap;
        double Content(int n) => Math.Max(2 * H, 2 * pad + P + 2 * gap + n * P + Math.Max(0, n - 1) * gap + (rightCount > 0 ? 2 * sepGap + rightW : 0));
        double mag = Math.Clamp(s.Magnify, 1, 2), pitch = P + gap;
        double stretch = mag > 1.001 ? Math.Ceiling((mag - 1) * pitch * 2.3) : 0;
        double maxW = Math.Max(2 * H, Math.Min(s.Width, (_heroW - 16) / S - stretch));
        int n = _previewPins.Length + (s.ShowRunning ? 1 : 0);
        while (n > 0 && Content(n) > maxW) n--;
        double barW = s.AutoWidth ? Math.Min(Content(n), maxW) : maxW;

        double cursor = barW - pad;
        double Slot(double width, bool on)
        {
            if (!on) return -1;
            cursor -= width;
            double x = cursor;
            cursor -= gap;
            return x;
        }
        double bellLeft = Slot(P, s.ShowNotifications), clockLeft = Slot(clockW, s.ShowClock);
        double statusLeft = Slot(P + statusExtra, s.ShowStatus), trayLeft = Slot(P, s.ShowTray);
        double rightLeft = rightCount > 0 ? cursor + gap : barW - pad;
        double appsMin = pad + P + 2 * gap;
        double appsLeft = appsMin + Math.Max(0, (rightLeft - (rightCount > 0 ? 2 * sepGap : 0) - appsMin - (n * P + Math.Max(0, n - 1) * gap)) / 2);
        double reff = Math.Min(s.CornerRadius, Math.Min(H, barW) / 2);

        double bx = (_heroW - barW * S) / 2;
        double by = Math.Max(10, _heroH - s.Margin * S - H * S);
        double X(double left) => left < 0 ? -1 : bx + left * S;

        // As if the pointer rested on the second app.
        double k = mag > 1.001 ? mag - 1 : 0, range = pitch * 2.3 * S;
        double centre = n > 1 ? bx + (appsLeft + pitch) * S + P * S / 2 : bx + (pad + P / 2) * S;
        double Map(double u)
        {
            if (k <= 0) return u;
            double x = (u - centre) / range;
            return u + k * range * (x >= 1 ? 0.5 : x <= -1 ? -0.5 : x / 2 + Math.Sin(Math.PI * x) / (2 * Math.PI));
        }
        double left = Map(bx), right = Map(bx + barW * S);

        // The loupe's view: the whole top right corner and the bell, 10 DIP of air around.
        double lw = Math.Max(64, reff + 34), lh = Math.Max(56, reff + 26);
        return new BarGeometry(S, bx, left, by, right - left, H * S, reff * S, maxW * S,
            n, I * S, P * S, Math.Max(4, Math.Min(P / 2, reff - v)) * S, appsLeft, pitch, by + v * S,
            clockW > 0, X(clockLeft), clockW * S, timePx * S, datePx * S, rightCount > 0 ? bx + (rightLeft - sepGap) * S : -1,
            bx + pad * S, X(trayLeft), X(statusLeft), X(bellLeft),
            k, centre, range,
            right - (lw - 10) * S, by - 10 * S, lw * S, lh * S, statusExtra * S);
    }

    private void UpdateHero()
    {
        UpdateReadout();
        if (HeroBlock.Visibility != Visibility.Visible || _renderWorker == null) return;
        var L = ComputeLayout(_current);
        if (L == null) return;
        bool loupe = Loupe.Visibility == Visibility.Visible;
        if (loupe)
        {
            Loupe.Width = Math.Round(L.LoupeW * LoupeZoom);
            Loupe.Height = Math.Round(L.LoupeH * LoupeZoom);
            LoupeCanvas.Clip = new RectangleGeometry { Rect = new Rect(0, 0, Loupe.Width, Loupe.Height) };
        }
        PositionTaskbar(L, loupe);
        DrawAnnotations(L);
        _renderWorker.Post(new RenderJob(_current.Clone(), L, _heroW, _heroH, RasterScale, loupe, Photo.Capture(), Photo.Generation));
    }

    private bool DarkInk() => _current.TextColor == "dark" || (_current.TextColor == "auto" && _darkInk);

    private void PositionTaskbar(BarGeometry L, bool loupe)
    {
        int running = _current.ShowRunning && L.N > _previewPins.Length ? _previewPins.Length : -1;
        for (int i = 0; i < MaxPreviewApps; i++)
        {
            var vm = i < _previewPins.Length ? _previewPins[i] : _previewRunning;
            if (_face.Icons[i].DataContext != vm) _face.Icons[i].DataContext = vm;
            if (_loupeFace.Icons[i].DataContext != vm) _loupeFace.Icons[i].DataContext = vm;
        }
        var clock = DateTime.Now;
        string time = clock.ToString("H:mm"), date = clock.ToString("yyyy/M/d");
        _face.SetClock(time, date);
        var battery = BarFace.Battery();
        _face.Layout(L, DarkInk(), L.N, running, _current.Magnify, 1, 0, 0, battery);
        if (loupe)
        {
            _loupeFace.SetClock(time, date);
            _loupeFace.Layout(L, DarkInk(), L.N, running, _current.Magnify, LoupeZoom, L.LoupeX0, L.LoupeY0, battery);
        }
    }

    // ---- Dimension marks for the card in use ----

    private void DrawAnnotations(BarGeometry L)
    {
        foreach (var a in _annotations) HeroCanvas.Children.Remove(a);
        _annotations.Clear();
        if (_engaged == null) return;
        var white = new SolidColorBrush(Color.FromArgb(235, 255, 255, 255));
        void Line(double x1, double y1, double x2, double y2, bool dashed = false)
        {
            var l = new Line { X1 = x1, Y1 = y1, X2 = x2, Y2 = y2, Stroke = white, StrokeThickness = 1.5, StrokeStartLineCap = PenLineCap.Round, StrokeEndLineCap = PenLineCap.Round, IsHitTestVisible = false };
            if (dashed) l.StrokeDashArray = new DoubleCollection { 3, 3 };
            _annotations.Add(l);
            HeroCanvas.Children.Add(l);
        }
        void Chip(string text, double cx, double top)
        {
            var chip = new Border
            {
                Background = new SolidColorBrush(Color.FromArgb(235, 255, 255, 255)), CornerRadius = new CornerRadius(6), Padding = new Thickness(7, 2, 7, 3),
                Child = new TextBlock { Text = text, FontSize = 11, FontFamily = (FontFamily)Application.Current.Resources["EmphasisFont"], Foreground = new SolidColorBrush(Color.FromArgb(255, 22, 24, 29)) },
                IsHitTestVisible = false,
            };
            Microsoft.UI.Xaml.Documents.Typography.SetNumeralAlignment((TextBlock)chip.Child, FontNumeralAlignment.Tabular);
            chip.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
            Canvas.SetLeft(chip, Math.Clamp(cx - chip.DesiredSize.Width / 2, 4, Math.Max(4, _heroW - chip.DesiredSize.Width - 4)));
            Canvas.SetTop(chip, Math.Clamp(top, 4, Math.Max(4, _heroH - chip.DesiredSize.Height - 4)));
            _annotations.Add(chip);
            HeroCanvas.Children.Add(chip);
        }

        double barBottom = L.BarY + L.BarH, barRight = L.BarX + L.BarW;
        switch (_engaged)
        {
            case "高度":
            {
                double x = L.BarX - 14;
                Line(x, L.BarY, x, barBottom); Line(x - 4, L.BarY, x + 4, L.BarY); Line(x - 4, barBottom, x + 4, barBottom);
                Chip($"{Math.Round(_current.Height)} px", x - 30, L.BarY + L.BarH / 2 - 10);
                break;
            }
            case "底部间距":
            {
                double x = L.BarX + L.BarW * 0.5;
                Line(x, barBottom, x, _heroH); Line(x - 4, barBottom, x + 4, barBottom); Line(x - 4, _heroH - 1, x + 4, _heroH - 1);
                Chip($"{Math.Round(_current.Margin)} px", x + 34, (barBottom + _heroH) / 2 - 10);
                break;
            }
            case "最大宽度":
            {
                double y = L.BarY - 12, x0 = (_heroW - L.MaxW) / 2, x1 = x0 + L.MaxW;
                Line(x0, y, x1, y, dashed: true); Line(x0, y - 4, x0, y + 4); Line(x1, y - 4, x1, y + 4);
                Chip($"{Math.Round(_current.Width)} px", _heroW / 2, y - 24);
                break;
            }
            case "圆角":
            {
                // The circle the corner is an arc of, and its radius.
                double r = Math.Max(L.Radius, 2);
                var circle = new Ellipse { Width = 2 * r, Height = 2 * r, Stroke = white, StrokeThickness = 1.25, StrokeDashArray = new DoubleCollection { 3, 3 }, IsHitTestVisible = false };
                Canvas.SetLeft(circle, L.BarX);
                Canvas.SetTop(circle, L.BarY);
                _annotations.Add(circle);
                HeroCanvas.Children.Add(circle);
                Line(L.BarX + r, L.BarY + r, L.BarX + r - r * 0.7071, L.BarY + r - r * 0.7071);
                Chip($"R {Math.Round(Math.Min(_current.CornerRadius, _current.Height / 2))} px", L.BarX + r, L.BarY - 30);
                break;
            }
            case "图标大小":
            {
                if (L.N == 0) break;
                double x = L.PlateX(0) + L.P / 2 - L.Icon / 2 - 4, y0 = L.PlateY + L.P / 2 - L.Icon / 2, y1 = y0 + L.Icon;
                Line(x, y0, x, y1); Line(x - 3, y0, x + 3, y0); Line(x - 3, y1, x + 3, y1);
                Chip($"{Math.Round(_current.IconSize)} px", L.PlateX(0) + L.P / 2, L.BarY - 28);
                break;
            }
        }
    }

    // ---- Rendering (thread pool) ----

    private void RenderFrame(RenderJob j)
    {
        int pw = (int)Math.Round(j.W * j.Rs), ph = (int)Math.Round(j.H * j.Rs);
        if (pw < 8 || ph < 8) return;
        var L = j.L;

        // Main view: the photo once per size, then the glass alone, over it.
        string key = $"{pw}x{ph}/{j.W:F1}x{j.H:F1}/{j.PhotoGeneration}";
        if (_mainBg == null || key != _mainKey)
        {
            _mainBg = Backdrop.Create(pw, ph, j.W, j.H, 0, 0, j.Rs, j.Photo.Value);
            _mainKey = key;
            _heroPhotoGen = j.PhotoGeneration;
            Interlocked.Exchange(ref _photoPending, _mainBg);
        }
        // Keep the specimen and loupe in step with the native hover-rise switch.
        GlassSpec Spec(double ppd) => GlassSpec.For(j.S, L.S * ppd) with
        {
            WaveX = (float)((L.Centre - L.BarX - L.BarW / 2) * ppd),
            WaveWidth = (float)(L.Range * (1 + L.K / 2) * ppd),
            WaveHeight = j.S.HoverRise ? (float)(L.Icon * L.K * ppd) : 0,
        };
        var spec = Spec(j.Rs);
        int inset = (int)spec.Inset;
        int rise = spec.RiseInset;
        int bx = (int)Math.Round(L.BarX * j.Rs), by = (int)Math.Round(L.BarY * j.Rs);
        int bw = (int)Math.Round(L.BarW * j.Rs), bh = (int)Math.Round(L.BarH * j.Rs);
        int gw = bw + 2 * inset, gh = bh + 2 * inset + rise;
        var frames = _heroFrames;
        if (frames == null || frames.Width != gw || frames.Height != gh)
            _heroFrames = frames = new FrameBuffers(gw, gh);
        Array.Clear(frames.Back);
        GlassRenderer.DrawOverlay(_mainBg, frames.Back, bx - inset, by - inset - rise, gw, gh, spec);
        float luma = GlassRenderer.MeanLuma(frames.Back, gw, inset + Math.Clamp(bh / 2, 0, bw - 1), inset + rise + bh / 4,
            inset + Math.Clamp(bw - bh / 2, 1, bw), inset + rise + Math.Max(bh * 3 / 4, bh / 4 + 1));
        frames.Publish(luma, (bx - inset) / j.Rs, (by - inset - rise) / j.Rs, j.Rs);

        // Only the material page with the loupe enabled needs its second render.
        int lw = j.Loupe ? (int)Math.Round(Math.Round(L.LoupeW * LoupeZoom) * j.Rs) : 0, lh = j.Loupe ? (int)Math.Round(Math.Round(L.LoupeH * LoupeZoom) * j.Rs) : 0;
        if (j.Loupe)
        {
            double ppd = LoupeZoom * j.Rs;
            string lkey = $"{lw}x{lh}/{j.W:F1}x{j.H:F1}/{L.LoupeX0:F2},{L.LoupeY0:F2}/{j.PhotoGeneration}";
            if (_loupeBg == null || lkey != _loupeKey)
            {
                _loupeBg = Backdrop.Create(lw, lh, j.W, j.H, L.LoupeX0, L.LoupeY0, ppd, j.Photo.Value);
                _loupeKey = lkey;
            }
            var lframes = _loupeFrames;
            if (lframes == null || lframes.Width != lw || lframes.Height != lh)
                _loupeFrames = lframes = new FrameBuffers(lw, lh);
            Buffer.BlockCopy(_loupeBg.Base, 0, lframes.Back, 0, _loupeBg.Base.Length);
            var lspec = Spec(ppd);
            int linset = (int)lspec.Inset;
            int lbx = (int)Math.Round((L.BarX - L.LoupeX0) * ppd), lby = (int)Math.Round((L.BarY - L.LoupeY0) * ppd);
            int lbw = (int)Math.Round(L.BarW * ppd), lbh = (int)Math.Round(L.BarH * ppd);
            GlassRenderer.Draw(_loupeBg, lframes.Back, lbx - linset, lby - linset - lspec.RiseInset,
                lbw + 2 * linset, lbh + 2 * linset + lspec.RiseInset, lspec);
            lframes.Publish(luma);
        }
        else
        {
            _loupeBg = null;
            _loupeFrames = null;
            _loupeKey = "";
        }

        if (Interlocked.Exchange(ref _uiPending, 1) == 0)
            DispatcherQueue.TryEnqueue(ApplyFrames);
    }

    // ---- UI thread: frames to bitmaps ----

    private void ApplyFrames()
    {
        Interlocked.Exchange(ref _uiPending, 0);
        if (Interlocked.Exchange(ref _photoPending, null) is { } bg)
        {
            _photoBmp = new WriteableBitmap(bg.Width, bg.Height);
            using (var stream = _photoBmp.PixelBuffer.AsStream()) stream.Write(bg.Base, 0, bg.Base.Length);
            _photoBmp.Invalidate();
            _heroBrush.ImageSource = _photoBmp;
        }
        if (_heroFrames is { } hf)
        {
            if (_glassBmp == null || _glassBmp.PixelWidth != hf.Width || _glassBmp.PixelHeight != hf.Height)
            {
                _glassBmp = new WriteableBitmap(hf.Width, hf.Height);
                _glass.Source = _glassBmp;
            }
            FrameBuffers.Placement at;
            using (var stream = _glassBmp.PixelBuffer.AsStream()) at = hf.WriteFront(stream);
            _glassBmp.Invalidate();
            _glass.Width = hf.Width / at.Scale;
            _glass.Height = hf.Height / at.Scale;
            Canvas.SetLeft(_glass, at.X);
            Canvas.SetTop(_glass, at.Y);
            if (Math.Abs(at.Luma - _luma) > 0.001f)
            {
                _luma = at.Luma;
                // kDarkInkAbove / kLightInkBelow (bar.cpp): no flicker at the threshold.
                bool dark = _darkInk ? _luma >= 0.42f : _luma > 0.50f;
                if (dark != _darkInk)
                {
                    _darkInk = dark;
                    if (_current.TextColor == "auto") UpdateHero();
                }
            }
        }
        if (Loupe.Visibility == Visibility.Visible && _loupeFrames is { } lf) Upload(lf, ref _loupeBmp, _loupeBrush);
        if (_heroPhotoGen == Photo.Generation && _heroFrames != null) Ready("hero", _heroPhotoGen);
    }

    private static void Upload(FrameBuffers f, ref WriteableBitmap? bmp, ImageBrush brush)
    {
        if (bmp == null || bmp.PixelWidth != f.Width || bmp.PixelHeight != f.Height)
        {
            bmp = new WriteableBitmap(f.Width, f.Height);
            brush.ImageSource = bmp;
        }
        using (var stream = bmp.PixelBuffer.AsStream()) f.WriteFront(stream);
        bmp.Invalidate();
    }

    // ---- Preset swatches: the same renderer on a small canvas ----

    private const double SwatchW = 240, SwatchH = 72;
    private static (int Gen, Backdrop Bg)? s_swatchBg;
    private static readonly object s_swatchLock = new();

    private BarSettings? _swatchSettings;

    private void UpdateCustomSwatch()
    {
        if (_page != 0 || (_swatchSettings?.SameMaterial(_current) ?? false)) return;
        _swatchSettings = _current.Clone();
        _swatchWorker?.Post(_swatchSettings);
    }

    // Swatches are tiny; rendering them at 2x keeps them crisp up to 200 % scale.
    private void RenderSwatch(BarSettings material, Border target, SwatchPhoto? snapshot = null)
    {
        const double rs = 2;
        int w = (int)Math.Round(SwatchW * rs), h = (int)Math.Round(SwatchH * rs);
        Backdrop bg;
        snapshot ??= new SwatchPhoto(Photo.Generation, Photo.Capture());
        int gen = snapshot.Generation;
        var photo = snapshot.Photo;
        if (gen != Photo.Generation) return;
        lock (s_swatchLock)
        {
            if (s_swatchBg?.Gen != gen) s_swatchBg = (gen, Backdrop.Create(w, h, SwatchW, SwatchH, 0, 0, rs, photo.Value));
            bg = s_swatchBg.Value.Bg;
        }
        // A 60 DIP capsule at 60 %, centred.
        var s = material.Clone();
        s.Height = 60;
        s.CornerRadius = 30;
        var spec = GlassSpec.For(s, 0.6 * rs);
        int inset = (int)spec.Inset;
        int bw = (int)Math.Round(170 * rs), bh = (int)Math.Round(36 * rs);
        int bx = (w - bw) / 2, by = (h - bh) / 2;
        var pixels = new byte[bg.Base.Length];
        Buffer.BlockCopy(bg.Base, 0, pixels, 0, pixels.Length);
        GlassRenderer.Draw(bg, pixels, bx - inset, by - inset, bw + 2 * inset, bh + 2 * inset, spec);
        DispatcherQueue.TryEnqueue(() =>
        {
            if (gen != Photo.Generation || _updatesClosed) return;
            var bmp = new WriteableBitmap(w, h);
            using (var stream = bmp.PixelBuffer.AsStream()) stream.Write(pixels, 0, pixels.Length);
            bmp.Invalidate();
            target.Background = new ImageBrush { ImageSource = bmp, Stretch = Stretch.UniformToFill };
        });
    }
}
