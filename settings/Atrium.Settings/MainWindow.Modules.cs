using System.Numerics;
using System.Runtime.InteropServices.WindowsRuntime;
using Microsoft.UI;
using Microsoft.UI.Composition;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Data;
using Microsoft.UI.Xaml.Hosting;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using Windows.UI;
using Path = Microsoft.UI.Xaml.Shapes.Path;

namespace Atrium.Settings;

// The pages under the bar (the start menu, the tray, the status entry, the time): each shows the
// bar at the bottom of the picture and the popup that opens from it, laid out with the core's
// numbers and moving with its curves. The glass is rendered on the CPU (GlassPreview.cs) for the
// shapes as they are on this frame; the marks on it are laid out for the shapes the glass on
// screen was rendered for, so the two never drift apart while a shape springs to another size.
public sealed partial class MainWindow
{
    private ModuleScene? _startScene, _trayScene, _statusScene, _clockScene;

    private ModuleScene? SceneFor(int page) => page switch
    {
        10 => _startScene ??= new StartScene(this),
        11 => _trayScene ??= new TrayScene(this),
        12 => _statusScene ??= new StatusScene(this),
        13 => _clockScene ??= new ClockScene(this),
        _ => null,
    };

    private IEnumerable<ModuleScene> Scenes() => new[] { _startScene, _trayScene, _statusScene, _clockScene }.OfType<ModuleScene>();

    // A page was opened: its scene comes in (the popup pops out of its entry), the others rest.
    private void EnterModulePage(int page)
    {
        var scene = SceneFor(page);
        foreach (var other in Scenes()) if (other != scene) other.Leave();
        scene?.Enter();
    }

    private void LeaveModulePages()
    {
        foreach (var scene in Scenes()) scene.Leave();
    }

    private void StopModules()
    {
        foreach (var scene in Scenes()) scene.Stop();
    }

    // The settings or the picture changed.
    private void ShowModulePreview(int page) => SceneFor(page)?.Refresh();

    private void OnModuleStageSizeChanged(object sender, SizeChangedEventArgs e)
    {
        if (Math.Abs(e.NewSize.Width - e.PreviousSize.Width) + Math.Abs(e.NewSize.Height - e.PreviousSize.Height) > 0.5) SceneFor(_page)?.Resize();
    }

    // The bar preview's apps whose icons are read.
    private ImageSource[] LoadedIcons() => _previewPins.Select(p => p.Icon).OfType<ImageSource>().ToArray();

    // ---- Motion (native/core/motion.h) ----

    // motion::Spring: response (s) and damping ratio, stepped in closed form.
    private readonly record struct Spring(double Response, double Damping)
    {
        public void Step(ref double x, ref double v, double target, double dt)
        {
            double w0 = 2 * Math.PI / Math.Max(Response, 0.01), z = Math.Max(Damping, 0), y0 = x - target, v0 = v, y, vy;
            if (Math.Abs(z - 1) < 1e-3)
            {
                double e = Math.Exp(-w0 * dt), b = v0 + w0 * y0;
                y = e * (y0 + b * dt);
                vy = e * (v0 - w0 * dt * b);
            }
            else if (z < 1)
            {
                double a = z * w0, wd = w0 * Math.Sqrt(1 - z * z), e = Math.Exp(-a * dt), c = Math.Cos(wd * dt), s = Math.Sin(wd * dt);
                y = e * (y0 * c + (v0 + a * y0) / wd * s);
                vy = e * (v0 * c - (a * v0 + w0 * w0 * y0) / wd * s);
            }
            else
            {
                double q = w0 * Math.Sqrt(z * z - 1), r1 = -z * w0 + q, r2 = -z * w0 - q, c2 = (v0 - r1 * y0) / (r2 - r1), c1 = y0 - c2;
                double e1 = Math.Exp(r1 * dt), e2 = Math.Exp(r2 * dt);
                y = c1 * e1 + c2 * e2;
                vy = c1 * r1 * e1 + c2 * r2 * e2;
            }
            x = target + y;
            v = vy;
        }
    }

    private static readonly Spring ResizeSpring = new(0.40, 0.82); // StartMenu's kResize
    private static readonly Spring SmoothSpring = new(0.48, 0.77); // motion::kSmooth: the status ring's level

    // motion::Approach: closes the share `rate` of the distance per second, whatever the frame time.
    private static double Approach(double v, double target, double dtMs, double rate) => v + (target - v) * (1 - Math.Exp(-dtMs / 1000 * rate));

    // ---- The popups' ink (Theme::For) over light glass (dark ink) or dark glass ----

    private sealed class PanelInk
    {
        public readonly SolidColorBrush Text = new(), Sub = new(), Faint = new(), Hover = new(), Divider = new(), Card = new(), Border = new(),
                                        Wash45 = new(), Search = new(), Track = new(), Quiet = new(), Level = new(), OnFill = new(),
                                        Disabled = new(), DisabledTrack = new(), DisabledGlyph = new(), Chevron = new(), Ring = new();
        public bool? Dark;

        public static Color A(Color c, double a) => Color.FromArgb((byte)Math.Round(c.A * Math.Clamp(a, 0, 1)), c.R, c.G, c.B);

        public void Set(bool dark)
        {
            if (Dark == dark) return;
            Dark = dark;
            Color ink = dark ? Color.FromArgb(255, 0x11, 0x13, 0x17) : Colors.White, wash = dark ? Colors.Black : Colors.White;
            Text.Color = A(ink, dark ? 0.92 : 0.96);
            Sub.Color = A(ink, dark ? 0.66 : 0.76);
            Faint.Color = A(ink, dark ? 0.46 : 0.56);
            Hover.Color = A(wash, dark ? 0.10 : 0.20);
            Divider.Color = A(wash, dark ? 0.12 : 0.24);
            Card.Color = A(wash, dark ? 0.05 : 0.08);
            Border.Color = A(wash, dark ? 0.10 : 0.30);
            Wash45.Color = A(Hover.Color, 0.45);
            Search.Color = A(Card.Color, 0.6);
            Track.Color = A(Text.Color, 0.14);
            Quiet.Color = A(Text.Color, 0.16);
            Ring.Color = A(Text.Color, 0.18);
            Level.Color = A(Text.Color, 0.9);
            OnFill.Color = dark ? Color.FromArgb(247, 255, 255, 255) : Color.FromArgb(247, 18, 18, 23);
            Disabled.Color = A(Text.Color, 0.4);
            DisabledTrack.Color = A(Text.Color, 0.08);
            DisabledGlyph.Color = A(Text.Color, 0.35);
            Chevron.Color = A(Text.Color, 0.08);
        }
    }

    private static readonly SolidColorBrush AccentBrush = new(Color.FromArgb(255, 0x0A, 0x84, 0xFF));
    private static readonly SolidColorBrush WhiteInk = new(Color.FromArgb(250, 255, 255, 255));

    // ---- Marks: text and symbols at the core's sizes, in boxes as Canvas::Text places them ----

    private static TextBlock Words(double size, Brush brush, bool bold = false, TextAlignment align = TextAlignment.Left, bool wrap = false) => new()
    {
        FontSize = size, Foreground = brush, TextAlignment = align, IsTextScaleFactorEnabled = false, IsHitTestVisible = false,
        TextWrapping = wrap ? TextWrapping.Wrap : TextWrapping.NoWrap, TextTrimming = TextTrimming.CharacterEllipsis, MaxLines = wrap ? 2 : 1,
        FontFamily = (FontFamily)Application.Current.Resources[bold ? "TitleFont" : "TextFont"],
    };

    private static TextBlock Symbol(string glyph, double size, Brush brush) => new()
    {
        Text = glyph, FontSize = size, Foreground = brush, TextAlignment = TextAlignment.Center, IsTextScaleFactorEnabled = false, IsHitTestVisible = false,
        FontFamily = new FontFamily("Segoe Fluent Icons"),
    };

    // In the box (x, y, w, h): one line centred on its height (DWrite's centred paragraph); wrapping
    // text from its top.
    private static void Box(TextBlock t, double x, double y, double w, double h)
    {
        t.Width = Math.Max(0, w);
        Canvas.SetLeft(t, x);
        Canvas.SetTop(t, t.TextWrapping == TextWrapping.NoWrap ? y + (h - TextMetrics.Line(t)) / 2 : y);
    }

    private static Border Plate(Brush fill, double radius) => new() { Background = fill, CornerRadius = new CornerRadius(radius), IsHitTestVisible = false };

    private static void Put(FrameworkElement e, double x, double y, double w, double h)
    {
        e.Width = Math.Max(0, w);
        e.Height = Math.Max(0, h);
        Canvas.SetLeft(e, x);
        Canvas.SetTop(e, y);
    }

    private static readonly Dictionary<(string, double, bool), double> TextWidths = new();

    // The width of a line of text (Canvas::TextWidth).
    private static double TextWidth(string text, double size, bool bold = false)
    {
        if (TextWidths.TryGetValue((text, size, bold), out double w)) return w;
        var t = Words(size, AccentBrush, bold);
        t.Text = text;
        t.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
        return TextWidths[(text, size, bold)] = t.DesiredSize.Width;
    }

    // A tile's icon, drawn here: a rounded square in its colour, its symbol in white.
    private static Grid DrawnIcon(string glyph, uint top, uint bottom)
    {
        static Color Rgb(uint c) => Color.FromArgb(255, (byte)(c >> 16), (byte)(c >> 8), (byte)c);
        var fill = new LinearGradientBrush { StartPoint = new Point(0.5, 0), EndPoint = new Point(0.5, 1) };
        fill.GradientStops.Add(new GradientStop { Color = Rgb(top), Offset = 0 });
        fill.GradientStops.Add(new GradientStop { Color = Rgb(bottom), Offset = 1 });
        var g = new Grid { Width = 40, Height = 40, IsHitTestVisible = false };
        g.Children.Add(new Border { Background = fill, CornerRadius = new CornerRadius(10) });
        g.Children.Add(new FontIcon { Glyph = glyph, FontSize = 19, Foreground = WhiteInk, FontFamily = new FontFamily("Segoe Fluent Icons") });
        return g;
    }

    private static Path RingPath(double width) => new()
    {
        IsHitTestVisible = false, StrokeThickness = width, StrokeStartLineCap = PenLineCap.Round, StrokeEndLineCap = PenLineCap.Round,
    };

    // ---- A page's scene: the picture, the bar and the popup above it ----

    private sealed record GlassJob(int Id, double W, double H, double Rs, bool Low, double Scale, Lazy<Photo> Photo, int PhotoGeneration, BarSettings Material,
                                   GlassTile[] Tiles, object State);
    private sealed record GlassFrame(int Id, GlassTile[] Tiles, GlassOverlay[] Overlays, double Rs, bool Low, double Ms, object State);

    private abstract class ModuleScene
    {
        private const double BarGlideMs = 320, PopupGap = 12; // kPopupGapDip: a popup over the bar

        protected readonly MainWindow M;
        private readonly Border _stage;
        private readonly Canvas _root;
        private readonly Image _barGlass = new() { Stretch = Stretch.Fill, IsHitTestVisible = false };
        private readonly Canvas _barLayer = new() { IsHitTestVisible = false };
        protected readonly BarFace Face;
        private readonly Canvas _pop = new() { IsHitTestVisible = false };
        protected readonly Canvas Marks = new() { IsHitTestVisible = false };
        private readonly ScaleTransform _marksScale = new();
        private readonly List<Image> _glass = new();
        private readonly LatestWorker<GlassJob> _worker;

        protected double W, H, S;
        protected BarGeometry? Bar;
        private BarGeometry? _barFrom, _barTo;
        private double _barT = 1;
        private bool _barDark;
        protected bool Active;
        protected double Now;
        private bool _ticking, _open, _popPending;
        private long _last;
        private int _posted, _applied;
        private string _postedKey = "";
        private bool _postedLow;
        // In motion the glass is rendered at a share of the resolution, smaller if a frame took long.
        private static readonly double[] MotionScales = { 0.5, 0.35 };
        private int _motion;
        private object? _shown;

        // Render thread: the pictures (sharp, and at the motion resolutions), the glass of the last
        // frame, and the bar's at the motion resolutions, ready before anything moves.
        private readonly Dictionary<string, Backdrop> _backdrops = new();
        private Dictionary<(GlassTile, GlassSpec, string), GlassOverlay> _cache = new(), _warm = new();

        protected ModuleScene(MainWindow m, Border stage, Canvas root)
        {
            M = m;
            _stage = stage;
            _root = root;
            root.Children.Add(_barGlass);
            root.Children.Add(_barLayer);
            root.Children.Add(_pop);
            _pop.Children.Add(Marks);
            Face = new BarFace(_barLayer, MaxPreviewApps);
            foreach (var icon in Face.Icons) icon.SetBinding(Image.SourceProperty, new Binding { Path = new PropertyPath(nameof(PinVm.Icon)) });
            Marks.RenderTransform = _marksScale;
            _worker = new LatestWorker<GlassJob>(Render);
        }

        // ---- What a scene says ----

        protected abstract bool IsOpen { get; }                        // the popup is there (its entry is on the bar)
        protected abstract Size PopupSize { get; }                     // its largest size (DIP), to fit the scene
        protected abstract (GlassTile[] Tiles, object State) Pieces(); // its glass on this frame, in the stage's DIP
        protected abstract Vector2 Anchor { get; }                     // where it pops out of
        protected abstract void Ink(int piece, bool dark);
        protected virtual bool Step(double dt) => false;               // its shapes move
        protected abstract bool Arrange(object state, double dt);     // lays its marks; true while they move
        protected virtual void Entered() { }
        protected virtual void Refreshed() { }
        protected virtual void PoppedIn() { }
        protected virtual void Left() { }
        protected virtual StatusLook BarStatus() => StatusLook.Current();

        protected bool HasFrame => _shown != null;
        protected double BarHeight => M._current.Height;
        protected double Radius => Math.Clamp(M._current.CornerRadius, 8, Math.Max(8, M._current.Height / 2));
        protected double PopupBottom => Bar!.BarY - PopupGap * S;
        protected double BarRight => Math.Min(Bar!.BarX + Bar.BarW, W - 16); // PopupRect, right-aligned with the bar

        // The marks' origin (stage DIP); inside, real DIP.
        protected void PlaceMarks(double x, double y)
        {
            _marksScale.ScaleX = _marksScale.ScaleY = S;
            Canvas.SetLeft(Marks, x);
            Canvas.SetTop(Marks, y);
        }

        // ---- The page comes and goes ----

        public void Enter()
        {
            Active = true;
            if (ContinuousCorners.Original(_stage) is not ImageBrush) _stage.Background = M.PhotoBrush();
            _open = IsOpen;
            var visual = ElementCompositionPreview.GetElementVisual(_pop);
            visual.StopAnimation("Scale");
            visual.StopAnimation("Opacity");
            visual.Opacity = 0;
            _popPending = _open;
            _shown = null;
            _postedKey = "";
            Entered();
            if (Fit()) Kick();
        }

        public void Leave()
        {
            if (!Active) return;
            Active = false;
            StopTicking();
            Left();
        }

        public void Stop()
        {
            Leave();
            _worker.Stop();
        }

        public void Resize()
        {
            if (!Active) return;
            _postedKey = "";
            if (Fit()) Kick();
        }

        public void Refresh()
        {
            if (!Active) return;
            if (ContinuousCorners.Original(_stage) is not ImageBrush) _stage.Background = M.PhotoBrush();
            if (Bar == null || Math.Abs(W - _stage.ActualWidth) > 0.5 || Math.Abs(H - _stage.ActualHeight) > 0.5 || Math.Abs(S - FitScale()) > 1e-6)
            {
                if (!Fit())
                {
                    _open = IsOpen;
                    _popPending = _open;
                    Refreshed();
                    return;
                }
            }
            else
            {
                var target = M.BarLayout(M._current, W, H, S, false);
                if (!HasFrame) (Bar, _barTo, _barT) = (target, target, 1); // nothing shown yet: no glide
                else if (target != _barTo)
                {
                    _barFrom = Bar;
                    _barTo = target;
                    _barT = 0;
                }
            }
            bool open = IsOpen;
            if (open != _open)
            {
                _open = open;
                if (open) PopIn();
                else PopOut();
            }
            Refreshed();
            _postedKey = ""; // the material or the picture may have changed
            Kick();
        }

        private double FitScale()
        {
            var s = M._current;
            var need = PopupSize;
            double w = _stage.ActualWidth, h = _stage.ActualHeight;
            return Math.Min(1.0, Math.Min(Math.Min((w - 48) / (HeroReferenceW + 2 * GlassSpec.InsetDip), (w - 32) / need.Width),
                                          (h - 20) / (need.Height + PopupGap + s.Height + s.Margin)));
        }

        private bool Fit()
        {
            double w = _stage.ActualWidth, h = _stage.ActualHeight;
            if (w < 120 || h < 120) return false;
            W = w;
            H = h;
            S = FitScale();
            _root.Width = _pop.Width = w;
            _root.Height = _pop.Height = h;
            _root.Clip = new RectangleGeometry { Rect = new Rect(0, 0, w, h) };
            Bar = _barTo = M.BarLayout(M._current, w, h, S, false);
            _barFrom = null;
            _barT = 1;
            return true;
        }

        // ---- Frames ----

        protected void Kick()
        {
            if (!Active || _ticking) return;
            _ticking = true;
            _last = Environment.TickCount64;
            CompositionTarget.Rendering += Tick;
        }

        private void StopTicking()
        {
            if (!_ticking) return;
            _ticking = false;
            CompositionTarget.Rendering -= Tick;
        }

        private void Tick(object? sender, object e)
        {
            long t = Environment.TickCount64;
            double dt = Math.Clamp(t - _last, 0, 100);
            _last = t;
            Now = t;
            bool moving = false;
            if (_barT < 1 && _barFrom != null && _barTo != null)
            {
                _barT = Math.Min(1, _barT + dt / BarGlideMs);
                Bar = Lerp(_barFrom, _barTo, Motion.EaseOut(_barT));
                moving = true;
            }
            moving |= Step(dt);
            Post(moving);
            bool marks = _shown != null && Arrange(_shown, dt);
            if (!moving && !marks && _applied == _posted && !_postedLow) StopTicking();
        }

        private static GlassTile BarTile(BarGeometry L) => new(L.BarX, L.BarY, L.BarW, L.BarH, L.Radius);

        // The glass for this frame, unless it is the one already on its way; while shapes move at a
        // lower resolution (so it keeps up with the frames), sharp again once they rest.
        private void Post(bool moving)
        {
            if (Bar == null) return;
            var (tiles, state) = Pieces();
            var all = new GlassTile[tiles.Length + 1];
            all[0] = BarTile(Bar);
            tiles.CopyTo(all, 1);
            double rs = Math.Clamp(M.Content.XamlRoot?.RasterizationScale ?? 1, 1, 2);
            if (moving && !_postedLow) _motion = 1; // a motion starts on the smaller one: the first frames come at once
            double r = moving ? rs * MotionScales[_motion] : rs;
            string key = string.Join(";", all) + $"|{r}|{W}x{H}|{Photo.Generation}|{GlassSpec.For(M._current, S * rs)}";
            if (key == _postedKey) return;
            _postedKey = key;
            _postedLow = moving;
            _worker.Post(new GlassJob(++_posted, W, H, r, moving, S, Photo.Capture(), Photo.Generation, M._current.Clone(), all, (Bar, state)));
        }

        private void Render(GlassJob j)
        {
            long start = System.Diagnostics.Stopwatch.GetTimestamp();
            if (Picture(j, j.Rs) is not { } picture) return;
            var (bg, key) = picture;
            var spec = GlassSpec.For(j.Material, j.Scale * j.Rs);
            var next = new Dictionary<(GlassTile, GlassSpec, string), GlassOverlay>();
            var overlays = new GlassOverlay[j.Tiles.Length];
            for (int i = 0; i < j.Tiles.Length; i++)
            {
                var k = (j.Tiles[i], spec, key);
                if (!_cache.TryGetValue(k, out var o) && !_warm.TryGetValue(k, out o))
                    o = GlassRenderer.CardOverlays(bg, j.Material, j.Rs, new[] { j.Tiles[i] }, j.Scale)[0];
                overlays[i] = next[k] = o;
            }
            _cache = next;
            var frame = new GlassFrame(j.Id, j.Tiles, overlays, j.Rs, j.Low, System.Diagnostics.Stopwatch.GetElapsedTime(start).TotalMilliseconds, j.State);
            M.DispatcherQueue.TryEnqueue(() => Apply(frame));

            // At rest: the pictures and the bar at the motion resolutions, so that a motion starts at once.
            if (j.Low) return;
            var warm = new Dictionary<(GlassTile, GlassSpec, string), GlassOverlay>();
            foreach (double m in MotionScales)
            {
                double r = j.Rs * m;
                if (Picture(j, r) is not { } low) continue;
                var k = (j.Tiles[0], GlassSpec.For(j.Material, j.Scale * r), low.Item2);
                warm[k] = _warm.TryGetValue(k, out var o) ? o : GlassRenderer.CardOverlays(low.Item1, j.Material, r, new[] { j.Tiles[0] }, j.Scale)[0];
            }
            _warm = warm;
        }

        private (Backdrop, string)? Picture(GlassJob j, double rs)
        {
            int pw = (int)Math.Round(j.W * rs), ph = (int)Math.Round(j.H * rs);
            if (pw < 8 || ph < 8) return null;
            string key = $"{pw}x{ph}/{j.W:F1}x{j.H:F1}/{j.PhotoGeneration}";
            if (!_backdrops.TryGetValue(key, out var bg))
            {
                if (_backdrops.Count > MotionScales.Length) _backdrops.Clear();
                bg = _backdrops[key] = Backdrop.Create(pw, ph, j.W, j.H, 0, 0, rs, j.Photo.Value);
            }
            return (bg, key);
        }

        private bool DarkInk(float luma) => M._current.TextColor == "dark" || (M._current.TextColor == "auto" && luma > 0.5f);

        private void Apply(GlassFrame f)
        {
            if (!Active || f.Id < _applied) return;
            _applied = f.Id;
            if (f.Low) _motion = _motion == 0 && f.Ms > 16 ? 1 : _motion == 1 && f.Ms < 7 ? 0 : _motion;
            Show(_barGlass, f.Tiles[0], f.Overlays[0], f.Rs);
            _barDark = DarkInk(f.Overlays[0].Luma);
            for (int i = 1; i < f.Tiles.Length; i++)
            {
                while (_glass.Count < i)
                {
                    var image = new Image { Stretch = Stretch.Fill, IsHitTestVisible = false };
                    _pop.Children.Insert(_glass.Count, image);
                    _glass.Add(image);
                }
                Show(_glass[i - 1], f.Tiles[i], f.Overlays[i], f.Rs);
                Ink(i - 1, DarkInk(f.Overlays[i].Luma));
            }
            var (bar, state) = ((BarGeometry, object))f.State;
            LayoutBar(bar);
            _shown = state;
            Arrange(state, 0);
            if (_popPending)
            {
                _popPending = false;
                PopIn();
            }
            Kick();
        }

        private static void Show(Image image, GlassTile t, GlassOverlay o, double rs)
        {
            if (image.Source is not WriteableBitmap bmp || bmp.PixelWidth != o.Width || bmp.PixelHeight != o.Height)
            {
                bmp = new WriteableBitmap(o.Width, o.Height);
                image.Source = bmp;
            }
            using (var stream = bmp.PixelBuffer.AsStream()) stream.Write(o.Pixels, 0, o.Pixels.Length);
            bmp.Invalidate();
            double w = o.Width / rs, h = o.Height / rs;
            image.Width = w;
            image.Height = h;
            Canvas.SetLeft(image, t.X + t.W / 2 - w / 2);
            Canvas.SetTop(image, t.Y + t.H / 2 - h / 2);
        }

        private void LayoutBar(BarGeometry L)
        {
            int running = M._current.ShowRunning && L.N > M._previewPins.Length ? M._previewPins.Length : -1;
            for (int i = 0; i < MaxPreviewApps; i++)
            {
                var vm = i < M._previewPins.Length ? M._previewPins[i] : M._previewRunning;
                if (Face.Icons[i].DataContext != vm) Face.Icons[i].DataContext = vm;
            }
            SetBarClock();
            // No pointer on the bar: no magnification, no hover plate.
            Face.Layout(L, _barDark, L.N, running, 2, 1, 0, 0, BarStatus());
        }

        protected void SetBarClock()
        {
            var clock = DateTime.Now;
            Face.SetClock(clock.ToString("H:mm"), clock.ToString("yyyy/M/d"));
        }

        // The bar between two layouts: everything glides; an entry that comes or goes does at once.
        private static BarGeometry Lerp(BarGeometry a, BarGeometry b, double t)
        {
            double Mix(double x, double y) => x + (y - x) * t;
            double Slot(double x, double y) => x < 0 || y < 0 ? y : x + (y - x) * t;
            return b with
            {
                BaseX = Mix(a.BaseX, b.BaseX), BarX = Mix(a.BarX, b.BarX), BarY = Mix(a.BarY, b.BarY), BarW = Mix(a.BarW, b.BarW), BarH = Mix(a.BarH, b.BarH),
                Radius = Mix(a.Radius, b.Radius), Icon = Mix(a.Icon, b.Icon), P = Mix(a.P, b.P), PlateR = Mix(a.PlateR, b.PlateR),
                AppsLeft = Mix(a.AppsLeft, b.AppsLeft), Pitch = Mix(a.Pitch, b.Pitch), PlateY = Mix(a.PlateY, b.PlateY),
                ClockX = Slot(a.ClockX, b.ClockX), ClockW = Mix(a.ClockW, b.ClockW), SepX = Slot(a.SepX, b.SepX), StartX = Mix(a.StartX, b.StartX),
                TrayX = Slot(a.TrayX, b.TrayX), StatusX = Slot(a.StatusX, b.StatusX), BellX = Slot(a.BellX, b.BellX), StatusExtra = Mix(a.StatusExtra, b.StatusExtra),
            };
        }

        protected void UpdateBarStatus(StatusLook look) => Face.ShowStatus(look);

        // ---- The popup pops out of its entry (PopVisual) and back ----

        private void PopIn()
        {
            var visual = ElementCompositionPreview.GetElementVisual(_pop);
            var c = visual.Compositor;
            visual.CenterPoint = new Vector3(Anchor, 0);
            visual.StopAnimation("Scale");
            visual.StopAnimation("Opacity");
            visual.Scale = new Vector3(0.92f, 0.92f, 1);
            visual.Opacity = 0;
            var scale = c.CreateVector3KeyFrameAnimation();
            scale.Duration = TimeSpan.FromSeconds(0.6);
            scale.InsertKeyFrame(0, new Vector3(0.92f, 0.92f, 1));
            NotificationFrames(c, 0.6, PopSample, (time, value, ease) =>
                scale.InsertKeyFrame(time, new Vector3((float)(0.92 + 0.08 * value), (float)(0.92 + 0.08 * value), 1), ease));
            scale.InsertKeyFrame(1, Vector3.One);
            visual.StartAnimation("Scale", scale);
            var fade = c.CreateScalarKeyFrameAnimation();
            fade.Duration = TimeSpan.FromSeconds(0.16);
            fade.InsertKeyFrame(0, 0);
            fade.InsertKeyFrame(1, 1, c.CreateCubicBezierEasingFunction(new Vector2(1f / 3, 1), new Vector2(2f / 3, 1)));
            visual.StartAnimation("Opacity", fade);
            Now = Environment.TickCount64;
            PoppedIn();
            Kick();
        }

        private void PopOut()
        {
            var visual = ElementCompositionPreview.GetElementVisual(_pop);
            var c = visual.Compositor;
            visual.CenterPoint = new Vector3(Anchor, 0);
            visual.StopAnimation("Scale");
            visual.StopAnimation("Opacity");
            var ease = c.CreateCubicBezierEasingFunction(new Vector2(1f / 3, 1), new Vector2(2f / 3, 1));
            var scale = c.CreateVector3KeyFrameAnimation();
            scale.Duration = TimeSpan.FromSeconds(0.13);
            scale.InsertKeyFrame(1, new Vector3(0.95f, 0.95f, 1), ease);
            visual.StartAnimation("Scale", scale);
            var fade = c.CreateScalarKeyFrameAnimation();
            fade.Duration = TimeSpan.FromSeconds(0.12);
            fade.InsertKeyFrame(1, 0, ease);
            visual.StartAnimation("Opacity", fade);
        }
    }

    // ---- The start menu: the panel and its capsule, from the bar's left end ----

    private sealed class StartScene : ModuleScene
    {
        private static readonly int[] Cols = { 7, 9, 11 }, Rows = { 3, 4, 5 }; // StartLayout::kCols, kRows
        private const double CellW = 72, CellH = 88, GridTop = 86, GridPad = 28, Foot = 42, Gap = 12;
        // Its tiles, drawn here: a name, a symbol, a colour.
        private static readonly (string Name, string Glyph, uint Top, uint Bottom)[] Apps =
        {
            ("文件资源管理器", "\uE8B7", 0xFFD66B, 0xF2A922), ("浏览器", "\uE774", 0x5AC8FA, 0x0A84FF), ("邮件", "\uE715", 0x6AAEFF, 0x2D6BEA),
            ("日历", "\uE787", 0xFF7A70, 0xE5383B), ("照片", "\uEB9F", 0xFFB86B, 0xF0623A), ("音乐", "\uE8D6", 0xFF6F91, 0xE0245E),
            ("设置", "\uE713", 0xA7AFBC, 0x67707D), ("记事本", "\uE70B", 0x7DD3C0, 0x2BA58C), ("计算器", "\uE8EF", 0x8E9AAF, 0x4A5568),
            ("地图", "\uE707", 0x7CDB8A, 0x30B158), ("相机", "\uE722", 0x9DA6B5, 0x515A68), ("时钟", "\uE823", 0xB39DFF, 0x6E56CF),
        };

        private readonly PanelInk _panel = new(), _shelf = new();
        private readonly Border _search, _all;
        private readonly TextBlock _searchGlyph, _hint, _allGlyph, _allLabel;
        private readonly Rectangle _caret = new() { Fill = AccentBrush, IsHitTestVisible = false, Opacity = 0.9 };
        private readonly Grid[] _icons;
        private readonly TextBlock[] _names;
        private readonly double[] _x, _y;
        private bool _placed;
        private double _staged = double.MaxValue;
        // The panel's size, springing (kResize) to the chosen one.
        private int _size = -1;
        private double _pw, _pv, _mh, _mv;
        // The capsule.
        private readonly Ellipse _avatar = new() { Fill = new SolidColorBrush(Color.FromArgb(230, 0x0A, 0x84, 0xFF)), IsHitTestVisible = false };
        private readonly TextBlock _initial = Words(13, new SolidColorBrush(Colors.White), true, TextAlignment.Center), _user;
        private readonly Rectangle _divider = new() { IsHitTestVisible = false };
        private readonly Shelf[] _buttons;
        private double _dividerA, _dividerX;

        // A round button of the capsule; Right: from the capsule's right end to its own (gliding).
        private sealed class Shelf
        {
            public required string Id;
            public required FrameworkElement Mark;
            public TextBlock? Label;
            public double Right, Alpha, Width;
            public bool On, Placed;
        }

        private sealed record Frame(double Left, double Bottom, double PanelW, double MainH);

        public StartScene(MainWindow m) : base(m, m.StartStage, m.StartCanvas)
        {
            _search = Plate(_panel.Search, 22);
            _search.BorderBrush = _panel.Border;
            _search.BorderThickness = new Thickness(1);
            _all = Plate(_panel.Wash45, 22);
            _searchGlyph = Symbol("\uE721", 16, _panel.Sub);
            _hint = Words(15, _panel.Faint);
            _hint.Text = "搜索应用和文件夹";
            _allGlyph = Symbol("\uE71D", 14, _panel.Text);
            _allLabel = Words(13, _panel.Text);
            _allLabel.Text = "全部应用";
            foreach (var e in new UIElement[] { _search, _searchGlyph, _hint, _caret, _all, _allGlyph, _allLabel }) Marks.Children.Add(e);
            _icons = new Grid[Apps.Length];
            _names = new TextBlock[Apps.Length];
            _x = new double[Apps.Length];
            _y = new double[Apps.Length];
            for (int i = 0; i < Apps.Length; i++)
            {
                _icons[i] = DrawnIcon(Apps[i].Glyph, Apps[i].Top, Apps[i].Bottom);
                _names[i] = Words(11.5, _panel.Text, false, TextAlignment.Center, true);
                _names[i].Text = Apps[i].Name;
                Marks.Children.Add(_icons[i]);
                Marks.Children.Add(_names[i]);
            }
            _user = Words(14, _shelf.Text, true);
            _user.Text = "用户";
            _initial.Text = "用";
            _divider.Fill = _shelf.Divider;
            foreach (var e in new UIElement[] { _avatar, _initial, _user, _divider }) Marks.Children.Add(e);
            _buttons = new[]
            {
                new Shelf { Id = "power", Mark = Symbol("\uE7E8", 16, _shelf.Text) },
                new Shelf { Id = "tray", Mark = Symbol("\uE70E", 16, _shelf.Text) },
                new Shelf { Id = "battery", Mark = Symbol("", 16, _shelf.Text), Label = Words(12, _shelf.Sub) },
                new Shelf { Id = "volume", Mark = Symbol("\uE994", 16, _shelf.Text) },
                new Shelf { Id = "network", Mark = new WifiFan() },
            };
            foreach (var b in _buttons)
            {
                Marks.Children.Add(b.Mark);
                if (b.Label != null) Marks.Children.Add(b.Label);
            }
        }

        private static Size Panel(int size) => new(Cols[size] * CellW + 2 * GridPad, GridTop + Rows[size] * CellH + Foot);

        private int Chosen => M._current.StartSize switch { "small" => 0, "large" => 2, _ => 1 };
        protected override bool IsOpen => true;
        protected override Size PopupSize => new(Panel(2).Width, Panel(2).Height + Gap + BarHeight);
        protected override Vector2 Anchor => new((float)(Bar!.StartX + Bar.P / 2), (float)PopupBottom);

        // PopupRect: on the bar's left end, inside the stage, for the window of the largest size; the
        // menu stands on the window's bottom left corner.
        private double MenuLeft => Math.Clamp(Bar!.BarX, 16, Math.Max(16, W - 16 - Panel(2).Width * S));

        protected override void Entered()
        {
            _size = Chosen;
            (_pw, _mh) = (Panel(_size).Width, Panel(_size).Height);
            _pv = _mv = 0;
            _placed = false;
            foreach (var b in _buttons) b.Placed = false;
            _dividerA = -1;
            _staged = double.MaxValue; // the tiles come in with the menu
        }

        protected override void PoppedIn() => _staged = Now;

        // The size chosen: the panel springs there (Step); before the glass is first shown, it is there at once.
        protected override void Refreshed()
        {
            if (!HasFrame) Entered();
            _size = Chosen;
        }

        protected override bool Step(double dt)
        {
            var target = Panel(_size);
            if (_pw == target.Width && _mh == target.Height && _pv == 0 && _mv == 0) return false;
            ResizeSpring.Step(ref _pw, ref _pv, target.Width, dt / 1000);
            ResizeSpring.Step(ref _mh, ref _mv, target.Height, dt / 1000);
            if (Math.Abs(_pw - target.Width) < 0.25 && Math.Abs(_mh - target.Height) < 0.25 && Math.Abs(_pv) < 2 && Math.Abs(_mv) < 2)
            {
                (_pw, _mh) = (target.Width, target.Height);
                _pv = _mv = 0;
            }
            return true;
        }

        protected override (GlassTile[] Tiles, object State) Pieces()
        {
            double left = MenuLeft, bottom = PopupBottom, bh = BarHeight;
            return (new[]
            {
                new GlassTile(left, bottom - (_mh + Gap + bh) * S, _pw * S, _mh * S, Radius * S),
                new GlassTile(left, bottom - bh * S, _pw * S, bh * S, bh * S / 2),
            }, new Frame(left, bottom, _pw, _mh));
        }

        protected override void Ink(int piece, bool dark) => (piece == 0 ? _panel : _shelf).Set(dark);

        protected override bool Arrange(object state, double dt)
        {
            var f = (Frame)state;
            PlaceMarks(f.Left, f.Bottom);
            double bh = BarHeight, top = -(f.MainH + Gap + bh), w = f.PanelW;
            bool moving = false;

            // Search (PaintSearch), and the way to all the apps beside it.
            double allL = w - 24 - 108;
            Put(_search, 24, top + 20, allL - 8 - 24, 44);
            Box(_searchGlyph, 34, top + 20, 34, 44);
            Box(_hint, 24 + 46, top + 20, Math.Max(0, allL - 8 - 48 - 70), 44);
            Put(_caret, 24 + 46 + 1 - 0.75, top + 32, 1.5, 20);
            Put(_all, allL, top + 20, 108, 44);
            double tw = TextWidth("全部应用", 13), gx = allL + (108 - (16 + 6 + tw)) / 2;
            Box(_allGlyph, gx, top + 20, 16, 44);
            Box(_allLabel, gx + 22, top + 20, tw + 4, 44);

            // The tiles (PaintTiles): their cells for the chosen size, gliding there.
            int cols = Cols[_size];
            double gridX = (w - cols * CellW) / 2;
            for (int i = 0; i < Apps.Length; i++)
            {
                double hx = gridX + i % cols * CellW, hy = GridTop + i / cols * CellH;
                if (!_placed) (_x[i], _y[i]) = (hx, hy);
                _x[i] = Approach(_x[i], hx, dt, 16);
                _y[i] = Approach(_y[i], hy, dt, 16);
                if (Math.Abs(_x[i] - hx) < 0.3) _x[i] = hx; else moving = true;
                if (Math.Abs(_y[i] - hy) < 0.3) _y[i] = hy; else moving = true;
                // In one after another (kStageMs 260, kStaggerMs 12), rising 10 DIP.
                double e = Motion.EaseOut((Now - _staged - 12 * Math.Min(i, 18)) / 260);
                if (e < 1) moving = true;
                double x = _x[i], y = top + _y[i] + 10 * (1 - e);
                Put(_icons[i], x + CellW / 2 - 20, y + 8, 40, 40);
                Box(_names[i], x + 2, y + 52, CellW - 4, 38);
                _icons[i].Opacity = _names[i].Opacity = e;
            }
            _placed = true;
            moving |= ArrangeShelf(w, bh, dt);
            return moving;
        }

        // The capsule (PaintShelf): the account on the left; from the right, power, the tray (startTray),
        // battery, volume and network (startStatus), round buttons concentric with the capsule.
        private bool ArrangeShelf(double w, double barH, double dt)
        {
            double bh = Math.Min(44, barH - 12), edge = (barH - bh) / 2, cy = -barH / 2;
            var s = M._current;
            var look = StatusLook.Current();
            bool moving = false;
            double r = edge, dividerAt = -1;
            foreach (var b in _buttons)
            {
                b.On = b.Id switch
                {
                    "tray" => s.StartTray,
                    "battery" => s.StartStatus && look.Battery,
                    "volume" or "network" => s.StartStatus,
                    _ => true,
                };
                if (b.Id == "battery" && look.Battery)
                {
                    string label = $"{look.Percent}%";
                    b.Label!.Text = label;
                    ((TextBlock)b.Mark).Text = ((char)((look.Charging ? 0xEBAB : 0xEBA0) + Math.Clamp(look.Percent / 10, 0, 10))).ToString();
                    b.Width = 12 + 20 + 4 + TextWidth(label, 12) + 4 + 12;
                }
                else b.Width = bh;
                if (b.Id == "tray" && b.On)
                {
                    dividerAt = r + 4;
                    r += 8;
                }
                double target = r;
                if (b.On) r += b.Width + 2;
                if (!b.Placed) (b.Right, b.Alpha, b.Placed) = (target, b.On ? 1 : 0, true);
                if (b.On) b.Right = Approach(b.Right, target, dt, 16);
                b.Alpha = Approach(b.Alpha, b.On ? 1 : 0, dt, b.On ? 20 : 11);
                if (Math.Abs(b.Right - target) < 0.3) b.Right = target; else if (b.On) moving = true;
                if (Math.Abs(b.Alpha - (b.On ? 1 : 0)) < 0.01) b.Alpha = b.On ? 1 : 0; else moving = true;

                double left = w - b.Right - b.Width;
                b.Mark.Opacity = b.Alpha;
                b.Mark.Visibility = b.Alpha > 0.005 ? Visibility.Visible : Visibility.Collapsed;
                if (b.Mark is WifiFan fan)
                {
                    var on = _shelf.Text.Color;
                    fan.Update(left + bh / 2, cy, 12, 3, on, PanelInk.A(on, 0.28));
                }
                else Box((TextBlock)b.Mark, b.Label != null ? left + 10 : left, cy - bh / 2, b.Label != null ? 24 : bh, bh);
                if (b.Label != null)
                {
                    b.Label.Opacity = b.Alpha;
                    b.Label.Visibility = b.Mark.Visibility;
                    Box(b.Label, left + 36, cy - bh / 2, Math.Max(0, b.Width - 44), bh);
                }
            }
            // The tray's divider, before its button.
            bool divider = dividerAt >= 0;
            if (_dividerA < 0) (_dividerA, _dividerX) = (divider ? 1 : 0, Math.Max(dividerAt, 0));
            if (divider) _dividerX = Approach(_dividerX, dividerAt, dt, 16);
            _dividerA = Approach(_dividerA, divider ? 1 : 0, dt, divider ? 20 : 11);
            if (Math.Abs(_dividerA - (divider ? 1 : 0)) < 0.01) _dividerA = divider ? 1 : 0; else moving = true;
            if (divider && Math.Abs(_dividerX - dividerAt) >= 0.3) moving = true;
            _divider.Opacity = _dividerA;
            Put(_divider, w - _dividerX - 0.5, cy - 12, 1, 24);

            // The account, in what is left.
            double x0 = edge, room = w - r;
            double nw = Math.Clamp(TextWidth(_user.Text, 14, true), 0, Math.Max(0, room - x0 - 70));
            Put(_avatar, x0 + 8, cy - 14, 28, 28);
            Box(_initial, x0 + 8, cy - 14, 28, 28);
            Box(_user, x0 + 36 + 10, cy - barH / 2, nw + 2, barH);
            return moving;
        }
    }

    // ---- The tray: its panel over the bar's right end ----

    private sealed class TrayScene : ModuleScene
    {
        private const double Cell = 40, IconSize = 20, Air = (Cell - IconSize) / 2, Split = 14, FooterH = 24;
        private const int Count = 13, ColsN = 5, RowsN = 3; // TrayPanel::Cols: 13 icons in rows of 5, 5 and 3
        private static readonly string[] Glyphs = { "\uE753", "\uE702", "\uEA18", "\uE88E", "\uE8BD", "\uE7F4" };
        private readonly PanelInk _ink = new();
        private readonly FrameworkElement?[] _items = new FrameworkElement?[Count];
        private readonly Rectangle _divider = new() { IsHitTestVisible = false };
        private readonly TextBlock _count, _system;
        private readonly Border _button;
        private double _opened = double.MaxValue;

        private sealed record Frame(double Left, double Top);

        public TrayScene(MainWindow m) : base(m, m.TrayStage, m.TrayCanvas)
        {
            _divider.Fill = _ink.Divider;
            _count = Words(12, _ink.Sub);
            _count.Text = $"{Count} 个图标";
            _button = Plate(_ink.Wash45, 12);
            _system = Words(11.5, _ink.Text, false, TextAlignment.Center);
            _system.Text = "系统托盘";
            foreach (var e in new UIElement[] { _divider, _count, _button, _system }) Marks.Children.Add(e);
        }

        private double Margin => Math.Clamp(Radius * 0.65, 14, 22);
        private Size Panel => new(2 * (Margin - Air) + ColsN * Cell, (Margin - Air) + RowsN * Cell - Air + 2 * Split + FooterH + Margin);
        protected override bool IsOpen => M._current.ShowTray;
        protected override Size PopupSize => Panel;
        protected override Vector2 Anchor => new((float)(Bar!.TrayX >= 0 ? Bar.TrayX + Bar.P / 2 : BarRight), (float)PopupBottom);

        protected override (GlassTile[] Tiles, object State) Pieces()
        {
            double left = BarRight - Panel.Width * S, top = PopupBottom - Panel.Height * S;
            return (new[] { new GlassTile(left, top, Panel.Width * S, Panel.Height * S, Radius * S) }, new Frame(left, top));
        }

        protected override void Ink(int piece, bool dark) => _ink.Set(dark);
        protected override void Entered() => _opened = double.MaxValue;
        protected override void PoppedIn() => _opened = Now;

        protected override bool Arrange(object state, double dt)
        {
            var f = (Frame)state;
            PlaceMarks(f.Left, f.Top);
            double m = Margin, pad = m - Air, w = Panel.Width, h = Panel.Height;
            // The icons: the bar's apps (their own icons) between one-colour symbols in the panel's ink.
            var icons = M.LoadedIcons();
            bool moving = false;
            for (int i = 0; i < Count; i++)
            {
                bool app = i % 2 == 0 && icons.Length > 0;
                if (_items[i] == null || (_items[i] is Image) != app)
                {
                    if (_items[i] != null) Marks.Children.Remove(_items[i]);
                    _items[i] = app ? new Image { Stretch = Stretch.Uniform, IsHitTestVisible = false } : Symbol(Glyphs[i / 2 % Glyphs.Length], 16, _ink.Text);
                    Marks.Children.Add(_items[i]);
                }
                var item = _items[i]!;
                if (item is Image image) image.Source = icons[i / 2 % icons.Length];
                double cx = pad + i % ColsN * Cell + Cell / 2, cy = pad + i / ColsN * Cell + Cell / 2;
                // In one after another (kStageMs 240, kStaggerMs 14), rising 6 DIP.
                double e = Motion.EaseOut((Now - _opened - 14 * Math.Min(i, 18)) / 240);
                if (e < 1) moving = true;
                cy += 6 * (1 - e);
                if (item is TextBlock t) Box(t, cx - IconSize / 2, cy - IconSize / 2, IconSize, IconSize);
                else Put(item, cx - IconSize / 2, cy - IconSize / 2, IconSize, IconSize);
                item.Opacity = e;
            }
            // Below them: the count, and the system's own tray.
            double footTop = h - m - FooterH, divider = footTop - Split;
            Put(_divider, m, divider, w - 2 * m, 1);
            double bw = TextWidth("系统托盘", 11.5) + 20;
            Box(_count, m, footTop, w - m - bw - 10 - m, FooterH);
            Put(_button, w - m - bw, footTop, bw, FooterH);
            Box(_system, w - m - bw, footTop, bw, FooterH);
            return moving;
        }
    }

    // ---- The status entry: the control centre it opens, and the ring as it follows the levels ----

    private sealed class StatusScene : ModuleScene
    {
        private const double CW = 340, CGap = 10, TopH = 150, SliderH = 68, StageH = 58, Pad = 14;
        private const double Loop = 12000, LevelShowMs = 1500;
        private readonly PanelInk[] _inks = { new(), new(), new(), new(), new(), new() };
        // Connections.
        private readonly Ellipse[] _discs = { new() { Fill = AccentBrush, IsHitTestVisible = false }, new() { Fill = AccentBrush, IsHitTestVisible = false } };
        private readonly WifiFan _wifi = new();
        private readonly TextBlock _bt = Symbol("\uE702", 15, WhiteInk);
        private readonly TextBlock[] _rowTitle = new TextBlock[2], _rowSub = new TextBlock[2];
        // Battery (or power), casting, Stage Manager.
        private readonly Path _battTrack = RingPath(4), _battLevel = RingPath(4);
        private readonly Ellipse _powerDisc = new() { IsHitTestVisible = false }, _castDisc = new() { IsHitTestVisible = false }, _stageDisc = new() { IsHitTestVisible = false };
        private readonly TextBlock _battGlyph, _battTitle, _battSub, _castGlyph, _castTitle, _castSub, _stageGlyph, _stageTitle, _stageSub;
        // The sliders.
        private readonly LevelRow _bright, _sound;
        private readonly Border _chevron;
        private readonly TextBlock _chevronGlyph;
        // The levels the demonstration moves, and the ring's motion (BarPainter's StatusMotion).
        private double _start, _volumeUntil, _brightUntil, _volumeMix, _brightMix, _fill = -1, _speed;
        private int _volume = 40, _brightness = 45;

        private sealed record Frame(double Left, double Top);

        private sealed class LevelRow
        {
            public required TextBlock Title, Value, Glyph;
            public required Border Track, Fill;
        }

        public StatusScene(MainWindow m) : base(m, m.StatusStage, m.StatusCanvas)
        {
            var conn = _inks[0];
            foreach (var d in _discs) Marks.Children.Add(d);
            Marks.Children.Add(_wifi);
            Marks.Children.Add(_bt);
            for (int i = 0; i < 2; i++)
            {
                Marks.Children.Add(_rowTitle[i] = Words(13.5, conn.Text, true));
                Marks.Children.Add(_rowSub[i] = Words(11.5, conn.Sub));
            }
            _rowTitle[0].Text = "Wi-Fi";
            _rowSub[0].Text = "已连接";
            _rowTitle[1].Text = "蓝牙";
            _rowSub[1].Text = "已打开";
            var batt = _inks[1];
            _battTrack.Stroke = batt.Ring;
            _powerDisc.Fill = batt.Quiet;
            _battGlyph = Symbol("\uE945", 11, batt.Text);
            _battTitle = Words(15, batt.Text, true);
            _battSub = Words(11.5, batt.Sub);
            foreach (var e in new UIElement[] { _powerDisc, _battTrack, _battLevel, _battGlyph, _battTitle, _battSub }) Marks.Children.Add(e);
            var cast = _inks[2];
            _castDisc.Fill = cast.Quiet;
            _castGlyph = Symbol("\uE7F4", 13, cast.Text);
            _castTitle = Words(13.5, cast.Text, true);
            _castTitle.Text = "投屏";
            _castSub = Words(11.5, cast.Sub);
            _castSub.Text = "无线显示器";
            foreach (var e in new UIElement[] { _castDisc, _castGlyph, _castTitle, _castSub }) Marks.Children.Add(e);
            var stage = _inks[3];
            _stageGlyph = Symbol("\uE7C4", 14, stage.Text);
            _stageTitle = Words(13.5, stage.Text, true);
            _stageTitle.Text = "台前调度";
            _stageSub = Words(11.5, stage.Sub);
            foreach (var e in new UIElement[] { _stageDisc, _stageGlyph, _stageTitle, _stageSub }) Marks.Children.Add(e);
            _bright = NewRow(_inks[4], "显示器", "\uE706");
            _sound = NewRow(_inks[5], "声音", "\uE994");
            _chevron = Plate(_inks[5].Chevron, 12);
            _chevronGlyph = Symbol("\uE76C", 9.5, _inks[5].Text);
            Marks.Children.Add(_chevron);
            Marks.Children.Add(_chevronGlyph);
        }

        private LevelRow NewRow(PanelInk ink, string title, string glyph)
        {
            var row = new LevelRow
            {
                Title = Words(12.5, ink.Text, true), Value = Words(11.5, ink.Sub, false, TextAlignment.Right), Glyph = Symbol(glyph, 11, ink.OnFill),
                Track = Plate(ink.Track, 12), Fill = Plate(ink.Level, 12),
            };
            row.Title.Text = title;
            foreach (var e in new UIElement[] { row.Title, row.Value, row.Track, row.Fill, row.Glyph }) Marks.Children.Add(e);
            return row;
        }

        private static double MainH => TopH + CGap + StageH + CGap + SliderH + CGap + SliderH; // ControlCenter::MainHeight
        protected override bool IsOpen => M._current.ShowStatus;
        protected override Size PopupSize => new(CW, MainH);
        protected override Vector2 Anchor => new((float)(Bar!.StatusX >= 0 ? Bar.StatusX + (Bar.P + Bar.StatusExtra) / 2 : BarRight), (float)PopupBottom);

        // ControlCenter::MainShapes: connections; battery over casting; Stage Manager; brightness; sound.
        private static readonly Rect[] Shapes =
        {
            new(0, 0, (CW - CGap) / 2, TopH),
            new((CW - CGap) / 2 + CGap, 0, (CW - CGap) / 2, (TopH - CGap) / 2),
            new((CW - CGap) / 2 + CGap, (TopH - CGap) / 2 + CGap, (CW - CGap) / 2, (TopH - CGap) / 2),
            new(0, TopH + CGap, CW, StageH),
            new(0, TopH + CGap + StageH + CGap, CW, SliderH),
            new(0, TopH + CGap + StageH + CGap + SliderH + CGap, CW, SliderH),
        };

        protected override (GlassTile[] Tiles, object State) Pieces()
        {
            double left = BarRight - CW * S, top = PopupBottom - MainH * S, r = Math.Min(Radius, 22) * S;
            return (Shapes.Select(s => new GlassTile(left + s.X * S, top + s.Y * S, s.Width * S, s.Height * S, r)).ToArray(), new Frame(left, top));
        }

        protected override void Ink(int piece, bool dark) => _inks[piece].Set(dark);

        protected override void Entered()
        {
            _start = Environment.TickCount64;
            _volumeUntil = _brightUntil = _volumeMix = _brightMix = 0;
            _fill = -1;
            _speed = 0;
            _volume = 40;
            _brightness = 45;
        }

        // The demonstration: the volume up, the brightness up, the volume down, the brightness down,
        // as dragging the sliders does; each change shows on the ring for a while.
        private static double Ramp(double t, double at, double from, double to)
        {
            double u = Math.Clamp((t - at) / 700, 0, 1);
            return from + (to - from) * u * u * (3 - 2 * u);
        }

        protected override StatusLook BarStatus() => Look();

        private StatusLook Look()
        {
            var real = StatusLook.Current();
            return real with { Fill = _fill < 0 ? real.Fill : _fill, Volume = _volume, VolumeMix = _volumeMix, BrightMix = _brightMix };
        }

        protected override bool Arrange(object state, double dt)
        {
            var f = (Frame)state;
            PlaceMarks(f.Left, f.Top);
            var real = StatusLook.Current();
            bool laptop = real.Battery;
            if (IsOpen)
            {
                double t = (Now - _start) % Loop;
                int volume = (int)Math.Round(t < 4000 ? Ramp(t, 800, 40, 80) : Ramp(t, 6400, 80, 40));
                int brightness = laptop ? (int)Math.Round(t < 7000 ? Ramp(t, 3600, 45, 85) : Ramp(t, 9200, 85, 45)) : 45;
                if (volume != _volume)
                {
                    _volumeUntil = Now + LevelShowMs;
                    _brightUntil = 0;
                }
                if (brightness != _brightness)
                {
                    _brightUntil = Now + LevelShowMs;
                    _volumeUntil = 0;
                }
                (_volume, _brightness) = (volume, brightness);
            }
            bool showVolume = Now < _volumeUntil, showBright = Now < _brightUntil;
            _volumeMix = Approach(_volumeMix, showVolume ? 1 : 0, dt, 12);
            _brightMix = Approach(_brightMix, showBright ? 1 : 0, dt, 12);
            double target = showBright ? _brightness / 100.0 : (showVolume || !laptop) ? _volume / 100.0 : real.Fill;
            if (_fill < 0) _fill = target;
            SmoothSpring.Step(ref _fill, ref _speed, target, dt / 1000);
            UpdateBarStatus(Look());

            ArrangeConnections();
            ArrangeTiles(real);
            ArrangeRow(_bright, Shapes[4], _brightness, laptop, laptop ? $"{_brightness}%" : "不支持调节", _inks[4], false);
            _sound.Glyph.Text = StatusGlyph.VolumeGlyph(Look());
            ArrangeRow(_sound, Shapes[5], _volume, true, $"{_volume}%", _inks[5], true);
            return IsOpen || _volumeMix > 0.001 || _brightMix > 0.001 || Math.Abs(_speed) > 0.001;
        }

        private void ArrangeConnections()
        {
            var c = Shapes[0];
            double rh = (TopH - 16) / 2;
            for (int i = 0; i < 2; i++)
            {
                double cy = c.Y + 8 + i * rh + rh / 2;
                Put(_discs[i], c.X + 14, cy - 18, 36, 36);
                Box(_rowTitle[i], c.X + 60, cy - 18, c.Width - 70, 18);
                Box(_rowSub[i], c.X + 60, cy, c.Width - 70, 18);
                if (i == 0) _wifi.Update(c.X + 32, cy, 13, 3, WhiteInk.Color, WhiteInk.Color);
                else Box(_bt, c.X + 14, cy - 18, 36, 36);
            }
        }

        private void ArrangeTiles(StatusLook real)
        {
            // Battery: a small ring, the level and the state; a desktop's power instead.
            var b = Shapes[1];
            var ink = _inks[1];
            double cx = b.X + 32, cy = b.Y + b.Height / 2;
            _powerDisc.Visibility = real.Battery ? Visibility.Collapsed : Visibility.Visible;
            _battTrack.Visibility = _battLevel.Visibility = real.Battery ? Visibility.Visible : Visibility.Collapsed;
            if (real.Battery)
            {
                int pct = Math.Clamp(real.Percent, 0, 100);
                _battTrack.Data = new EllipseGeometry { Center = new Point(cx, cy), RadiusX = 15, RadiusY = 15 };
                _battLevel.Data = StatusGlyph.Arc(cx, cy, 15, -90, Math.Max(0.01, Math.Min(359.99, 360 * pct / 100.0)));
                _battLevel.Stroke = real.Charging ? new SolidColorBrush(Color.FromArgb(255, 0x30, 0xD1, 0x58))
                                  : pct <= 20 ? new SolidColorBrush(Color.FromArgb(255, 0xFF, 0x45, 0x3A)) : ink.Text;
                _battGlyph.Visibility = real.Charging ? Visibility.Visible : Visibility.Collapsed;
                _battGlyph.FontSize = 11;
                Box(_battGlyph, cx - 12, cy - 12, 24, 24);
                _battTitle.FontSize = 15;
                _battTitle.Text = $"{pct}%";
                _battSub.Text = real.Charging ? (pct >= 100 ? "已充满" : "正在充电") : "使用电池";
            }
            else
            {
                Put(_powerDisc, cx - 16, cy - 16, 32, 32);
                _battGlyph.Visibility = Visibility.Visible;
                _battGlyph.FontSize = 13;
                Box(_battGlyph, cx - 14, cy - 14, 28, 28);
                _battTitle.FontSize = 13.5;
                _battTitle.Text = "电源";
                _battSub.Text = "已接通电源";
            }
            Box(_battTitle, cx + 24, cy - 18, b.Right - 8 - cx - 24, 19);
            Box(_battSub, cx + 24, cy + 1, b.Right - 8 - cx - 24, 17);

            // Casting.
            var k = Shapes[2];
            double kx = k.X + 32, ky = k.Y + k.Height / 2;
            Put(_castDisc, kx - 16, ky - 16, 32, 32);
            Box(_castGlyph, kx - 14, ky - 14, 28, 28);
            Box(_castTitle, kx + 24, ky - 18, k.Right - 8 - kx - 24, 19);
            Box(_castSub, kx + 24, ky + 1, k.Right - 8 - kx - 24, 17);

            // Stage Manager: its symbol lit while on.
            var s = Shapes[3];
            var stageInk = _inks[3];
            bool on = M._current.StageManager;
            double sx = s.X + 32, sy = s.Y + s.Height / 2;
            Put(_stageDisc, sx - 18, sy - 18, 36, 36);
            _stageDisc.Fill = on ? AccentBrush : stageInk.Quiet;
            _stageGlyph.Foreground = on ? WhiteInk : stageInk.Text;
            Box(_stageGlyph, sx - 14, sy - 14, 28, 28);
            _stageSub.Text = on ? (M._current.StageOnRight ? "已打开 · 最近使用的应用在右侧" : "已打开 · 最近使用的应用在左侧") : "已关闭";
            Box(_stageTitle, sx + 28, sy - 18, s.Right - Pad - sx - 28, 18);
            Box(_stageSub, sx + 28, sy, s.Right - Pad - sx - 28, 18);
        }

        // PaintSlider: the title, the value on the right, a wide pill filled from the left with the symbol at its start.
        private void ArrangeRow(LevelRow row, Rect card, int value, bool enabled, string right, PanelInk ink, bool more)
        {
            row.Title.Foreground = enabled ? ink.Text : ink.Disabled;
            row.Value.Foreground = enabled ? ink.Sub : ink.Disabled;
            row.Value.Text = right;
            Box(row.Title, card.X + Pad, card.Y + 8, card.Width - 130 - Pad, 20);
            Box(row.Value, card.Right - 130, card.Y + 8, 130 - Pad - (more ? 30 : 0), 20);
            var track = new Rect(card.X + Pad, card.Y + 34, card.Width - 2 * Pad, 24);
            Put(row.Track, track.X, track.Y, track.Width, track.Height);
            row.Track.Background = enabled ? ink.Track : ink.DisabledTrack;
            row.Fill.Visibility = enabled ? Visibility.Visible : Visibility.Collapsed;
            double k = Math.Clamp(value, 0, 100) / 100.0;
            Put(row.Fill, track.X, track.Y, track.Height + (track.Width - track.Height) * k, track.Height);
            row.Glyph.Foreground = enabled ? ink.OnFill : ink.DisabledGlyph;
            Box(row.Glyph, track.X + 2, track.Y, track.Height, track.Height);
            if (more)
            {
                Put(_chevron, card.Right - Pad - 24, card.Y + 6, 24, 24);
                Box(_chevronGlyph, card.Right - Pad - 24, card.Y + 6, 24, 24);
            }
        }
    }

    // ---- The time: the month over its capsule, from the clock ----

    private sealed class ClockScene : ModuleScene
    {
        private const double CW = 336, Pad = 14, MonthH = 48, WeekH = 26, CellH = 46, Footer = 40, CapGap = 10;
        private static readonly string[] WeekNames = { "一", "二", "三", "四", "五", "六", "日" };
        private readonly PanelInk _card = new(), _cap = new();
        private readonly TextBlock _title, _prev, _next, _settings, _time, _date, _lunar;
        private readonly TextBlock[] _week = new TextBlock[7], _days = new TextBlock[42], _labels = new TextBlock[42];
        private readonly Border _today = Plate(AccentBrush, 12);
        private readonly Rectangle _line = new() { IsHitTestVisible = false };
        private readonly Microsoft.UI.Dispatching.DispatcherQueueTimer _tick;

        private sealed record Frame(double Left, double Top);

        public ClockScene(MainWindow m) : base(m, m.ClockStage, m.ClockCanvas)
        {
            _title = Words(16, _card.Text, true);
            _prev = Symbol("\uE76B", 11, _card.Text);
            _next = Symbol("\uE76C", 11, _card.Text);
            _settings = Words(12.5, _card.Sub, false, TextAlignment.Center);
            _settings.Text = "日期和时间设置";
            _line.Fill = _card.Divider;
            Marks.Children.Add(_today);
            foreach (var e in new UIElement[] { _title, _prev, _next, _line, _settings }) Marks.Children.Add(e);
            for (int i = 0; i < 7; i++) Marks.Children.Add(_week[i] = Words(12, _card.Faint, false, TextAlignment.Center));
            for (int i = 0; i < 42; i++)
            {
                Marks.Children.Add(_days[i] = Words(14.5, _card.Text, false, TextAlignment.Center));
                Marks.Children.Add(_labels[i] = Words(9.5, _card.Sub, false, TextAlignment.Center));
            }
            _time = Words(20, _cap.Text, true);
            _date = Words(13, _cap.Text, false, TextAlignment.Right);
            _lunar = Words(11.5, _cap.Sub, false, TextAlignment.Right);
            Microsoft.UI.Xaml.Documents.Typography.SetNumeralAlignment(_time, FontNumeralAlignment.Tabular);
            foreach (var e in new UIElement[] { _time, _date, _lunar }) Marks.Children.Add(e);
            _tick = m.DispatcherQueue.CreateTimer();
            _tick.Interval = TimeSpan.FromSeconds(1);
            _tick.Tick += (_, _) => Kick(); // the seconds
        }

        private static double CardH => MonthH + WeekH + 6 * CellH + 8 + Footer;
        protected override bool IsOpen => M._current.ShowClock;
        protected override Size PopupSize => new(CW, CardH + CapGap + BarHeight);
        protected override Vector2 Anchor => new((float)(Bar!.ClockX >= 0 ? Bar.ClockX + Bar.ClockW / 2 : BarRight), (float)PopupBottom);
        protected override void Entered() => _tick.Start();
        protected override void Left() => _tick.Stop();

        protected override (GlassTile[] Tiles, object State) Pieces()
        {
            double bh = BarHeight, left = BarRight - CW * S, top = PopupBottom - (CardH + CapGap + bh) * S;
            return (new[]
            {
                new GlassTile(left, top, CW * S, CardH * S, Radius * S),
                new GlassTile(left, top + (CardH + CapGap) * S, CW * S, bh * S, bh * S / 2),
            }, new Frame(left, top));
        }

        protected override void Ink(int piece, bool dark) => (piece == 0 ? _card : _cap).Set(dark);

        protected override bool Arrange(object state, double dt)
        {
            var f = (Frame)state;
            PlaceMarks(f.Left, f.Top);
            var now = DateTime.Now;
            // The month and its buttons (PaintMonth).
            _title.Text = $"{now.Year}年{now.Month}月";
            Box(_title, Pad + 6, 0, CW / 2 + 20 - Pad - 6, MonthH);
            double by = MonthH / 2;
            Box(_next, CW - Pad - 34, by - 17, 34, 34);
            Box(_prev, CW - Pad - 34 - 38, by - 17, 34, 34);

            // The weekdays from the user's first one, then six weeks from the week holding the 1st.
            int first = ((int)System.Globalization.CultureInfo.CurrentCulture.DateTimeFormat.FirstDayOfWeek + 6) % 7; // 0 = Monday
            double cellW = (CW - 2 * Pad) / 7, gridTop = MonthH + WeekH;
            for (int i = 0; i < 7; i++)
            {
                int wd = (first + i) % 7;
                _week[i].Text = WeekNames[wd];
                _week[i].Foreground = wd >= 5 ? _card.Sub : _card.Faint;
                Box(_week[i], Pad + i * cellW, MonthH, cellW, WeekH);
            }
            var firstDay = new DateTime(now.Year, now.Month, 1);
            int lead = (((int)firstDay.DayOfWeek + 6) % 7 - first + 7) % 7;
            var day = firstDay.AddDays(-lead);
            _today.Visibility = Visibility.Collapsed;
            for (int i = 0; i < 42; i++, day = day.AddDays(1))
            {
                double x = Pad + i % 7 * cellW, y = gridTop + i / 7 * CellH;
                var cell = new Rect(x + 2, y + 2, cellW - 4, CellH - 4);
                bool inMonth = day.Month == now.Month, today = day.Date == now.Date;
                double fade = inMonth || today ? 1 : 0.38;
                string label = WidgetFaces.DayLabel(day, out bool festival);
                _days[i].Text = day.Day.ToString();
                _days[i].FontWeight = today ? Microsoft.UI.Text.FontWeights.SemiBold : Microsoft.UI.Text.FontWeights.Normal;
                _days[i].Foreground = today ? WhiteInk : _card.Text;
                _labels[i].Text = label;
                _labels[i].Foreground = today ? WhiteInk : festival ? AccentBrush : _card.Sub;
                _days[i].Opacity = _labels[i].Opacity = fade;
                Box(_days[i], cell.X, cell.Y + 3, cell.Width, 21);
                Box(_labels[i], cell.X - 2, cell.Y + 23, cell.Width + 4, cell.Height - 25);
                if (today)
                {
                    _today.Visibility = Visibility.Visible;
                    Put(_today, cell.X, cell.Y, cell.Width, cell.Height);
                }
            }

            // The system's settings.
            double fy = gridTop + 6 * CellH + 8;
            Put(_line, Pad + 8, fy, CW - 2 * Pad - 16, 1);
            Box(_settings, Pad, fy + 5, TextWidth("日期和时间设置", 12.5) + 28, Footer - 10);

            // The capsule (PaintCapsule): the time with its seconds, the day and its lunar date.
            double bh = BarHeight, top = CardH + CapGap, edge = Math.Max(16, bh / 2 - 4);
            _time.Text = now.ToString("HH:mm:ss");
            double tw = TextWidth("00:00:00", 20, true);
            Box(_time, edge, top, tw + 4, bh);
            _date.Text = $"{now.Month}月{now.Day}日 星期{WeekNames[((int)now.DayOfWeek + 6) % 7]}";
            _lunar.Text = WidgetFaces.LunarLine(now);
            double tl = edge + tw + 16, mid = top + bh / 2;
            Box(_date, tl, mid - 19, CW - edge - tl, 20);
            Box(_lunar, tl, mid, CW - edge - tl, 18);
            SetBarClock(); // the bar's clock, by the minute
            return false;
        }
    }
}
