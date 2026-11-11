using System.Numerics;
using System.Text.Json.Nodes;
using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Composition;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Hosting;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using Windows.UI;

namespace Atrium.Settings;

// The workspace previews other than the bar: the shape's numbers, the notification cards as they
// pop up, and the desktop's widget layout (read from the core's widgets.json, laid out by its rules).
public sealed partial class MainWindow
{
    // ---- Shape: the geometry, read out ----

    private readonly List<(TextBlock Value, TextBlock Note)> _readout = new();

    private void UpdateReadout()
    {
        if (_page != 1) return;
        if (_readout.Count == 0)
            foreach (var name in new[] { "高度", "圆角", "宽度", "轮廓" })
            {
                var value = new TextBlock { Style = StyleOf("TitleStyle") };
                Microsoft.UI.Xaml.Documents.Typography.SetNumeralAlignment(value, Microsoft.UI.Xaml.FontNumeralAlignment.Tabular);
                var note = Text("", "FootnoteStyle");
                var cell = new StackPanel { Spacing = 2 };
                cell.Children.Add(Text(name, "RowNoteStyle"));
                cell.Children.Add(value);
                cell.Children.Add(note);
                ShapeReadout.Children.Add(cell);
                _readout.Add((value, note));
            }
        var s = _current;
        double r = Math.Min(s.CornerRadius, s.Height / 2);
        bool capsule = s.CornerRadius >= s.Height / 2;
        _readout[0].Value.Text = $"{s.Height:0} px";
        _readout[0].Note.Text = $"图标 {s.IconSize:0} px";
        _readout[1].Value.Text = $"{r:0} px";
        _readout[1].Note.Text = s.CornerRadius > r ? $"设为 {s.CornerRadius:0}，不超过高度的一半" : capsule ? "G3 连续胶囊轮廓" : "G3 连续圆角";
        _readout[2].Value.Text = s.AutoWidth ? "自动" : $"{s.Width:0} px";
        _readout[2].Note.Text = s.AutoWidth ? $"最宽 {s.Width:0} px" : "固定宽度";
        _readout[3].Value.Text = capsule ? "胶囊形" : r < 1 ? "直角" : "圆角矩形";
        _readout[3].Note.Text = $"距底边 {s.Margin:0} px";
    }

    // Notification previews follow the selected live layout and its existing motion curves.

    private const double CardW = 384, CardH = 112, CardPeek = 10, CardInset = 12;
    private bool _notifyDark, _notifyBackDark, _notifyPreviewReady, _notifyPreviewRendered;
    private bool _notifyEntrancePending;

    private void ShowNotifyPreview(bool animate)
    {
        if (ContinuousCorners.Original(NotifyStage) is not ImageBrush) NotifyStage.Background = PhotoBrush();
        var s = _current;
        double radius = Math.Min(Math.Clamp(s.CornerRadius, 8, Math.Max(8, s.Height / 2)), CardH / 2);
        double W = NotifyStage.ActualWidth, H = NotifyStage.ActualHeight;
        if (W < CardW + 48 || H < CardH + 60) { W = Math.Max(W, CardW + 48); H = Math.Max(H, CardH + 60); }
        bool topRight = s.NotifyPlacement == "topRight";
        // Over the bar, right-aligned with it (the bell's popup); or the screen's top right corner.
        var bar = ShowNotifyBar(W, H);
        double barRight = Math.Min(bar.BarX + bar.BarW, W - 16);
        var front = new GlassTile(topRight ? W - 24 - CardW : barRight - CardW, topRight ? 24 : bar.BarY - 12 * bar.S - CardH, CardW, CardH, radius);
        var back = topRight ? new GlassTile(front.X, front.Y + CardH + CardPeek, CardW, CardH, radius)
            : new GlassTile(front.X + CardInset, front.Y - CardPeek, CardW - 2 * CardInset, CardH, radius);

        NotifyCards.Width = W;
        NotifyCards.Height = H;
        NotifyPreviewClip.Rect = new Rect(0, 0, W, H);
        if (!_notifyPreviewReady)
        {
            ElementCompositionPreview.GetElementVisual(NotifyCards).Opacity = 0;
            _notifyPreviewReady = true;
        }
        var backCard = NotifyBackCard;
        var frontCard = NotifyFrontCard;
        const double pad = GlassSpec.InsetDip;
        backCard.Width = back.W + 2 * pad; backCard.Height = back.H + 2 * pad;
        frontCard.Width = front.W + 2 * pad; frontCard.Height = front.H + 2 * pad;
        UpdateNotifyText(s);
        NotifyBackText.Visibility = topRight ? Visibility.Visible : Visibility.Collapsed;
        Canvas.SetLeft(backCard, back.X - pad); Canvas.SetTop(backCard, back.Y - pad);
        Canvas.SetLeft(frontCard, front.X - pad); Canvas.SetTop(frontCard, front.Y - pad);
        _notifyEntrancePending |= animate;
        NotifyCaption.Text = (topRight ? "主屏右上角 · 已有通知向下让位 · " : "跟随任务栏 · ") + $"{s.NotifyTimeout:0} 秒后收起" + (s.NotifyHideBody ? " · 正文已隐藏" : "");

        // Their glass: rendered over the picture where they rest; each card carries its own piece.
        PostTiles(NotifyCards, W, H, 1, new[] { back, front }, null, f =>
        {
            NotifyBackImage.Source = f.Overlay(0);
            NotifyFrontImage.Source = f.Overlay(1);
            _notifyPreviewRendered = true;
            bool dark = _notifyDark ? f.Lumas[1] >= 0.42f : f.Lumas[1] > 0.50f;
            bool backDark = _notifyBackDark ? f.Lumas[0] >= 0.42f : f.Lumas[0] > 0.50f;
            if (dark != _notifyDark || backDark != _notifyBackDark)
            {
                _notifyDark = dark;
                _notifyBackDark = backDark;
                UpdateNotifyText(s);
            }
            if (_notifyEntrancePending)
            {
                _notifyEntrancePending = false;
                ReplayNotify();
            }
            else ElementCompositionPreview.GetElementVisual(NotifyCards).Opacity = 1;
        }, overlays: true);
    }

    private BarFace? _notifyFace;
    private readonly Image _notifyBarGlass = new() { Stretch = Stretch.Fill, IsHitTestVisible = false };

    // The bar at the bottom of the notifications' picture, as on the pages of its parts.
    private BarGeometry ShowNotifyBar(double w, double h)
    {
        if (_notifyFace == null)
        {
            NotifyBar.Children.Add(_notifyBarGlass);
            _notifyFace = new BarFace(NotifyBar, MaxPreviewApps);
            foreach (var icon in _notifyFace.Icons)
                icon.SetBinding(Image.SourceProperty, new Microsoft.UI.Xaml.Data.Binding { Path = new PropertyPath(nameof(PinVm.Icon)) });
        }
        NotifyBar.Width = w;
        NotifyBar.Height = h;
        NotifyBar.Clip = new RectangleGeometry { Rect = new Rect(0, 0, w, h) };
        double scale = Math.Min(1.0, (w - 48) / (HeroReferenceW + 2 * GlassSpec.InsetDip));
        var L = BarLayout(_current, w, h, scale, false);
        var tile = new GlassTile(L.BarX, L.BarY, L.BarW, L.BarH, L.Radius);
        PostTiles(NotifyBar, w, h, scale, new[] { tile }, null, f =>
        {
            var glass = f.Overlays![0];
            _notifyBarGlass.Source = f.Overlay(0);
            _notifyBarGlass.Width = glass.Width / f.Rs;
            _notifyBarGlass.Height = glass.Height / f.Rs;
            Canvas.SetLeft(_notifyBarGlass, tile.X + tile.W / 2 - _notifyBarGlass.Width / 2);
            Canvas.SetTop(_notifyBarGlass, tile.Y + tile.H / 2 - _notifyBarGlass.Height / 2);
            bool dark = _current.TextColor == "dark" || (_current.TextColor == "auto" && glass.Luma > 0.5f);
            int running = _current.ShowRunning && L.N > _previewPins.Length ? _previewPins.Length : -1;
            for (int i = 0; i < MaxPreviewApps; i++)
            {
                var vm = i < _previewPins.Length ? _previewPins[i] : _previewRunning;
                if (_notifyFace.Icons[i].DataContext != vm) _notifyFace.Icons[i].DataContext = vm;
            }
            var clock = DateTime.Now;
            _notifyFace.SetClock(clock.ToString("H:mm"), clock.ToString("yyyy/M/d"));
            _notifyFace.Layout(L, dark, L.N, running, 2, 1, 0, 0, StatusLook.Current());
        }, overlays: true, scaled: true);
        return L;
    }

    private void UpdateNotifyText(BarSettings settings)
    {
        NotifyFrontText.Margin = NotifyBackText.Margin = new Thickness(GlassSpec.InsetDip);
        NotifyFrontBody.Text = settings.NotifyHideBody ? "内容已隐藏" : "明天的活动时间定了吗？我这边上午都有空，下午三点以后也可以。";
        NotifyBackBody.Text = settings.NotifyHideBody ? "内容已隐藏" : "下午三点，项目讨论。";
        SetNotifyInk(_notifyDark, settings.NotifyHideBody, NotifyFrontApp, NotifyFrontTime, NotifyFrontTitle, NotifyFrontBody);
        SetNotifyInk(_notifyBackDark, settings.NotifyHideBody, NotifyBackApp, NotifyBackTime, NotifyBackTitle, NotifyBackBody);
    }

    // PaintCard: the app (12 semibold, secondary ink) after the unread dot, when (12, faint) on the right,
    // the title (14 semibold), the body (13, secondary ink, two lines) or "content hidden".
    private static void SetNotifyInk(bool dark, bool hideBody, TextBlock app, TextBlock time, TextBlock title, TextBlock body)
    {
        Color ink = dark ? Color.FromArgb(255, 0x11, 0x13, 0x17) : Colors.White;
        SolidColorBrush A(double a) => new(Color.FromArgb((byte)Math.Round(255 * a), ink.R, ink.G, ink.B));
        var text = A(dark ? 0.92 : 0.96);
        var sub = A(dark ? 0.66 : 0.76);
        var faint = A(dark ? 0.46 : 0.56);
        app.Foreground = sub;
        time.Foreground = faint;
        title.Foreground = text;
        body.Foreground = hideBody ? faint : sub;
    }

    private void OnNotifyStageSizeChanged(object sender, SizeChangedEventArgs e)
    {
        if (_page == 3 && Math.Abs(e.NewSize.Width - e.PreviousSize.Width) + Math.Abs(e.NewSize.Height - e.PreviousSize.Height) > 0.5) ShowNotifyPreview(false);
    }

    private void OnReplayNotify(object sender, RoutedEventArgs e) => ReplayNotify();

    private void ReplayNotify()
    {
        if (!_notifyPreviewRendered)
        {
            _notifyEntrancePending = true;
            return;
        }
        if (_current.NotifyPlacement == "topRight")
        {
            var visual = ElementCompositionPreview.GetElementVisual(NotifyCards);
            visual.StopAnimation("Scale");
            visual.StopAnimation("Opacity");
            visual.Scale = Vector3.One;
            visual.Opacity = 1;
            AnimateNotifyCard(NotifyBackCard, new Vector3(0, -(float)(CardH + CardPeek), 0), false);
            AnimateNotifyCard(NotifyFrontCard, new Vector3((float)CardW, 0, 0), true);
            return;
        }
        // The default popup still opens from the taskbar bell.
        var anchor = new Vector2((float)(Canvas.GetLeft(NotifyFrontCard) + NotifyFrontCard.Width - GlassSpec.InsetDip - _current.Height / 2 - 4),
                                 (float)(Canvas.GetTop(NotifyFrontCard) + NotifyFrontCard.Height - GlassSpec.InsetDip));
        ReplayNotification(NotifyCards, anchor, NotifyBackCard, NotifyFrontCard);
    }

    // PopVisual (glass_renderer.cpp) plus each new card's Glide (notifications.cpp).
    // The live notification code remains the authority; these previews use its equations.
    private static void ReplayNotification(FrameworkElement surface, Vector2 anchor, params FrameworkElement[] cards)
    {
        var visual = ElementCompositionPreview.GetElementVisual(surface);
        var compositor = visual.Compositor;
        visual.CenterPoint = new Vector3(anchor, 0);
        // Every replay is a new entrance, including clicks during the previous one.
        // Keep the existing card/glass buffers; replace only their animations.
        visual.StopAnimation("Scale");
        visual.StopAnimation("Opacity");
        visual.Scale = new Vector3(0.92f, 0.92f, 1);
        visual.Opacity = 0;
        var scale = compositor.CreateVector3KeyFrameAnimation();
        scale.Duration = TimeSpan.FromSeconds(0.6);
        scale.InsertKeyFrame(0, new Vector3(0.92f, 0.92f, 1));
        NotificationFrames(compositor, 0.6, PopSample, (time, value, ease) =>
            scale.InsertKeyFrame(time, new Vector3((float)(0.92 + 0.08 * value), (float)(0.92 + 0.08 * value), 1), ease));
        scale.InsertKeyFrame(1, Vector3.One);
        visual.StartAnimation("Scale", scale);

        var fade = compositor.CreateScalarKeyFrameAnimation();
        fade.Duration = TimeSpan.FromSeconds(0.16);
        fade.InsertKeyFrame(0, 0);
        fade.InsertKeyFrame(1, 1, compositor.CreateCubicBezierEasingFunction(new Vector2(1f / 3, 1), new Vector2(2f / 3, 1)));
        visual.StartAnimation("Opacity", fade);

        foreach (var card in cards)
            AnimateNotifyCard(card, new Vector3(0, 18, 0), true);
    }

    private static void AnimateNotifyCard(FrameworkElement card, Vector3 offset, bool fade)
    {
        ElementCompositionPreview.SetIsTranslationEnabled(card, true);
        var content = ElementCompositionPreview.GetElementVisual(card);
        var compositor = content.Compositor;
        content.StopAnimation("Translation");
        content.StopAnimation("Opacity");
        content.Properties.InsertVector3("Translation", offset);
        content.Opacity = fade ? 0 : 1;
        var move = compositor.CreateVector3KeyFrameAnimation();
        double moveTime = Math.Log(offset.Length() / 0.2) / 16; // the native card's Glide
        move.Duration = TimeSpan.FromSeconds(moveTime);
        move.InsertKeyFrame(0, offset);
        NotificationFrames(compositor, moveTime, t => ApproachSample(t, 16), (time, value, ease) =>
            move.InsertKeyFrame(time, offset * (float)(1 - value), ease));
        move.InsertKeyFrame(1, Vector3.Zero);
        content.StartAnimation("Translation", move);
        if (!fade) return;

        var opacity = compositor.CreateScalarKeyFrameAnimation();
        double fadeTime = Math.Log(1 / 0.01) / 12; // Glide(alpha, 1, dt, 12, 0.01)
        opacity.Duration = TimeSpan.FromSeconds(fadeTime);
        opacity.InsertKeyFrame(0, 0);
        NotificationFrames(compositor, fadeTime, t => ApproachSample(t, 12), (time, value, ease) =>
            opacity.InsertKeyFrame(time, (float)value, ease));
        opacity.InsertKeyFrame(1, 1);
        content.StartAnimation("Opacity", opacity);
    }

    private static (double Value, double Velocity) PopSample(double time)
    {
        const double response = 0.36, damping = 0.62; // motion::kPop
        double w = 2 * Math.PI / response, wd = w * Math.Sqrt(1 - damping * damping);
        return (Motion.Spring(time, response, damping), Math.Exp(-damping * w * time) * w * w / wd * Math.Sin(wd * time));
    }

    private static (double Value, double Velocity) ApproachSample(double time, double rate)
        => (1 - Math.Exp(-rate * time), rate * Math.Exp(-rate * time));

    // As SpringTo in glass_renderer.cpp: 12 Hermite segments, preserving both
    // position and velocity. Bezier X is linear time; Y is the same cubic.
    private static void NotificationFrames(Compositor compositor, double seconds,
        Func<double, (double Value, double Velocity)> sample, Action<float, double, CompositionEasingFunction> insert)
    {
        var previous = sample(0);
        double previousTime = 0;
        for (int i = 1; i <= 12; i++)
        {
            // The native animations snap to their target at End / Glide's epsilon.
            float progress = i == 12 ? 0.99999f : i / 12f;
            double time = progress * seconds;
            var next = sample(time);
            double delta = next.Value - previous.Value, span = time - previousTime;
            var ease = compositor.CreateCubicBezierEasingFunction(
                new Vector2(1f / 3, (float)(span * previous.Velocity / (3 * delta))),
                new Vector2(2f / 3, (float)(1 - span * next.Velocity / (3 * delta))));
            insert(progress, next.Value, ease);
            previous = next;
            previousTime = time;
        }
    }

    // ---- Widgets: the desktop, scaled down, with the widgets where the core puts them ----

    // The core's grid (native/core/ui/desktop_widgets.cpp): cells 164, gaps 16, margins 24, a row's room for the bar.
    private const double WCell = 164, WGap = 16, WMargin = 24;
    [System.Runtime.InteropServices.DllImport("user32.dll")] private static extern IntPtr MonitorFromPoint(System.Drawing.Point pt, uint flags);
    [System.Runtime.InteropServices.DllImport("shcore.dll")] private static extern int GetDpiForMonitor(IntPtr monitor, int type, out uint dpiX, out uint dpiY);
    private FileSystemWatcher? _widgetWatcher;

    private static string WidgetsPath => System.IO.Path.Combine(System.IO.Path.GetDirectoryName(CoreClient.SettingsPath)!, "widgets.json");

    private void OnWidgetStageSizeChanged(object sender, SizeChangedEventArgs e)
    {
        if (Math.Abs(e.NewSize.Width - e.PreviousSize.Width) > 0.5) ShowWidgetMap();
    }

    // A widget on the map: where it is in the core's grid, its card (its glass and its face), and
    // what the face was drawn for.
    private sealed class MapCard
    {
        public required (string Kind, string Size, int Col, int Row) Cell;
        public required Grid View;
        public required Image Glass;
        public required Viewbox Face;
        public GlassTile Tile;
        public bool Dark;
        public long Stamp;
        public string Material = "";
        public string Cities = "";
    }

    private readonly List<MapCard> _mapCards = new();
    private Image? _mapBar;
    private bool _mapShown; // the first layout appears as it is; later changes animate

    // The new layout against the shown one: the same widget in the same place stays; one of the same
    // kind elsewhere (moved, resized) glides there; the others pop in or shrink away, as on the desktop.
    private void PlaceMapCards(List<GlassTile> tiles, List<(string Kind, string Size, string[] Cities)> faces, List<(string Kind, string Size, int Col, int Row)> cells,
                               TileFrame frame, double pad)
    {
        var old = new List<MapCard>(_mapCards);
        var next = new MapCard?[tiles.Count];
        for (int i = 0; i < tiles.Count; i++)
            if (old.FirstOrDefault(c => c.Cell == cells[i]) is { } same) { next[i] = same; old.Remove(same); }
        var moved = new HashSet<MapCard>();
        for (int i = 0; i < tiles.Count; i++)
        {
            if (next[i] != null) continue;
            var cell = cells[i];
            var other = old.Where(c => c.Cell.Kind == cell.Kind)
                           .OrderBy(c => Math.Abs(c.Cell.Col - cell.Col) + Math.Abs(c.Cell.Row - cell.Row)).FirstOrDefault();
            if (other == null) continue;
            next[i] = other;
            old.Remove(other);
            moved.Add(other);
        }
        foreach (var gone in old) MapCardLeave(gone);
        _mapCards.Clear();
        for (int i = 0; i < tiles.Count; i++)
        {
            var t = tiles[i];
            var (kind, size, cities) = faces[i];
            var card = next[i];
            bool added = card == null;
            if (card == null)
            {
                var glass = new Image { Stretch = Stretch.Fill };
                var face = new Viewbox { Stretch = Stretch.Fill };
                card = new MapCard { Cell = cells[i], View = new Grid { IsHitTestVisible = false, Children = { glass, face } }, Glass = glass, Face = face };
                WidgetMap.Children.Insert(Math.Min(i, WidgetMap.Children.Count), card.View);
            }
            var before = card.Tile;
            card.Cell = cells[i];
            bool dark = frame.Lumas[i] > 0.5f;
            long stamp = WidgetFaces.Stamp(kind);
            string material = _current.WidgetMaterial;
            string cityList = string.Join("|", cities);
            if (added || moved.Contains(card) || card.Dark != dark || card.Stamp != stamp || card.Material != material || card.Cities != cityList)
            {
                card.Face.Child = WidgetFaces.Face(kind, size, dark, PhotoBrush(), material, cities);
                card.Cities = cityList;
                ContinuousCorners.Install(card.Face);
                card.Dark = dark;
                card.Stamp = stamp;
                card.Material = material;
            }
            card.Glass.Source = frame.Overlay(i);
            card.Tile = t;
            card.View.Width = t.W + 2 * pad;
            card.View.Height = t.H + 2 * pad;
            card.Face.Width = t.W;
            card.Face.Height = t.H;
            card.Face.Margin = new Thickness(pad);
            card.Face.HorizontalAlignment = HorizontalAlignment.Left;
            card.Face.VerticalAlignment = VerticalAlignment.Top;
            Canvas.SetLeft(card.View, t.X - pad);
            Canvas.SetTop(card.View, t.Y - pad);
            if (_mapShown && added) MapCardPop(card.View);
            else if (_mapShown && moved.Contains(card)) MapCardGlide(card.View, before, t, pad);
            _mapCards.Add(card);
        }
        _mapShown = true;
    }

    // A new widget: the pop of a popup (PopVisual), growing from its middle.
    private static void MapCardPop(FrameworkElement view)
    {
        var visual = ElementCompositionPreview.GetElementVisual(view);
        var compositor = visual.Compositor;
        visual.CenterPoint = new Vector3((float)view.Width / 2, (float)view.Height / 2, 0);
        var scale = compositor.CreateVector3KeyFrameAnimation();
        scale.Duration = TimeSpan.FromSeconds(0.6);
        scale.InsertKeyFrame(0, new Vector3(0.8f, 0.8f, 1));
        NotificationFrames(compositor, 0.6, PopSample, (time, value, ease) =>
            scale.InsertKeyFrame(time, new Vector3((float)(0.8 + 0.2 * value), (float)(0.8 + 0.2 * value), 1), ease));
        scale.InsertKeyFrame(1, Vector3.One);
        visual.StartAnimation("Scale", scale);
        var fade = compositor.CreateScalarKeyFrameAnimation();
        fade.Duration = TimeSpan.FromSeconds(0.2);
        fade.InsertKeyFrame(0, 0);
        fade.InsertKeyFrame(1, 1, compositor.CreateCubicBezierEasingFunction(new Vector2(1f / 3, 1), new Vector2(2f / 3, 1)));
        visual.StartAnimation("Opacity", fade);
    }

    // A removed widget shrinks and fades, then its card goes.
    private void MapCardLeave(MapCard card)
    {
        var view = card.View;
        var visual = ElementCompositionPreview.GetElementVisual(view);
        var compositor = visual.Compositor;
        visual.CenterPoint = new Vector3((float)view.Width / 2, (float)view.Height / 2, 0);
        var ease = compositor.CreateCubicBezierEasingFunction(new Vector2(0.4f, 0), new Vector2(1, 1));
        var batch = compositor.CreateScopedBatch(CompositionBatchTypes.Animation);
        var scale = compositor.CreateVector3KeyFrameAnimation();
        scale.Duration = TimeSpan.FromSeconds(0.22);
        scale.InsertKeyFrame(1, new Vector3(0.8f, 0.8f, 1), ease);
        visual.StartAnimation("Scale", scale);
        var fade = compositor.CreateScalarKeyFrameAnimation();
        fade.Duration = TimeSpan.FromSeconds(0.22);
        fade.InsertKeyFrame(1, 0, ease);
        visual.StartAnimation("Opacity", fade);
        batch.End();
        batch.Completed += (_, _) => WidgetMap.Children.Remove(view);
    }

    // A moved or resized widget glides from where it was: its card is laid out at the new place and
    // size, then moved and scaled back to the old one, and springs to rest.
    private static void MapCardGlide(FrameworkElement view, GlassTile from, GlassTile to, double pad)
    {
        if (from.W <= 0 || to.W <= 0) return;
        ElementCompositionPreview.SetIsTranslationEnabled(view, true);
        var visual = ElementCompositionPreview.GetElementVisual(view);
        var compositor = visual.Compositor;
        visual.CenterPoint = new Vector3((float)pad, (float)pad, 0); // the widget's corner, not the shadow's
        var dx = (float)(from.X - to.X);
        var dy = (float)(from.Y - to.Y);
        var sx = (float)(from.W / to.W);
        var sy = (float)(from.H / to.H);
        var move = compositor.CreateVector3KeyFrameAnimation();
        var scale = compositor.CreateVector3KeyFrameAnimation();
        move.Duration = scale.Duration = TimeSpan.FromSeconds(0.6);
        move.InsertKeyFrame(0, new Vector3(dx, dy, 0));
        scale.InsertKeyFrame(0, new Vector3(sx, sy, 1));
        NotificationFrames(compositor, 0.6, PopSample, (time, value, ease) =>
        {
            float rest = (float)(1 - value);
            move.InsertKeyFrame(time, new Vector3(dx * rest, dy * rest, 0), ease);
            scale.InsertKeyFrame(time, new Vector3(1 + (sx - 1) * rest, 1 + (sy - 1) * rest, 1), ease);
        });
        move.InsertKeyFrame(1, Vector3.Zero);
        scale.InsertKeyFrame(1, Vector3.One);
        visual.StartAnimation("Translation", move);
        visual.StartAnimation("Scale", scale);
    }

    private void ShowWidgetMap()
    {
        if (_widgetWatcher == null && Directory.Exists(System.IO.Path.GetDirectoryName(WidgetsPath)))
        {
            // The core rewrites the file when a widget is moved, resized, added or removed.
            _widgetWatcher = new FileSystemWatcher(System.IO.Path.GetDirectoryName(WidgetsPath)!, "widgets.json")
            {
                NotifyFilter = NotifyFilters.LastWrite | NotifyFilters.FileName | NotifyFilters.Size, EnableRaisingEvents = true,
            };
            FileSystemEventHandler changed = (_, _) => DispatcherQueue.TryEnqueue(() => { if (_page == 4) ShowWidgetMap(); });
            _widgetWatcher.Changed += changed;
            _widgetWatcher.Created += changed;
            _widgetWatcher.Renamed += (_, _) => DispatcherQueue.TryEnqueue(() => { if (_page == 4) ShowWidgetMap(); });
        }

        // Match the selected desktop's grid, including mixed scaling.
        var displays = DesktopDisplay.Read();
        var display = displays.First(d => d.Id == _current.DesktopDisplay ||
            (d.Primary && !displays.Any(other => other.Id == _current.DesktopDisplay)));
        double W = display.WorkWidth / display.Scale, H = display.WorkHeight / display.Scale;
        int cols = Math.Max(1, (int)((W - 2 * WMargin + WGap) / (WCell + WGap)));
        int rows = Math.Max(1, (int)((H - 2 * WMargin - 90 + WGap) / (WCell + WGap)));
        double stageW = WidgetStage.ActualWidth > 0 ? WidgetStage.ActualWidth : 760;
        WidgetStage.Height = Math.Clamp(Math.Round(stageW * H / W), 220, 560);
        double k = stageW / W, kh = WidgetStage.Height / H;
        if (ContinuousCorners.Original(WidgetStage) is not ImageBrush) WidgetStage.Background = PhotoBrush();

        // The tiles where the core puts them, then the bar at the bottom for scale; all glass.
        var tiles = new List<GlassTile>();
        var faces = new List<(string Kind, string Size, string[] Cities)>();
        var cells = new List<(string Kind, string Size, int Col, int Row)>();
        foreach (var (kind, size, col, row, cities) in ReadWidgets())
        {
            int wc = size == "small" ? 1 : 2, wr = size == "large" ? 2 : 1;
            if (col < 0 || row < 0 || col + wc > cols || row + wr > rows) continue; // the core keeps it aside on this screen
            double w = wc * WCell + (wc - 1) * WGap, h = wr * WCell + (wr - 1) * WGap;
            int visualCol = _current.WidgetsOnRight ? cols - col - wc : col;
            bool fromRight = 2 * visualCol + wc >= cols;
            double left = fromRight ? W - WMargin - (cols - visualCol - wc) * (WCell + WGap) - w : WMargin + visualCol * (WCell + WGap);
            double top = WMargin + row * (WCell + WGap);
            tiles.Add(new GlassTile(left * k, top * kh, w * k, h * kh, 22 * Math.Min(k, kh)));
            faces.Add((kind, size, cities));
            cells.Add((kind, size, col, row));
        }
        double barW = 560 * k, barH = Math.Max(4, _current.Height * kh);
        var bar = new GlassTile((stageW - barW) / 2, WidgetStage.Height - barH - Math.Max(2, _current.Margin * kh), barW, barH, barH / 2);
        int shown = tiles.Count;
        double stageH = WidgetStage.Height, kk = Math.Min(k, kh);
        int photoGeneration = Photo.Generation;
        var placed = new List<(string Kind, string Size, int Col, int Row)>(cells);
        // Each widget carries its own piece of glass, so that it can come, go and move over the picture.
        PostTiles(WidgetMap, stageW, stageH, kk, tiles.Append(bar).ToArray(), null, frame =>
        {
            frame.Apply(WidgetStage);
            // Match the bitmap's completed viewport during subsequent resizes.
            WidgetMap.Width = stageW;
            WidgetMap.Height = stageH;
            WidgetMap.Clip = new RectangleGeometry { Rect = new Rect(0, 0, stageW, stageH) };
            double pad = Math.Round(GlassSpec.InsetDip * kk * frame.Rs) / frame.Rs;
            PlaceMapCards(tiles, faces, placed, frame, pad);
            _mapBar ??= new Image { Stretch = Stretch.Fill, IsHitTestVisible = false };
            if (_mapBar.Parent == null) WidgetMap.Children.Add(_mapBar);
            _mapBar.Source = frame.Overlay(tiles.Count);
            _mapBar.Width = bar.W + 2 * pad;
            _mapBar.Height = bar.H + 2 * pad;
            Canvas.SetLeft(_mapBar, bar.X - pad);
            Canvas.SetTop(_mapBar, bar.Y - pad);
            Ready("widgets", photoGeneration);
        }, overlays: true, scaled: true);

        WidgetMapCaption.Text = !_current.Widgets ? "桌面小组件已关闭"
            : shown == 0 ? "暂无小组件"
            : $"{shown} 个小组件";
        WidgetMap.Opacity = _current.Widgets ? 1 : 0.5;
    }

    private static IEnumerable<(string Kind, string Size, int Col, int Row, string[] Cities)> ReadWidgets()
    {
        JsonNode? root = null;
        try { root = JsonNode.Parse(File.ReadAllText(WidgetsPath)); }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
        catch (System.Text.Json.JsonException) { }
        if (root?["widgets"] is not JsonArray list) yield break;
        foreach (var e in list)
        {
            if (e is not JsonObject o) continue;
            string kind = (string?)o["kind"] ?? "", size = (string?)o["size"] ?? "small";
            int col = (int?)(double?)o["col"] ?? 0, row = (int?)(double?)o["row"] ?? 0;
            string[] cities = o["cities"] is JsonArray a ? a.Select(c => c is JsonValue v && v.TryGetValue(out string? s) ? s : "").ToArray() : Array.Empty<string>();
            if (kind.Length > 0) yield return (kind, size, col, row, cities);
        }
    }
}
