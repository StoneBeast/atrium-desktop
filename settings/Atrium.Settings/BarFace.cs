using System.Runtime.InteropServices;
using Microsoft.UI;
using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Documents;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using Windows.UI;
using Path = Microsoft.UI.Xaml.Shapes.Path;

namespace Atrium.Settings;

// What the bar draws on its glass, as XAML over the preview's rendered glass: the same marks,
// sizes and inks as BarPainter (native/core/render/bar_painter.cpp) — the Windows logo's four
// panes, the app icons growing in the magnification wave, the running dots, the separator, the
// tray chevron, the status ring, the clock with its halo, the bell and its badge. One BarFace
// draws the preview at 1x, another the loupe's close-up.
internal sealed class BarFace
{
    [StructLayout(LayoutKind.Sequential)]
    private struct PowerStatus { public byte AcLine, BatteryFlag, Percent, SaverStatus; public int LifeTime, FullLifeTime; }
    [DllImport("kernel32.dll")] private static extern bool GetSystemPowerStatus(out PowerStatus status);

    // The bar's inks (BarPainter::DrawContent): white 96 % on dark glass, near-black 92 % on light.
    public static readonly Color LightInk = Color.FromArgb(245, 255, 255, 255), DarkInk = Color.FromArgb(235, 18, 18, 23);

    private readonly Canvas _canvas;
    private readonly Canvas _logo = new() { IsHitTestVisible = false };
    private readonly Rectangle[] _logoPanes = new Rectangle[4];
    private readonly List<Image> _icons = new();
    private readonly List<Ellipse> _dots = new();
    private readonly Border _plate = new() { IsHitTestVisible = false };
    private readonly Rectangle _sep = new() { IsHitTestVisible = false };
    private readonly FontIcon _tray = Glyph("\uE70E"), _bell = Glyph("\uEA8F");
    private readonly Border _badge = new() { IsHitTestVisible = false, Background = new SolidColorBrush(Color.FromArgb(255, 10, 133, 255)) };
    private readonly TextBlock _badgeText = new() { Text = "3", Foreground = new SolidColorBrush(Colors.White), HorizontalAlignment = HorizontalAlignment.Center,
                                                    VerticalAlignment = VerticalAlignment.Center, TextAlignment = TextAlignment.Center };
    private readonly StatusGlyph _status = new();
    private readonly StatusIcons _statusIcons = new();
    private readonly TextBlock _time = Clock(true), _date = Clock(false), _timeHalo = Clock(true), _dateHalo = Clock(false);
    private (MainWindow.BarGeometry Geometry, bool Dark, int Apps, int Running, double Magnify, double Zoom, double X, double Y,
             StatusLook Status)? _layout;
    private (double X, double Y, double Size, bool Icons, Color Ink)? _statusAt;

    public BarFace(Canvas canvas, int maxIcons)
    {
        _canvas = canvas;
        _badgeText.FontFamily = (FontFamily)Application.Current.Resources["TitleFont"];
        _badge.Child = _badgeText;
        for (int i = 0; i < _logoPanes.Length; ++i)
        {
            var pane = new Rectangle { IsHitTestVisible = false, Fill = LogoBrush() };
            ContinuousCorners.SetEnabled(pane, true);
            _logoPanes[i] = pane;
            _logo.Children.Add(pane);
        }
        canvas.Children.Add(_plate);
        canvas.Children.Add(_logo);
        for (int i = 0; i < maxIcons; i++)
        {
            var icon = new Image { Stretch = Stretch.Uniform, IsHitTestVisible = false };
            _icons.Add(icon);
            canvas.Children.Add(icon);
            var dot = new Ellipse { IsHitTestVisible = false };
            _dots.Add(dot);
            canvas.Children.Add(dot);
        }
        foreach (var e in new UIElement[] { _sep, _tray, _status, _statusIcons, _timeHalo, _dateHalo, _time, _date, _bell, _badge }) canvas.Children.Add(e);
    }

    public IReadOnlyList<Image> Icons => _icons;

    // The status entry in another state (the status page's demonstration), where Layout put it.
    public void ShowStatus(StatusLook status)
    {
        if (_statusAt is not var (x, y, size, icons, ink)) return;
        if (icons) _statusIcons.Update(size, ink, status);
        else _status.Update(size, ink, status);
        Canvas.SetLeft(icons ? _statusIcons : _status, x);
        Canvas.SetTop(icons ? _statusIcons : _status, y);
    }

    private static FontIcon Glyph(string glyph) => new()
    {
        Glyph = glyph, IsHitTestVisible = false, FontFamily = new FontFamily("Segoe Fluent Icons"),
        HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center,
    };

    // Time: Semibold; date: the regular face (MiSans Medium). Figures tabular.
    private static TextBlock Clock(bool time)
    {
        var t = new TextBlock
        {
            TextAlignment = TextAlignment.Center, IsHitTestVisible = false, IsTextScaleFactorEnabled = false, TextWrapping = TextWrapping.NoWrap,
            FontFamily = (FontFamily)Application.Current.Resources[time ? "TitleFont" : "TextFont"],
            LineStackingStrategy = LineStackingStrategy.BlockLineHeight, Text = time ? "10:24" : "2026/11/3",
        };
        Typography.SetNumeralAlignment(t, FontNumeralAlignment.Tabular);
        return t;
    }

    public void SetClock(string time, string date)
    {
        _time.Text = _timeHalo.Text = time;
        _date.Text = _dateHalo.Text = date;
    }

    // DrawLogo: four panes, Windows 11's light-to-deep blue across the whole logo.
    private static LinearGradientBrush LogoBrush()
    {
        var g = new LinearGradientBrush { StartPoint = new Point(0, 0), EndPoint = new Point(1, 1) };
        g.GradientStops.Add(new GradientStop { Color = Color.FromArgb(255, 0x5C, 0xCB, 0xFF), Offset = 0 });
        g.GradientStops.Add(new GradientStop { Color = Color.FromArgb(255, 0x1A, 0x9B, 0xF0), Offset = 0.5 });
        g.GradientStops.Add(new GradientStop { Color = Color.FromArgb(255, 0x00, 0x60, 0xC8), Offset = 1 });
        return g;
    }

    // The preview's state of the system: the real battery, Wi-Fi at full strength.
    public static (double Fill, bool Charging, bool Low) Battery()
    {
        if (!GetSystemPowerStatus(out var p) || (p.BatteryFlag & 128) != 0 || p.Percent > 100) return (0.6, false, false);
        bool charging = p.AcLine == 1;
        return (p.Percent / 100.0, charging, !charging && p.Percent <= 20);
    }

    // A computer without a battery (a desktop): the ring is the volume, with a speaker in its gap.
    public static bool HasBattery() => GetSystemPowerStatus(out var p) && (p.BatteryFlag & 128) == 0 && p.Percent <= 100;

    // Lays everything out for layout L (hero DIP), seen at `zoom` from origin (ox, oy).
    public void Layout(MainWindow.BarGeometry L, bool darkInk, int shownApps, int runningIndex, double magnify, double zoom, double ox, double oy,
                       StatusLook status)
    {
        // Material edits change the rendered glass, not the marks on it. Keep their geometries
        // and brushes until their layout, ink or status actually changes; clocks update separately.
        var layout = (L, darkInk, shownApps, runningIndex, magnify, zoom, ox, oy, status);
        if (_layout == layout) return;
        _layout = layout;
        var inkColor = darkInk ? DarkInk : LightInk;
        var ink = new SolidColorBrush(inkColor);
        double X(double u) => (L.Map(u) - ox) * zoom;
        double Y(double v) => (v - oy) * zoom;
        double iconTop = L.PlateY + (L.P - L.Icon) / 2, iconBottom = iconTop + L.Icon, barBottom = L.BarY + L.BarH;

        // One entry: its displayed square (left, top, size) under the wave, grown from its bottom.
        (double X, double Y, double Size) Item(double plateLeft)
        {
            double bx = plateLeft + L.P / 2;
            double grow = (L.Map(bx + L.P / 2) - L.Map(bx - L.P / 2)) / L.P;
            double size = L.Icon * grow * zoom;
            return (X(bx) - size / 2, Y(iconBottom) - size, size);
        }

        // The hover plate, only without the magnification (concentric with the bar).
        bool plate = shownApps > 1 && magnify <= 1.001;
        _plate.Visibility = plate ? Visibility.Visible : Visibility.Collapsed;
        if (plate)
        {
            _plate.Background = new SolidColorBrush(darkInk ? Color.FromArgb(26, 0, 0, 0) : Color.FromArgb(51, 255, 255, 255));
            _plate.Width = _plate.Height = L.P * zoom;
            _plate.CornerRadius = new CornerRadius(L.PlateR * zoom);
            Canvas.SetLeft(_plate, X(L.PlateX(1)));
            Canvas.SetTop(_plate, Y(L.PlateY));
        }

        // The logo: 80 % of the icon square, a gap of 8 %, corners of 10 % of a pane.
        {
            var (x, y, size) = Item(L.StartX);
            double w = size * 0.80, gap = w * 0.08, pane = (w - gap) / 2, r = pane * 0.10;
            _logo.Width = _logo.Height = w;
            for (int i = 0; i < 4; i++)
            {
                var part = _logoPanes[i];
                double px = (i % 2) * (pane + gap), py = (i / 2) * (pane + gap);
                part.Width = part.Height = pane;
                part.RadiusX = part.RadiusY = r;
                var gradient = (LinearGradientBrush)ContinuousCorners.Original(part)!;
                gradient.StartPoint = new Point(-px / pane, -py / pane);
                gradient.EndPoint = new Point((w - px) / pane, (w - py) / pane);
                Canvas.SetLeft(part, px);
                Canvas.SetTop(part, py);
            }
            Canvas.SetLeft(_logo, x + (size - w) / 2);
            Canvas.SetTop(_logo, y + (size - w) / 2);
        }

        for (int i = 0; i < _icons.Count; i++)
        {
            bool shown = i < shownApps;
            _icons[i].Visibility = shown ? Visibility.Visible : Visibility.Collapsed;
            _dots[i].Visibility = shown && i == runningIndex ? Visibility.Visible : Visibility.Collapsed;
            if (!shown) continue;
            var (x, y, size) = Item(L.PlateX(i));
            _icons[i].Width = _icons[i].Height = size;
            Canvas.SetLeft(_icons[i], x);
            Canvas.SetTop(_icons[i], y);
            // Running: a 1.9 DIP dot in the gap under the icon, 45 % ink (not the foreground app).
            double r = 1.9 * L.S * zoom;
            _dots[i].Width = _dots[i].Height = 2 * r;
            _dots[i].Fill = new SolidColorBrush(Color.FromArgb((byte)(inkColor.A * 0.45), inkColor.R, inkColor.G, inkColor.B));
            Canvas.SetLeft(_dots[i], X(L.PlateX(i) + L.P / 2) - r);
            Canvas.SetTop(_dots[i], Y((iconBottom + barBottom) / 2) - r);
        }

        // The separator: 46 % of the bar's height, 30 % (light) or 18 % (dark) ink.
        _sep.Visibility = L.SepX >= 0 ? Visibility.Visible : Visibility.Collapsed;
        if (L.SepX >= 0)
        {
            double h = L.BarH * 0.46;
            _sep.Width = Math.Max(1, L.S * zoom);
            _sep.Height = h * zoom;
            _sep.Fill = new SolidColorBrush(Color.FromArgb((byte)(255 * (darkInk ? 0.18 : 0.30)), inkColor.R, inkColor.G, inkColor.B));
            Canvas.SetLeft(_sep, X(L.SepX) - _sep.Width / 2);
            Canvas.SetTop(_sep, Y(L.BarY + (L.BarH - h) / 2));
        }

        // Glyph entries: Segoe Fluent Icons at 62 % of the icon square.
        void Entry(FontIcon view, double plateLeft)
        {
            view.Visibility = plateLeft < 0 ? Visibility.Collapsed : Visibility.Visible;
            if (plateLeft < 0) return;
            var (x, y, size) = Item(plateLeft);
            view.Width = view.Height = size;
            view.FontSize = Math.Max(4, size * 0.62);
            view.Foreground = ink;
            Canvas.SetLeft(view, x);
            Canvas.SetTop(view, y);
        }
        Entry(_tray, L.TrayX);
        Entry(_bell, L.BellX);

        // The bell's count: an accent disc, max(9, 46 % of the icon) across, on its upper right.
        _badge.Visibility = L.BellX < 0 ? Visibility.Collapsed : Visibility.Visible;
        if (L.BellX >= 0)
        {
            var (x, y, size) = Item(L.BellX);
            double d = Math.Max(9 * L.S * zoom, size * 0.46);
            _badge.Width = _badge.Height = d;
            _badge.CornerRadius = new CornerRadius(d / 2);
            _badgeText.FontSize = Math.Max(3, d * 0.62);
            Canvas.SetLeft(_badge, x + size - d * 0.35 - d / 2);
            Canvas.SetTop(_badge, y + d * 0.15 - d / 2);
        }

        bool icons = L.StatusExtra > 0;
        _status.Visibility = L.StatusX < 0 || icons ? Visibility.Collapsed : Visibility.Visible;
        _statusIcons.Visibility = L.StatusX < 0 || !icons ? Visibility.Collapsed : Visibility.Visible;
        _statusAt = null;
        if (L.StatusX >= 0)
        {
            // The three icons are centred on the wider entry (an icon square drawn past its sides).
            var (x, y, size) = Item(L.StatusX + L.StatusExtra / 2);
            _statusAt = (x, y, size, icons, inkColor);
            ShowStatus(status);
        }

        // The clock: moves with the stretched bar, does not grow; time 25 %, date 19 % of the bar's
        // height, line heights 1.22 and 1.25, centred on the bar; a halo just below (32 % black or 35 % white).
        bool clock = L.Clock;
        foreach (var t in new[] { _time, _date, _timeHalo, _dateHalo }) t.Visibility = clock ? Visibility.Visible : Visibility.Collapsed;
        if (clock)
        {
            double lineT = L.TimePx * 1.22, lineD = L.DatePx * 1.25;
            double cx = X(L.ClockX + L.ClockW / 2), w = Math.Max(L.ClockW, L.TimePx * 4) * zoom;
            double top = Y(L.BarY + L.BarH / 2 - (lineT + lineD) / 2);
            double halo = Math.Max(0.6 * L.S, 0.5) * zoom;
            var haloBrush = new SolidColorBrush(darkInk ? Color.FromArgb(89, 255, 255, 255) : Color.FromArgb(82, 0, 0, 0));
            void Place(TextBlock t, double size, double line, double y, Brush brush)
            {
                t.FontSize = Math.Max(1, size * zoom);
                t.LineHeight = line * zoom;
                t.Width = w;
                t.Foreground = brush;
                Canvas.SetLeft(t, cx - w / 2);
                Canvas.SetTop(t, y);
            }
            Place(_timeHalo, L.TimePx, lineT, top + halo, haloBrush);
            Place(_dateHalo, L.DatePx, lineD, top + lineT * zoom + halo, haloBrush);
            Place(_time, L.TimePx, lineT, top, ink);
            Place(_date, L.DatePx, lineD, top + lineT * zoom, ink);
        }
    }
}

// The height of a line of text in a TextBlock's font: what DWrite centres in a box (Canvas::Text's
// centred paragraphs), so marks sit where the core's do.
internal static class TextMetrics
{
    private static readonly Dictionary<(string, double, int), double> Lines = new();

    public static double Line(TextBlock t)
    {
        var key = (t.FontFamily.Source, t.FontSize, (int)t.FontWeight.Weight);
        if (Lines.TryGetValue(key, out double h)) return h;
        var probe = new TextBlock { FontFamily = t.FontFamily, FontSize = t.FontSize, FontWeight = t.FontWeight, Text = "Ag", IsTextScaleFactorEnabled = false };
        probe.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
        return Lines[key] = probe.DesiredSize.Height;
    }
}

// What the status entry shows (SystemStatus and BarPainter's StatusMotion): the battery, the volume, the
// connection, and how far a changed volume or brightness has taken the ring over (0..1 each).
internal sealed record StatusLook(double Fill, int Percent, bool Battery, bool Charging, int Volume = 50, bool Muted = false, int Lit = 3,
                                  bool Connected = true, bool Wifi = true, double VolumeMix = 0, double BrightMix = 0)
{
    public bool Low => Battery && !Charging && Percent >= 0 && Percent <= 20;

    // The computer's own battery (none on a desktop: the ring is the volume), Wi-Fi at full strength.
    public static StatusLook Current()
    {
        var (fill, charging, _) = BarFace.Battery();
        bool battery = BarFace.HasBattery();
        return battery ? new StatusLook(fill, (int)Math.Round(fill * 100), true, charging) : new StatusLook(0.5, -1, false, false);
    }
}

// The status ring (BarPainter::DrawStatus) in an h x h square: a ring open by 64 degrees at the
// bottom, filled clockwise from the gap's left end; a bolt in the gap while charging (a small speaker
// where there is no battery, the ring then the volume); in the middle the iPhone's Wi-Fi fan
// (wifi_glyph.cpp), or the speaker or the sun while a changed level shows on the ring.
internal sealed class StatusGlyph : Canvas
{
    private readonly Path _track = Stroke(), _level = Stroke(), _bolt = new() { IsHitTestVisible = false };
    private readonly WifiFan _fan = new();
    private readonly TextBlock _wired = Symbol(), _speaker = Symbol(), _sun = Symbol(), _gapSpeaker = Symbol();

    public StatusGlyph()
    {
        IsHitTestVisible = false;
        foreach (var e in new UIElement[] { _track, _level, _bolt, _fan, _wired, _speaker, _sun, _gapSpeaker }) Children.Add(e);
    }

    internal static Path Stroke() => new() { IsHitTestVisible = false, StrokeStartLineCap = PenLineCap.Round, StrokeEndLineCap = PenLineCap.Round };

    internal static TextBlock Symbol() => new()
    {
        IsHitTestVisible = false, FontFamily = new FontFamily("Segoe Fluent Icons"), TextAlignment = TextAlignment.Center, IsTextScaleFactorEnabled = false,
    };

    // A glyph centred in the w x w square at (x, y).
    internal static void Place(TextBlock t, string glyph, double x, double y, double w, double size, Color color)
    {
        t.Visibility = color.A < 1 ? Visibility.Collapsed : Visibility.Visible;
        t.Text = glyph;
        t.FontSize = Math.Max(1, size);
        t.Width = w;
        t.Foreground = new SolidColorBrush(color);
        Canvas.SetLeft(t, x);
        Canvas.SetTop(t, y + (w - TextMetrics.Line(t)) / 2);
    }

    internal static Point At(double cx, double cy, double r, double deg) =>
        new(cx + r * Math.Cos(deg * Math.PI / 180), cy + r * Math.Sin(deg * Math.PI / 180));

    internal static PathGeometry Arc(double cx, double cy, double r, double from, double sweep)
    {
        var f = new PathFigure { StartPoint = At(cx, cy, r, from), IsClosed = false, IsFilled = false };
        f.Segments.Add(new ArcSegment { Point = At(cx, cy, r, from + sweep), Size = new Size(r, r), SweepDirection = SweepDirection.Clockwise, IsLargeArc = sweep > 180 });
        return new PathGeometry { Figures = { f } };
    }

    internal static Color Fade(Color c, double a) => Color.FromArgb((byte)Math.Round(c.A * Math.Clamp(a, 0, 1)), c.R, c.G, c.B);

    internal static Color Red(Color ink) => ink.R < 128 ? Color.FromArgb(255, 0xE0, 0x35, 0x2B) : Color.FromArgb(255, 0xFF, 0x45, 0x3A);

    internal static string VolumeGlyph(StatusLook s) =>
        s.Muted || s.Volume == 0 ? "\uE74F" : s.Volume < 34 ? "\uE993" : s.Volume < 67 ? "\uE994" : "\uE995";

    // The bolt (BarPainter's chargingBolt_): six points in a unit square.
    internal static readonly (double X, double Y)[] BoltPoints = { (0.18, -0.5), (-0.4, 0.08), (-0.02, 0.08), (-0.18, 0.5), (0.4, -0.08), (0.02, -0.08) };

    public void Update(double h, Color ink, StatusLook s)
    {
        Width = Height = h;
        double cx = h / 2, cy = h / 2;
        // Inside: the connection (the fan, the wired glyph), at 60 % of the square.
        void Connection(double size, double alpha)
        {
            bool wired = s.Connected && !s.Wifi;
            _fan.Visibility = !wired && alpha > 0.003 ? Visibility.Visible : Visibility.Collapsed;
            _wired.Visibility = Visibility.Collapsed;
            if (alpha <= 0.003) return;
            if (wired) Place(_wired, "\uE839", cx - size, cy - size / 2, 2 * size, size * 0.62, Fade(ink, alpha));
            else
            {
                int lit = !s.Connected ? -1 : s.Lit;
                _fan.Update(cx, cy, size * 0.62, lit, Fade(ink, alpha), Fade(ink, (lit < 0 ? 0.32 : 0.24) * alpha));
            }
        }
        // The ring (the preview's computer has a playback device, so there always is one).
        const double Gap = 64, Start = 90 + Gap / 2, Sweep = 360 - Gap;
        double rad = h * 0.44, stroke = Math.Max(0.75, h * 0.07);
        double shown = Math.Min(1, s.VolumeMix + s.BrightMix);
        double mix = s.Battery ? s.VolumeMix : 1 - s.BrightMix; // 1: the ring shows the volume
        Color level = ink;
        if (s.Low)
        {
            var red = Red(ink);
            level = Color.FromArgb(ink.A, (byte)(red.R + (ink.R - red.R) * shown), (byte)(red.G + (ink.G - red.G) * shown), (byte)(red.B + (ink.B - red.B) * shown));
        }
        bool silent = s.Muted && mix > 0.5;
        _track.Data = Arc(cx, cy, rad, Start, Sweep);
        _track.Stroke = new SolidColorBrush(Fade(ink, silent ? 0.16 : 0.24));
        _track.StrokeThickness = stroke;
        double fill = Math.Clamp(s.Fill, 0, 1);
        _level.Visibility = fill > 0.0005 ? Visibility.Visible : Visibility.Collapsed;
        _level.Data = Arc(cx, cy, rad, Start, Math.Max(0.01, Sweep * Math.Min(fill, 0.9999)));
        _level.Stroke = new SolidColorBrush(level);
        _level.StrokeThickness = stroke;

        // In the gap: the bolt while charging (fading out while a level shows).
        double bolt = s.Battery && s.Charging ? 1 - shown : 0;
        _bolt.Visibility = bolt > 0.003 ? Visibility.Visible : Visibility.Collapsed;
        if (bolt > 0.003)
        {
            Point P((double X, double Y) p) => new(cx + p.X * 0.17 * h, cy + (0.44 + p.Y * 0.26) * h);
            var f = new PathFigure { IsClosed = true, IsFilled = true, StartPoint = P(BoltPoints[0]) };
            for (int i = 1; i < BoltPoints.Length; i++) f.Segments.Add(new LineSegment { Point = P(BoltPoints[i]) });
            _bolt.Data = new PathGeometry { Figures = { f } };
            _bolt.Fill = new SolidColorBrush(Fade(ink, bolt));
        }

        // Inside: the connection, or the speaker or the sun while a level shows.
        double speaker = s.Battery ? s.VolumeMix : 0;
        Connection(h * 0.6, 1 - Math.Min(1, speaker + s.BrightMix));
        Place(_speaker, VolumeGlyph(s), 0, 0, h, h * 0.40, Fade(ink, speaker));
        Place(_sun, "\uE706", 0, 0, h, h * 0.40, Fade(ink, s.BrightMix));
        // No battery: the ring is the volume, a small speaker in its gap (gone while the brightness shows).
        double gap = !s.Battery ? 1 - s.BrightMix : 0;
        Place(_gapSpeaker, VolumeGlyph(s), cx - h / 4, cy + rad - h / 4, h / 2, h * 0.22, Fade(ink, gap));
    }
}

// The iPhone's Wi-Fi fan (wifi_glyph.cpp): a wedge and two arcs about a point half a radius below
// the centre, as many lit as the signal (-1: offline, all faint).
internal sealed class WifiFan : Canvas
{
    private readonly Path _wedge = new() { IsHitTestVisible = false, StrokeLineJoin = PenLineJoin.Round };
    private readonly Path _arc1 = StatusGlyph.Stroke(), _arc2 = StatusGlyph.Stroke();

    public WifiFan()
    {
        IsHitTestVisible = false;
        Children.Add(_wedge);
        Children.Add(_arc1);
        Children.Add(_arc2);
    }

    public void Update(double cx, double cy, double radius, int lit, Color on, Color off)
    {
        const double Span = 78, From = 270 - Span / 2;
        double oy = cy + 0.5 * radius, r0 = 0.37 * radius;
        var wedge = new PathFigure { StartPoint = new Point(cx, oy), IsClosed = true, IsFilled = true };
        wedge.Segments.Add(new LineSegment { Point = StatusGlyph.At(cx, oy, r0, From) });
        wedge.Segments.Add(new ArcSegment { Point = StatusGlyph.At(cx, oy, r0, From + Span), Size = new Size(r0, r0), SweepDirection = SweepDirection.Clockwise });
        _wedge.Data = new PathGeometry { Figures = { wedge } };
        _wedge.Fill = _wedge.Stroke = new SolidColorBrush(lit >= 1 ? on : off);
        _wedge.StrokeThickness = 0.19 * 0.3 * radius;
        _arc1.Data = StatusGlyph.Arc(cx, oy, 0.595 * radius, From, Span);
        _arc1.Stroke = new SolidColorBrush(lit >= 2 ? on : off);
        _arc1.StrokeThickness = 0.19 * radius;
        _arc2.Data = StatusGlyph.Arc(cx, oy, 0.905 * radius, From, Span);
        _arc2.Stroke = new SolidColorBrush(lit >= 3 ? on : off);
        _arc2.StrokeThickness = 0.19 * radius;
    }
}

// The status entry as the Windows taskbar shows it (BarPainter::DrawStatusIcons), centred on an
// h x h square and wider than it: the network, the speaker with a wave for each third of the
// volume, the battery with its level (a bolt cut out of it while charging).
internal sealed class StatusIcons : Canvas
{
    private readonly WifiFan _fan = new();
    private readonly TextBlock _wired = StatusGlyph.Symbol();
    private readonly Path _cone = new() { IsHitTestVisible = false, StrokeLineJoin = PenLineJoin.Round };
    private readonly Path[] _waves = { StatusGlyph.Stroke(), StatusGlyph.Stroke(), StatusGlyph.Stroke() };
    private readonly Path _cross = StatusGlyph.Stroke();
    private readonly Path _body = new() { IsHitTestVisible = false }, _nub = new() { IsHitTestVisible = false }, _fill = new() { IsHitTestVisible = false };

    public StatusIcons()
    {
        IsHitTestVisible = false;
        foreach (var e in new UIElement[] { _fan, _wired, _cone, _waves[0], _waves[1], _waves[2], _cross, _body, _nub, _fill }) Children.Add(e);
    }

    private static PathFigure Figure(IReadOnlyList<Point> pts)
    {
        var f = new PathFigure { StartPoint = pts[0], IsClosed = true, IsFilled = true };
        for (int i = 1; i < pts.Count; i++) f.Segments.Add(new LineSegment { Point = pts[i] });
        return f;
    }

    public void Update(double h, Color ink, StatusLook s)
    {
        Width = Height = h;
        double cy = h / 2, stroke = Math.Max(0.75, h * 0.075);
        double netW = 0.62 * h, speakerW = 0.60 * h, batteryW = 0.68 * h, gap = 0.18 * h;
        double x = h / 2 - (netW + speakerW + (s.Battery ? batteryW + gap : 0) + gap) / 2;
        var on = new SolidColorBrush(ink);

        // The network: the fan lit by the signal, the wired glyph, or a faint fan offline.
        bool wired = s.Connected && !s.Wifi;
        _fan.Visibility = wired ? Visibility.Collapsed : Visibility.Visible;
        _wired.Visibility = wired ? Visibility.Visible : Visibility.Collapsed;
        if (wired) StatusGlyph.Place(_wired, "\uE839", x - h, cy - h / 2, netW + 2 * h, h * 0.62, ink);
        else
        {
            int lit = !s.Connected ? -1 : s.Lit;
            _fan.Update(x + netW / 2, cy, h * 0.46, lit, ink, StatusGlyph.Fade(ink, lit < 0 ? 0.32 : 0.24));
        }
        x += netW + gap;

        // The speaker: a cone, a wave for each third of the volume (the others faint); muted, a cross.
        _cone.Data = new PathGeometry { Figures = { Figure(new[] {
            new Point(x, cy - 0.10 * h), new Point(x + 0.11 * h, cy - 0.10 * h), new Point(x + 0.25 * h, cy - 0.22 * h),
            new Point(x + 0.25 * h, cy + 0.22 * h), new Point(x + 0.11 * h, cy + 0.10 * h), new Point(x, cy + 0.10 * h) }) } };
        _cone.Fill = _cone.Stroke = new SolidColorBrush(StatusGlyph.Fade(ink, s.Muted ? 0.55 : 1));
        _cone.StrokeThickness = stroke * 0.6;
        double ox = x + 0.25 * h;
        int waves = s.Volume <= 0 ? 0 : s.Volume < 34 ? 1 : s.Volume < 67 ? 2 : 3;
        for (int i = 0; i < 3; i++)
        {
            _waves[i].Visibility = s.Muted ? Visibility.Collapsed : Visibility.Visible;
            double r = (0.11 + 0.105 * i) * h;
            _waves[i].Data = StatusGlyph.Arc(ox, cy, r, -43, 86);
            _waves[i].Stroke = i < waves ? on : new SolidColorBrush(StatusGlyph.Fade(ink, 0.24));
            _waves[i].StrokeThickness = stroke;
        }
        _cross.Visibility = s.Muted ? Visibility.Visible : Visibility.Collapsed;
        if (s.Muted)
        {
            double c = ox + 0.19 * h, k = 0.08 * h;
            var g = new PathGeometry();
            foreach (var (a, b) in new[] { (new Point(c - k, cy - k), new Point(c + k, cy + k)), (new Point(c - k, cy + k), new Point(c + k, cy - k)) })
            {
                var f = new PathFigure { StartPoint = a, IsClosed = false, IsFilled = false };
                f.Segments.Add(new LineSegment { Point = b });
                g.Figures.Add(f);
            }
            _cross.Data = g;
            _cross.Stroke = on;
            _cross.StrokeThickness = stroke;
        }
        x += speakerW + gap;

        // The battery: an outline and its nub, the level inside (red when low), a bolt cut out of it
        // while charging (and drawn where there is no level).
        foreach (var p in new[] { _body, _nub, _fill }) p.Visibility = s.Battery ? Visibility.Visible : Visibility.Collapsed;
        if (!s.Battery) return;
        double bw = 0.58 * h, bh = 0.36 * h, rad = 0.09 * h;
        var body = new Rect(x + stroke / 2, cy - bh / 2, bw - stroke / 2, bh);
        var outline = new SolidColorBrush(StatusGlyph.Fade(ink, 0.9));
        _body.Data = RoundRect(body, rad);
        _body.Stroke = outline;
        _body.StrokeThickness = stroke;
        _nub.Data = RoundRect(new Rect(body.Right + 0.03 * h, cy - 0.075 * h, 0.05 * h, 0.15 * h), 0.02 * h);
        _nub.Fill = outline;
        double pad = stroke + 0.035 * h, level = Math.Clamp(s.Percent / 100.0, 0, 1);
        var inner = new Rect(body.Left + pad, body.Top + pad, Math.Max(0, body.Width - 2 * pad), Math.Max(0, body.Height - 2 * pad));
        var levelRect = new Rect(inner.Left, inner.Top, Math.Max(0.06 * h, inner.Width * level), inner.Height);
        var shape = new GeometryGroup { FillRule = FillRule.EvenOdd };
        shape.Children.Add(RoundRect(levelRect, 0.035 * h));
        if (s.Charging)
        {
            double bx = (body.Left + body.Right) / 2, k = bh * 0.62;
            shape.Children.Add(new PathGeometry { Figures = { Figure(StatusGlyph.BoltPoints.Select(p => new Point(bx + p.X * k, cy + p.Y * k * 1.5)).ToList()) } });
        }
        _fill.Data = shape;
        _fill.Fill = s.Low ? new SolidColorBrush(StatusGlyph.Red(ink)) : on;
    }

    private static PathGeometry RoundRect(Rect r, double radius)
    {
        radius = Math.Min(radius, Math.Min(r.Width, r.Height) / 2);
        var f = new PathFigure { StartPoint = new Point(r.Left + radius, r.Top), IsClosed = true, IsFilled = true };
        void Corner(double x, double y) => f.Segments.Add(new ArcSegment { Point = new Point(x, y), Size = new Size(radius, radius), SweepDirection = SweepDirection.Clockwise });
        f.Segments.Add(new LineSegment { Point = new Point(r.Right - radius, r.Top) });
        Corner(r.Right, r.Top + radius);
        f.Segments.Add(new LineSegment { Point = new Point(r.Right, r.Bottom - radius) });
        Corner(r.Right - radius, r.Bottom);
        f.Segments.Add(new LineSegment { Point = new Point(r.Left + radius, r.Bottom) });
        Corner(r.Left, r.Bottom - radius);
        f.Segments.Add(new LineSegment { Point = new Point(r.Left, r.Top + radius) });
        Corner(r.Left + radius, r.Top);
        return new PathGeometry { Figures = { f } };
    }
}
