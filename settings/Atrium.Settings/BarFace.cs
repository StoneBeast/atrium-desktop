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
             (double Fill, bool Charging, bool Low) Battery)? _layout;

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
                       (double Fill, bool Charging, bool Low) battery)
    {
        // Material edits change the rendered glass, not the marks on it. Keep their geometries
        // and brushes until their layout, ink or battery actually changes; clocks update separately.
        var layout = (L, darkInk, shownApps, runningIndex, magnify, zoom, ox, oy, battery);
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
        if (L.StatusX >= 0)
        {
            // The three icons are centred on the wider entry (an icon square drawn past its sides).
            var (x, y, size) = Item(L.StatusX + L.StatusExtra / 2);
            if (icons)
                _statusIcons.Update(size, inkColor, battery.Fill, battery.Charging, battery.Low, !HasBattery());
            else
                _status.Update(size, inkColor, battery.Fill, battery.Charging, battery.Low, 3, !HasBattery());
            Canvas.SetLeft(icons ? _statusIcons : _status, x);
            Canvas.SetTop(icons ? _statusIcons : _status, y);
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

// The status ring (BarPainter::DrawStatus) in an h x h square: a ring open by 64 degrees at the
// bottom, filled clockwise from the gap's left end, a bolt in the gap while charging (a speaker
// where there is no battery, the ring then the volume), and in the
// middle the iPhone's Wi-Fi fan (wifi_glyph.cpp): a wedge and two arcs, as many lit as the signal.
internal sealed class StatusGlyph : Canvas
{
    private readonly Path _track = Stroke(), _level = Stroke(), _bolt = new() { IsHitTestVisible = false };
    private readonly TextBlock _speaker = new() { IsHitTestVisible = false, FontFamily = new FontFamily("Segoe Fluent Icons"), Text = "\uE994",
                                                  TextAlignment = TextAlignment.Center };
    private readonly Path _wedge = new() { IsHitTestVisible = false, StrokeLineJoin = PenLineJoin.Round };
    private readonly Path _arc1 = Stroke(), _arc2 = Stroke();

    public StatusGlyph()
    {
        IsHitTestVisible = false;
        foreach (var p in new[] { _track, _level, _bolt, _wedge, _arc1, _arc2 }) Children.Add(p);
        Children.Add(_speaker);
    }

    private static Path Stroke() => new() { IsHitTestVisible = false, StrokeStartLineCap = PenLineCap.Round, StrokeEndLineCap = PenLineCap.Round };

    private static Point At(double cx, double cy, double r, double deg) =>
        new(cx + r * Math.Cos(deg * Math.PI / 180), cy + r * Math.Sin(deg * Math.PI / 180));

    private static PathGeometry Arc(double cx, double cy, double r, double from, double sweep)
    {
        var f = new PathFigure { StartPoint = At(cx, cy, r, from), IsClosed = false, IsFilled = false };
        f.Segments.Add(new ArcSegment { Point = At(cx, cy, r, from + sweep), Size = new Size(r, r), SweepDirection = SweepDirection.Clockwise, IsLargeArc = sweep > 180 });
        return new PathGeometry { Figures = { f } };
    }

    private static Color Fade(Color c, double a) => Color.FromArgb((byte)Math.Round(c.A * a), c.R, c.G, c.B);

    public void Update(double h, Color ink, double fill, bool charging, bool low, int lit, bool desktop = false)
    {
        Width = Height = h;
        double cx = h / 2, cy = h / 2;
        const double Gap = 64, Start = 90 + Gap / 2, Sweep = 360 - Gap;
        double rad = h * 0.44, stroke = Math.Max(0.75, h * 0.07);
        _track.Data = Arc(cx, cy, rad, Start, Sweep);
        _track.Stroke = new SolidColorBrush(Fade(ink, 0.24));
        _track.StrokeThickness = stroke;
        fill = Math.Clamp(fill, 0, 1);
        _level.Visibility = fill > 0.003 ? Visibility.Visible : Visibility.Collapsed;
        _level.Data = Arc(cx, cy, rad, Start, Math.Max(0.01, Sweep * Math.Min(fill, 0.9999)));
        _level.Stroke = new SolidColorBrush(low ? (ink.R < 128 ? Color.FromArgb(255, 0xE0, 0x35, 0x2B) : Color.FromArgb(255, 0xFF, 0x45, 0x3A)) : ink);
        _level.StrokeThickness = stroke;

        // The bolt, in the gap.
        _bolt.Visibility = charging ? Visibility.Visible : Visibility.Collapsed;
        if (charging)
        {
            (double X, double Y)[] pts = { (0.18, -0.5), (-0.4, 0.08), (-0.02, 0.08), (-0.18, 0.5), (0.4, -0.08), (0.02, -0.08) };
            var f = new PathFigure { IsClosed = true, IsFilled = true, StartPoint = new Point(cx + pts[0].X * 0.17 * h, cy + (0.44 + pts[0].Y * 0.26) * h) };
            for (int i = 1; i < pts.Length; i++)
                f.Segments.Add(new LineSegment { Point = new Point(cx + pts[i].X * 0.17 * h, cy + (0.44 + pts[i].Y * 0.26) * h) });
            _bolt.Data = new PathGeometry { Figures = { f } };
            _bolt.Fill = new SolidColorBrush(ink);
        }

        // A desktop's speaker, in the gap (BarPainter::DrawStatus): 22 % of the square, centred on the ring's bottom.
        _speaker.Visibility = desktop && !charging ? Visibility.Visible : Visibility.Collapsed;
        if (desktop)
        {
            _speaker.FontSize = Math.Max(1, h * 0.22);
            _speaker.Foreground = new SolidColorBrush(ink);
            _speaker.Width = h / 2;
            _speaker.LineHeight = h / 2;
            _speaker.LineStackingStrategy = LineStackingStrategy.BlockLineHeight;
            Canvas.SetLeft(_speaker, cx - h / 4);
            Canvas.SetTop(_speaker, cy + rad - h / 4);
        }

        // The fan: `size` = 60 % of the square, radius 62 % of that; its point half a radius below the centre.
        double R = h * 0.6 * 0.62, oy = cy + 0.5 * R;
        const double Span = 78, From = 270 - Span / 2;
        var on = new SolidColorBrush(ink);
        var off = new SolidColorBrush(Fade(ink, 0.24));
        double r0 = 0.37 * R;
        var wedge = new PathFigure { StartPoint = new Point(cx, oy), IsClosed = true, IsFilled = true };
        wedge.Segments.Add(new LineSegment { Point = At(cx, oy, r0, From) });
        wedge.Segments.Add(new ArcSegment { Point = At(cx, oy, r0, From + Span), Size = new Size(r0, r0), SweepDirection = SweepDirection.Clockwise });
        _wedge.Data = new PathGeometry { Figures = { wedge } };
        _wedge.Fill = _wedge.Stroke = lit >= 1 ? on : off;
        _wedge.StrokeThickness = 0.19 * 0.3 * R;
        _arc1.Data = Arc(cx, oy, 0.595 * R, From, Span);
        _arc1.Stroke = lit >= 2 ? on : off;
        _arc1.StrokeThickness = 0.19 * R;
        _arc2.Data = Arc(cx, oy, 0.905 * R, From, Span);
        _arc2.Stroke = lit >= 3 ? on : off;
        _arc2.StrokeThickness = 0.19 * R;
    }
}

// The status entry as the Windows taskbar shows it (BarPainter::DrawStatusIcons), centred on an
// h x h square and wider than it: the Wi-Fi fan, the speaker with its waves, the battery.
internal sealed class StatusIcons : Canvas
{
    private readonly Path _wedge = new() { IsHitTestVisible = false, StrokeLineJoin = PenLineJoin.Round };
    private readonly Path _arc1 = Stroke(), _arc2 = Stroke(), _cone = new() { IsHitTestVisible = false, StrokeLineJoin = PenLineJoin.Round };
    private readonly Path[] _waves = { Stroke(), Stroke(), Stroke() };
    private readonly Path _body = new() { IsHitTestVisible = false }, _nub = new() { IsHitTestVisible = false }, _fill = new() { IsHitTestVisible = false };

    public StatusIcons()
    {
        IsHitTestVisible = false;
        foreach (var p in new[] { _wedge, _arc1, _arc2, _cone, _waves[0], _waves[1], _waves[2], _body, _nub, _fill }) Children.Add(p);
    }

    private static Path Stroke() => new() { IsHitTestVisible = false, StrokeStartLineCap = PenLineCap.Round, StrokeEndLineCap = PenLineCap.Round };

    private static Point At(double cx, double cy, double r, double deg) =>
        new(cx + r * Math.Cos(deg * Math.PI / 180), cy + r * Math.Sin(deg * Math.PI / 180));

    private static PathGeometry Arc(double cx, double cy, double r, double from, double sweep)
    {
        var f = new PathFigure { StartPoint = At(cx, cy, r, from), IsClosed = false, IsFilled = false };
        f.Segments.Add(new ArcSegment { Point = At(cx, cy, r, from + sweep), Size = new Size(r, r), SweepDirection = SweepDirection.Clockwise });
        return new PathGeometry { Figures = { f } };
    }

    private static PathGeometry Polygon(params Point[] pts)
    {
        var f = new PathFigure { StartPoint = pts[0], IsClosed = true, IsFilled = true };
        for (int i = 1; i < pts.Length; i++) f.Segments.Add(new LineSegment { Point = pts[i] });
        return new PathGeometry { Figures = { f } };
    }

    private static Color Fade(Color c, double a) => Color.FromArgb((byte)Math.Round(c.A * a), c.R, c.G, c.B);

    public void Update(double h, Color ink, double fill, bool charging, bool low, bool desktop)
    {
        Width = Height = h;
        double cy = h / 2, stroke = Math.Max(0.75, h * 0.075);
        double netW = 0.62 * h, speakerW = 0.60 * h, batteryW = 0.68 * h, gap = 0.18 * h;
        double x = h / 2 - (netW + speakerW + gap + (desktop ? 0 : batteryW + gap)) / 2;
        var on = new SolidColorBrush(ink);
        var off = new SolidColorBrush(Fade(ink, 0.24));

        // The fan (wifi_glyph.cpp), fully lit: 46 % of the square high, its point half a radius below the middle.
        double R = 0.46 * h, cx = x + netW / 2, oy = cy + 0.5 * R;
        const double Span = 78, From = 270 - Span / 2;
        double r0 = 0.37 * R;
        var wedge = new PathFigure { StartPoint = new Point(cx, oy), IsClosed = true, IsFilled = true };
        wedge.Segments.Add(new LineSegment { Point = At(cx, oy, r0, From) });
        wedge.Segments.Add(new ArcSegment { Point = At(cx, oy, r0, From + Span), Size = new Size(r0, r0), SweepDirection = SweepDirection.Clockwise });
        _wedge.Data = new PathGeometry { Figures = { wedge } };
        _wedge.Fill = _wedge.Stroke = on;
        _wedge.StrokeThickness = 0.19 * 0.3 * R;
        _arc1.Data = Arc(cx, oy, 0.595 * R, From, Span);
        _arc2.Data = Arc(cx, oy, 0.905 * R, From, Span);
        _arc1.Stroke = _arc2.Stroke = on;
        _arc1.StrokeThickness = _arc2.StrokeThickness = 0.19 * R;
        x += netW + gap;

        // The speaker: a cone, two of three waves lit.
        _cone.Data = Polygon(new Point(x, cy - 0.10 * h), new Point(x + 0.11 * h, cy - 0.10 * h), new Point(x + 0.25 * h, cy - 0.22 * h),
                             new Point(x + 0.25 * h, cy + 0.22 * h), new Point(x + 0.11 * h, cy + 0.10 * h), new Point(x, cy + 0.10 * h));
        _cone.Fill = _cone.Stroke = on;
        _cone.StrokeThickness = stroke * 0.6;
        for (int i = 0; i < 3; i++)
        {
            double r = (0.11 + 0.105 * i) * h;
            _waves[i].Data = Arc(x + 0.25 * h, cy, r, -43, 86);
            _waves[i].Stroke = i < 2 ? on : off;
            _waves[i].StrokeThickness = stroke;
        }
        x += speakerW + gap;

        // The battery: an outline, its nub, the level (red when low).
        foreach (var p in new[] { _body, _nub, _fill }) p.Visibility = desktop ? Visibility.Collapsed : Visibility.Visible;
        if (desktop) return;
        double bw = 0.58 * h, bh = 0.36 * h;
        var body = new Rect(x + stroke / 2, cy - bh / 2, bw - stroke / 2, bh);
        _body.Data = new RectangleGeometry { Rect = body };
        _body.Stroke = new SolidColorBrush(Fade(ink, 0.9));
        _body.StrokeThickness = stroke;
        _nub.Data = new RectangleGeometry { Rect = new Rect(body.Right + 0.03 * h, cy - 0.075 * h, 0.05 * h, 0.15 * h) };
        _nub.Fill = new SolidColorBrush(Fade(ink, 0.9));
        double pad = stroke + 0.035 * h, inner = body.Width - 2 * pad;
        _fill.Data = new RectangleGeometry { Rect = new Rect(body.Left + pad, body.Top + pad, Math.Max(0.06 * h, inner * Math.Clamp(fill, 0, 1)), body.Height - 2 * pad) };
        _fill.Fill = low && !charging ? new SolidColorBrush(ink.R < 128 ? Color.FromArgb(255, 0xE0, 0x35, 0x2B) : Color.FromArgb(255, 0xFF, 0x45, 0x3A)) : on;
    }
}
