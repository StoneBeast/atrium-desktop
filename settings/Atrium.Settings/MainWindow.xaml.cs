using System.Diagnostics;
using System.Net;
using System.Net.Http;
using System.Numerics;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Nodes;
using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.UI;

namespace Atrium.Settings;

public sealed partial class MainWindow : Window
{
    [DllImport("user32.dll")] private static extern uint GetDpiForWindow(IntPtr hwnd);
    private delegate IntPtr SubclassProc(IntPtr hwnd, uint message, UIntPtr wparam, IntPtr lparam, UIntPtr id, UIntPtr data);
    [DllImport("comctl32.dll")] private static extern bool SetWindowSubclass(IntPtr hwnd, SubclassProc proc, UIntPtr id, UIntPtr data);
    [DllImport("comctl32.dll")] private static extern bool RemoveWindowSubclass(IntPtr hwnd, SubclassProc proc, UIntPtr id);
    [DllImport("comctl32.dll")] private static extern IntPtr DefSubclassProc(IntPtr hwnd, uint message, UIntPtr wparam, IntPtr lparam);
    private SubclassProc? _updateMessage;

    // One slider tile: its group, UI range, and the mapping between the stored
    // value and the slider value (percentages are stored as 0..1).
    private sealed record SliderDef(string Group, string Title, double Min, double Max, double Step, string Unit,
        Func<BarSettings, double> Get, Action<BarSettings, double> Set, bool Material);

    private SliderDef[] Sliders => new SliderDef[]
    {
        new("clarity", "透明度", 0, 100, 1, "%",
            s => s.Transparency * 100, (s, v) => s.Transparency = v / 100, true),
        new("clarity", "模糊", 0, 100, 1, "%",
            s => s.Blur * 100, (s, v) => s.Blur = v / 100, true),
        new("edge", "折射强度", 0, 60, 1, "px",
            s => s.Refraction, (s, v) => s.Refraction = v, true),
        new("edge", "折射边缘宽度", 1, 80, 1, "px",
            s => s.Edge, (s, v) => s.Edge = v, true),
        new("edge", "色散", 0, 50, 1, "%",
            s => s.Dispersion * 100, (s, v) => s.Dispersion = v / 100, true),
        new("light", "高光", 0, 100, 1, "%",
            s => s.Highlight * 100, (s, v) => s.Highlight = v / 100, true),
        new("light", "阴影", 0, 100, 1, "%",
            s => s.Shadow * 100, (s, v) => s.Shadow = v / 100, true),

        new("size", "高度", 32, 120, 1, "px",
            s => s.Height, (s, v) => s.Height = v, false),
        new("size", "圆角", 0, 80, 1, "px",
            s => s.CornerRadius, (s, v) => s.CornerRadius = v, false),
        new("dock", "图标大小", 16, 64, 1, "px",
            s => s.IconSize, (s, v) => s.IconSize = v, false),
        new("width", "最大宽度", 300, 2400, 10, "px",
            s => s.Width, (s, v) => s.Width = v, false),
        new("dock", "底部间距", 0, 200, 1, "px",
            s => s.Margin, (s, v) => s.Margin = v, false),
        new("dock", "悬停放大", 100, 200, 5, "%",
            s => s.Magnify * 100, (s, v) => s.Magnify = v / 100, false),

        new("notify", "停留时间", 3, 30, 1, "s",
            s => s.NotifyTimeout, (s, v) => s.NotifyTimeout = v, false),
    };

    private static readonly string[] PageTitles =
    {
        "材质", "形状", "任务栏", "通知", "小组件", "台前调度", "显示器", "系统", "设计语言", "关于",
    };
    private readonly CoreClient _client = new();
    private readonly List<(SliderDef Def, SliderTile Tile)> _rows = new();
    private readonly List<(uint Rgb, Button Button, FrameworkElement Ring)> _tintButtons = new();
    private BarSettings _saved = new();
    private BarSettings _current = new();
    private bool _loading;
    private bool _previewBusy, _previewPending;
    private bool _closePending;
    private FrameworkElement[] _mains = Array.Empty<FrameworkElement>();
    private FrameworkElement?[] _sides = Array.Empty<FrameworkElement?>();
    private int _page;
    private bool _folded; // a narrow window: the inspector's groups under the workspace
    private int _flashId;
    private bool _updatingCoreSwitch, _coreChanging = true;
    private Process? _coreProcess;
    private CancellationTokenSource? _updateCheck;

    public MainWindow()
    {
        InitializeComponent();
        RootGrid.Loaded += (_, _) => ContinuousCorners.Install(RootGrid);
        RootGrid.Loaded += (_, _) => LoadStageArt(); // the Stage Manager page's pictures, ready before it opens
#if ATRIUM_DEVELOPMENT
        RootGrid.Loaded += (_, _) => ExportStageArtIfAsked();
#endif
        ExtendsContentIntoTitleBar = true;
        AppWindow.TitleBar.PreferredHeightOption = TitleBarHeightOption.Tall;
        SetTitleBar(AppTitleBar);
        AppWindow.SetIcon(Path.Combine(AppContext.BaseDirectory, "Assets", "atrium.ico"));
        double scale = GetDpiForWindow(WinRT.Interop.WindowNative.GetWindowHandle(this)) / 96.0;
        // A roomy default (the preview is the centre of the window), never larger than the screen.
        var work = DisplayArea.GetFromWindowId(AppWindow.Id, DisplayAreaFallback.Primary).WorkArea;
        int width = Math.Min((int)(1440 * scale), work.Width * 92 / 100), height = Math.Min((int)(920 * scale), work.Height * 92 / 100);
        AppWindow.MoveAndResize(new Windows.Graphics.RectInt32(work.X + (work.Width - width) / 2, work.Y + (work.Height - height) / 2, width, height));
        if (AppWindow.Presenter is OverlappedPresenter presenter)
        {
            presenter.PreferredMinimumWidth = (int)(980 * scale);
            presenter.PreferredMinimumHeight = (int)(680 * scale);
        }
        AppWindow.Closing += OnClosing;
        var hwnd = WinRT.Interop.WindowNative.GetWindowHandle(this);
        _updateMessage = (handle, message, wparam, lparam, id, data) =>
        {
            if (message == App.OpenAboutMessage) { Nav.SelectedIndex = 9; return IntPtr.Zero; }
            if (message is 0x007E or 0x02E0 || (message == 0x001A && wparam.ToUInt64() == 0x002F))
                DispatcherQueue.TryEnqueue(RefreshDisplays); // hotplug, DPI or work-area changed
            return DefSubclassProc(handle, message, wparam, lparam);
        };
        SetWindowSubclass(hwnd, _updateMessage, (UIntPtr)1, UIntPtr.Zero);
        Closed += (_, _) =>
        {
            _client.Dispose();
            RemoveWindowSubclass(hwnd, _updateMessage, (UIntPtr)1);
            _updateCheck?.Cancel();
            _updatesClosed = true;
            _downloadTimer?.Stop();
            _coreProcess?.Dispose();
            _widgetWatcher?.Dispose();
            _settingsWatcher?.Dispose();
            StopDesignAnimations();
            _renderWorker?.Stop();
            _swatchWorker?.Stop();
            _fixedSwatchWorker?.Stop();
            _photoThawTimer?.Stop();
            ++_photoSelection;
            StopTiles();
        };

        _mains = new FrameworkElement[] { MainMaterial, MainShape, MainDock, MainNotify, MainWidgets, MainStage, MainDisplays, MainSystem, DesignStack, MainAbout };
        _sides = new FrameworkElement?[] { SideMaterial, SideShape, SideDock, SideNotify, SideWidgets, SideStage, null, null, null, null };
        InitDisplays();
        LoupeSwitch.IsOn = true;
        BuildRows();
        BuildTintSwatches();
        InitPins();
        InitHero();
        PhotoChanged += () =>
        {
            if (_page == 4) { ShowWidgetMap(); RefreshGallery(); }
            if (_page == 3) ShowNotifyPreview(false);
            if (_page == 5) ShowStagePreview(false);
        };
        PathText.Text = CoreClient.SettingsPath;
        StartupSwitch.IsEnabled = !CoreClient.Development;
        if (CoreClient.Development) ToolTipService.SetToolTip(StartupSwitch, "开发构建不修改 Windows 开机启动项。");
        var v = typeof(MainWindow).Assembly.GetName().Version;
        VersionText.Text = "版本 " + (v?.ToString(3) ?? "1.0.0");
        ShowStatus(false);
        // `--page widgets` (the widgets' menu): open on that page.
        var args = Environment.GetCommandLineArgs();
        int page = Array.IndexOf(args, "--page") is int i && i >= 0 && i + 1 < args.Length
            ? args[i + 1] switch { "dock" => 2, "widgets" => 4, "stage" => 5, "displays" => 6, "system" => 7, "about" => 9, _ => 0 } : 0;
        Nav.SelectedIndex = page;
        _ = ConnectAndLoadAsync();
        _ = CheckForUpdatesAsync(); // once per settings window; no resident polling
    }

    // ---- Pages ----

    private void OnNavChanged(object sender, SelectionChangedEventArgs e)
    {
        if (Nav.SelectedIndex >= 0) ShowPage(Nav.SelectedIndex);
    }

    // A page is its workspace part (under the title, with the live preview on the first three) and its
    // inspector part; pages without parameters of their own (system, design, about) take the full width.
    private void ShowPage(int index)
    {
        if (_page == 8 && index != 8) StopDesignAnimations();
        _page = index;
        for (int i = 0; i < _mains.Length; i++)
        {
            _mains[i].Visibility = i == index ? Visibility.Visible : Visibility.Collapsed;
            if (_sides[i] is { } side) side.Visibility = i == index ? Visibility.Visible : Visibility.Collapsed;
        }
        if (_mains[index] == DesignStack)
        {
            BuildDesignPage();
            RefreshDesignDetails(); // the "current" column follows the settings
        }
        if (_mains[index] == MainWidgets)
        {
            if (WidgetGallery.Children.Count == 0) BuildWidgetGallery(); // built when first shown
            else RefreshGallery();
            ShowWidgetMap();
        }
        if (_mains[index] == MainNotify) ShowNotifyPreview(true);
        if (_mains[index] == MainStage) ShowStagePreview(true);
        if (_mains[index] == MainDisplays) RefreshDisplays();
        PageTitle.Text = PageTitles[index];
        ContinuousCorners.Install(_mains[index]);
        HeroBlock.Visibility = index < 3 ? Visibility.Visible : Visibility.Collapsed;
        LoupeToggle.Visibility = index == 0 ? Visibility.Visible : Visibility.Collapsed;
        Loupe.Visibility = index == 0 && LoupeSwitch.IsOn ? Visibility.Visible : Visibility.Collapsed;
        // The design page reads wider than a settings page.
        CenterStack.MaxWidth = _mains[index] == DesignStack ? 1280 : 1120;
        ArrangeInspector();
        CenterScroll.ChangeView(null, 0, null, true);
        InspectorScroll.ChangeView(null, 0, null, true);
        if (index < 3) UpdateHero();
        if (index == 0) UpdateCustomSwatch();
        AnimateIn(_mains[index]);
        if (_sides[index] is { } shown) AnimateIn(shown);
    }

    // The inspector: its own column when there is room for the workspace beside it, else folded
    // under the workspace (the groups move, keeping their state).
    private const double FoldBelow = 900;

    private void OnContentSizeChanged(object sender, SizeChangedEventArgs e)
    {
        bool folded = e.NewSize.Width < FoldBelow;
        if (folded == _folded && _mains.Length > 0) return;
        _folded = folded;
        if (folded)
        {
            InspectorScroll.Content = null;
            InspectorStack.Margin = new Thickness(0, 8, 0, 0);
            FoldedInspector.Child = InspectorStack;
        }
        else
        {
            FoldedInspector.Child = null;
            InspectorStack.ClearValue(FrameworkElement.MarginProperty);
            InspectorScroll.Content = InspectorStack;
        }
        ArrangeInspector();
    }

    private void ArrangeInspector()
    {
        bool has = _sides.Length > _page && _sides[_page] != null;
        InspectorPane.Visibility = has && !_folded ? Visibility.Visible : Visibility.Collapsed;
        FoldedInspector.Visibility = has && _folded ? Visibility.Visible : Visibility.Collapsed;
    }

    // Every page uses the viewport width, including pages with small intrinsic content.
    private void OnWorkspaceSizeChanged(object sender, SizeChangedEventArgs e) =>
        CenterStack.Width = Math.Max(0, e.NewSize.Width - CenterStack.Margin.Left - CenterStack.Margin.Right);

    // The previews keep a natural proportion as the workspace widens.
    private void OnCenterSizeChanged(object sender, SizeChangedEventArgs e)
    {
        double w = e.NewSize.Width;
        HeroFrame.Height = Math.Clamp(Math.Round(w * 0.52), 280, 580);
        NotifyStage.Height = Math.Clamp(Math.Round(w * 0.42), 260, 440);
    }

    private void OnLoupeToggled(object sender, RoutedEventArgs e)
    {
        Loupe.Visibility = _page == 0 && LoupeSwitch.IsOn ? Visibility.Visible : Visibility.Collapsed;
        if (Loupe.Visibility == Visibility.Collapsed)
        {
            _loupeBrush.ImageSource = null;
            _loupeBmp = null;
        }
        UpdateHero();
    }

    // Fade and rise a few pixels; the transitions are attached one tick later so the start values stick.
    private void AnimateIn(UIElement e)
    {
        e.OpacityTransition = null;
        e.TranslationTransition = null;
        e.Opacity = 0;
        e.Translation = new Vector3(0, 10, 0);
        DispatcherQueue.TryEnqueue(Microsoft.UI.Dispatching.DispatcherQueuePriority.Low, () =>
        {
            e.OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(240) };
            e.TranslationTransition = new Vector3Transition { Duration = TimeSpan.FromMilliseconds(300) };
            e.Opacity = 1;
            e.Translation = Vector3.Zero;
        });
    }

    // ---- Rows ----

    // Every slider is a tile in its group's grid: after the group's other tiles, or before them
    // where the sliders lead (the tint, the notification switch).
    private void BuildRows()
    {
        // The basic three are tiles in the workspace; the rest are plain rows in the inspector.
        var groups = new Dictionary<string, (Panel Panel, bool First, bool Flat)>
        {
            ["clarity"] = (ClarityTiles, true, false), ["edge"] = (EdgeRows, false, true), ["light"] = (LightRows, false, true),
            ["size"] = (SizeRows, false, true), ["width"] = (WidthRows, false, true), ["dock"] = (DockRows, false, true), ["notify"] = (NotifyRows, true, true),
        };
        var placed = new Dictionary<string, int>();
        foreach (var d in Sliders)
        {
            var (panel, first, flat) = groups[d.Group];
            var tile = new SliderTile { Flat = flat };
            tile.Configure(d.Title, d.Min, d.Max, d.Step, d.Unit);
            tile.UserValueChanged += v => OnSliderChanged(d, v);
            tile.Engaged += on => OnEngage(d.Title, on);
            _rows.Add((d, tile));
            int at = first ? placed.GetValueOrDefault(d.Group) : panel.Children.Count;
            panel.Children.Insert(at, tile);
            placed[d.Group] = at + 1;
        }
        AutoWidthSwitch.GotFocus += (_, _) => OnEngage("最大宽度", true);
        AutoWidthSwitch.LostFocus += (_, _) => OnEngage("最大宽度", false);
    }

    private static readonly (string Name, uint Rgb)[] TintChoices =
    {
        ("白色", 0xFFFFFF), ("暖白", 0xFFE6C8), ("冷蓝", 0xD3E6FF), ("玫瑰", 0xFFD0DC), ("薄荷", 0xCCF4E0), ("石墨", 0x2E3138),
    };

    private void BuildTintSwatches()
    {
        foreach (var (name, rgb) in TintChoices)
        {
            var ring = new Microsoft.UI.Xaml.Shapes.Ellipse { Width = 28, Height = 28, Stroke = Tokens.Brush("AccentBrush"), StrokeThickness = 2, Opacity = 0 };
            ring.OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(160) };
            var dot = new Microsoft.UI.Xaml.Shapes.Ellipse
            {
                Width = 22, Height = 22, Fill = new SolidColorBrush(TintColor(rgb)), Stroke = Tokens.Brush("CardStrokeBrush"), StrokeThickness = 1,
            };
            var button = new Button
            {
                Content = new Grid { Width = 28, Height = 28, Children = { dot, ring } },
                Style = (Style)Application.Current.Resources["IconButtonStyle"], Width = 32, Height = 32, CornerRadius = new CornerRadius(16),
            };
            AutomationProperties.SetName(button, "染色 " + name);
            ToolTipService.SetToolTip(button, name);
            var captured = rgb;
            button.Click += (_, _) => SetTint(captured);
            _tintButtons.Add((rgb, button, ring));
            TintSwatches.Children.Add(button);
        }
    }

    private void SetTint(uint rgb)
    {
        _current.Tint = rgb;
        MarkCustom();
        _loading = true;
        TintPicker.Color = TintColor(rgb);
        _loading = false;
        ShowTint();
        Changed();
    }

    private void ShowTint()
    {
        var c = TintColor(_current.Tint);
        TintSwatch.Fill = new SolidColorBrush(c);
        TintHex.Text = "#" + _current.Tint.ToString("X6", System.Globalization.CultureInfo.InvariantCulture);
        foreach (var (rgb, _, ring) in _tintButtons) ring.Opacity = rgb == _current.Tint ? 1 : 0;
    }

    // ---- Updates ----

    private ReleaseInfo? _latestRelease;
    private ReleaseInstaller.Prepared? _preparedRelease, _restartUpdate;
    private ReleaseInstaller.TransferStatus? _transferStatus;
    private DispatcherTimer? _downloadTimer;
    private bool _downloadRefreshBusy, _queueingDownload, _updatesClosed;

    private void ShowInstallAction()
    {
        InstallRelease.Content = _preparedRelease != null ? "重启并完成更新"
            : _transferStatus is { CanRetry: false } || _queueingDownload ? "后台下载中" : "重试下载";
        InstallRelease.IsEnabled = _updateCheck == null && !_queueingDownload
            && (_preparedRelease != null || _transferStatus?.CanRetry != false);
        InstallRelease.Visibility = _preparedRelease != null || _transferStatus != null || _latestRelease?.Download != null
            && _latestRelease.Version > typeof(MainWindow).Assembly.GetName().Version!
            ? Visibility.Visible : Visibility.Collapsed;
    }

    private void ObserveDownload()
    {
        if (_downloadTimer == null)
        {
            _downloadTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
            _downloadTimer.Tick += async (_, _) => await RefreshDownloadAsync();
        }
        _downloadTimer.Start();
    }

    private async Task RefreshDownloadAsync()
    {
        if (_downloadRefreshBusy || _updatesClosed) return;
        _downloadRefreshBusy = true;
        try
        {
            var state = await Task.Run(() => (Transfer: ReleaseInstaller.ReadTransferStatus(),
                Ready: ReleaseInstaller.ReadPrepared(typeof(MainWindow).Assembly.GetName().Version!)));
            if (_updatesClosed) return;
            _preparedRelease = state.Ready;
            _transferStatus = state.Transfer;
            if (_preparedRelease != null)
            {
                UpdateStatus.Text = "更新已准备好，重启后生效";
                UpdateAvailableButton.Visibility = ReleaseLinks.Visibility = Visibility.Visible;
                ReleaseNotes.NavigateUri ??= new Uri("https://github.com/wynter-yaxley/atrium-desktop/releases/latest");
                _downloadTimer?.Stop();
            }
            else if (_transferStatus != null)
            {
                UpdateStatus.Text = _transferStatus.Text;
                UpdateAvailableButton.Visibility = ReleaseLinks.Visibility = Visibility.Visible;
                ReleaseNotes.NavigateUri ??= new Uri("https://github.com/wynter-yaxley/atrium-desktop/releases/latest");
                if (_transferStatus.CanRetry) _downloadTimer?.Stop();
                else ObserveDownload();
            }
            else _downloadTimer?.Stop();
            ShowInstallAction();
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or COMException or JsonException)
        {
            if (!_updatesClosed) UpdateStatus.Text = $"无法读取下载进度：{error.Message}";
            _downloadTimer?.Stop();
        }
        finally { _downloadRefreshBusy = false; }
    }

    private async Task QueueDownloadAsync(ReleaseInfo? release)
    {
        if (_queueingDownload) return;
        _queueingDownload = true;
        ShowInstallAction();
        try
        {
            using var worker = release != null ? ReleaseInstaller.StartBackgroundDownload(release)
                : ReleaseInstaller.ResumeBackgroundDownload();
            await worker.WaitForExitAsync(); // closing this window never cancels the transfer
            if (_updatesClosed) return;
            await RefreshDownloadAsync();
            if (worker.ExitCode != 0 && _transferStatus == null) UpdateStatus.Text = "无法开始后台下载，请重试";
        }
        catch (Exception error) when (error is IOException or System.ComponentModel.Win32Exception)
        {
            if (!_updatesClosed) UpdateStatus.Text = $"无法开始后台下载：{error.Message}";
        }
        finally
        {
            _queueingDownload = false;
            if (!_updatesClosed) ShowInstallAction();
        }
    }

    private async void OnInstallRelease(object sender, RoutedEventArgs e)
    {
        if (_updateCheck != null || _queueingDownload) return;
        if (_preparedRelease != null)
        {
            _restartUpdate = _preparedRelease;
            await CloseRequestedAsync();
            return;
        }
        if (_latestRelease?.Download == null && _transferStatus == null) return;
        await QueueDownloadAsync(_latestRelease);
    }

    private void OnOpenUpdate(object sender, RoutedEventArgs e) => Nav.SelectedIndex = 9;

    private async void OnCheckForUpdates(object sender, RoutedEventArgs e) => await CheckForUpdatesAsync();

    private async Task CheckForUpdatesAsync()
    {
        if (_updateCheck != null) return;
        using var cancel = new CancellationTokenSource();
        _updateCheck = cancel;
        CheckUpdateButton.IsEnabled = false;
        UpdateProgress.Visibility = Visibility.Visible;
        UpdateProgress.IsActive = true;
        ReleaseLinks.Visibility = Visibility.Collapsed;
        UpdateStatus.Text = "正在检查…";
        try
        {
            string? installFailure = ReleaseInstaller.ReadFailure();
            await RefreshDownloadAsync();
            if (cancel.IsCancellationRequested) return;
            if (_preparedRelease != null)
            {
                UpdateAvailableButton.Visibility = ReleaseLinks.Visibility = Visibility.Visible;
                UpdateStatus.Text = installFailure == null ? "更新已准备好，重启后生效" : $"更新未安装：{installFailure}";
                ReleaseNotes.NavigateUri = new Uri("https://github.com/wynter-yaxley/atrium-desktop/releases/latest");
                ShowInstallAction();
                return;
            }
            if (_transferStatus is { CanRetry: false }) await QueueDownloadAsync(null);
            var release = await ReleaseInfo.FetchLatestAsync(cancel.Token);
            if (cancel.IsCancellationRequested) return;
            if (release == null)
            {
                UpdateAvailableButton.Visibility = Visibility.Collapsed;
                UpdateStatus.Text = "暂无正式版本";
                return;
            }
            var current = typeof(MainWindow).Assembly.GetName().Version!;
            int comparison = release.Version.CompareTo(current);
            UpdateAvailableButton.Visibility = comparison > 0 ? Visibility.Visible : Visibility.Collapsed;
            UpdateStatus.Text = comparison > 0 ? $"有可用更新 {release.Tag}"
                : comparison == 0 ? "已是最新版本" : $"当前版本高于最新正式版 {release.Tag}";
            ReleaseNotes.NavigateUri = release.Page;
            _latestRelease = release;
            ShowInstallAction();
            ReleaseLinks.Visibility = Visibility.Visible;
            if (comparison > 0 && release.Download != null) await QueueDownloadAsync(release);
            else if (_transferStatus != null) UpdateStatus.Text = _transferStatus.Text;
        }
        catch (Exception error) when (error is HttpRequestException or OperationCanceledException
            or JsonException or FormatException or InvalidOperationException or KeyNotFoundException or IOException or UnauthorizedAccessException)
        {
            if (cancel.IsCancellationRequested) return; // the window closed while checking
            UpdateStatus.Text = _transferStatus?.Text ?? (error switch
            {
                OperationCanceledException => "连接超时，请重试",
                HttpRequestException { StatusCode: HttpStatusCode.Forbidden or HttpStatusCode.TooManyRequests }
                    => "GitHub 暂时无法响应，请稍后重试",
                HttpRequestException => "无法连接 GitHub，请重试",
                IOException or UnauthorizedAccessException => $"无法准备更新：{error.Message}",
                _ => "无法读取版本信息，请稍后重试",
            });
        }
        finally
        {
            _updateCheck = null;
            if (!cancel.IsCancellationRequested)
            {
                UpdateProgress.IsActive = false;
                UpdateProgress.Visibility = Visibility.Collapsed;
                CheckUpdateButton.IsEnabled = true;
                ShowInstallAction();
            }
        }
    }

    // ---- Connection ----

    private async Task ConnectAndLoadAsync()
    {
        await _client.ConnectAsync(800);
        var (saved, current) = await _client.GetAsync();
        if (_updatesClosed) return;
        _saved = saved;
        _current = current;
        _coreChanging = false;
        WatchCore();
        WatchSettingsFile();
        ShowStatus(_client.Connected);
        LoadIntoControls();
        UpdateButtons();
        if (Content is FrameworkElement { IsLoaded: true }) App.SignalStartupReady(_client.Connected && _client.StateConfirmed);
        else if (Content is FrameworkElement root) root.Loaded += (_, _) => App.SignalStartupReady(_client.Connected && _client.StateConfirmed);
    }

    private static readonly SolidColorBrush RunningBrush = new(Color.FromArgb(255, 76, 194, 106));
    private static readonly SolidColorBrush IdleBrush = new(Color.FromArgb(255, 150, 150, 156));

    private void ShowStatus(bool connected)
    {
        StatusDot.Fill = connected ? RunningBrush : IdleBrush;
        _updatingCoreSwitch = true;
        CoreSwitch.IsOn = connected || (_coreProcess != null && !_coreProcess.HasExited);
        _updatingCoreSwitch = false;
        CoreSwitch.IsEnabled = !_coreChanging && CoreClient.CoreExe() != null;
        if (!_coreChanging) StatusMessage.Text = connected ? "正在运行" : _client.ConnectionError ??
            (_coreProcess != null && !_coreProcess.HasExited ? "核心进程仍在运行，设置连接已断开" : "已关闭，系统任务栏已恢复");
    }

    private void ShowMessage(InfoBarSeverity severity, string title, string message)
    {
        Status.Severity = severity;
        Status.Title = title;
        Status.Message = message;
        Status.IsOpen = true;
    }

    // Follow the process exit event rather than polling the pipe while the settings window is idle.
    private void WatchCore()
    {
        if (_updatesClosed) return;
        if (!_client.Connected && _coreProcess != null && !_coreProcess.HasExited) return;
        _coreProcess?.Dispose();
        _coreProcess = null;
        if (!_client.Connected || _updatesClosed) return;
        try
        {
            var process = Process.GetProcessById(_client.ProcessId);
            _coreProcess = process;
            process.Exited += (_, _) => DispatcherQueue.TryEnqueue(() =>
            {
                if (_coreProcess != process) return;
                _client.Disconnect(coreStopped: true);
                _coreProcess.Dispose();
                _coreProcess = null;
                ShowStatus(false);
            });
            process.EnableRaisingEvents = true;
        }
        catch (ArgumentException) { _client.Disconnect(coreStopped: true); }
    }

    private async void OnCoreToggled(object sender, RoutedEventArgs e)
    {
        if (_updatingCoreSwitch || _coreChanging) return;
        bool start = CoreSwitch.IsOn;
        _coreChanging = true;
        CoreSwitch.IsEnabled = false;
        StatusMessage.Text = start ? "正在启动…" : "正在关闭…";
        Status.IsOpen = false;
        try
        {
            var exe = CoreClient.CoreExe() ?? throw new FileNotFoundException("没有找到 Atrium.exe");
            if (start)
            {
                using var launched = Process.Start(new ProcessStartInfo(exe) { UseShellExecute = true });
                if (!await _client.ConnectAsync(8000)) throw new TimeoutException("未能连接 Atrium，请稍后重试。");
                bool dirty = !_current.SameAs(_saved);
                var (saved, current) = await _client.GetAsync();
                _saved = saved;
                if (!dirty) _current = current;
                LoadIntoControls();
                UpdateButtons();
                if (dirty) SchedulePreview();
            }
            else
            {
                _client.Disconnect();
                WatchCore();
                using var quit = Process.Start(new ProcessStartInfo(exe, "--exit") { UseShellExecute = false, CreateNoWindow = true })
                    ?? throw new InvalidOperationException("未能发送退出请求。");
                await quit.WaitForExitAsync();
                if (quit.ExitCode != 0) throw new InvalidOperationException("Atrium 尚未退出，请稍后重试。");
                _client.Disconnect(coreStopped: true);
            }
        }
        catch (Exception error) when (error is System.ComponentModel.Win32Exception or InvalidOperationException or TimeoutException or IOException)
        {
            ShowMessage(InfoBarSeverity.Error, start ? "启动失败" : "关闭失败", error.Message);
        }
        finally
        {
            _coreChanging = false;
            WatchCore();
            ShowStatus(_client.Connected);
        }
    }

    // ---- Changes made elsewhere ----

    // The core writes settings.json for changes made at the bar (its menu, the control centre):
    // what changed there comes into the controls, the edits made here meanwhile stay.
    private FileSystemWatcher? _settingsWatcher;
    private int _settingsReload;

    private void WatchSettingsFile()
    {
        string? dir = Path.GetDirectoryName(CoreClient.SettingsPath);
        if (_settingsWatcher != null || dir == null || !Directory.Exists(dir)) return;
        _settingsWatcher = new FileSystemWatcher(dir, "settings.json")
        {
            NotifyFilter = NotifyFilters.LastWrite | NotifyFilters.FileName | NotifyFilters.Size, EnableRaisingEvents = true,
        };
        FileSystemEventHandler changed = (_, _) => DispatcherQueue.TryEnqueue(ReloadExternal);
        _settingsWatcher.Changed += changed;
        _settingsWatcher.Created += changed;
        _settingsWatcher.Renamed += (_, _) => DispatcherQueue.TryEnqueue(ReloadExternal);
    }

    private async void ReloadExternal()
    {
        int id = ++_settingsReload;
        await Task.Delay(120); // one reload for a burst of writes
        if (id != _settingsReload || _updatesClosed || !_client.Connected) return;
        var (saved, _) = await _client.GetAsync();
        if (id != _settingsReload || _updatesClosed) return;
        var before = _saved.ToJson();
        var after = saved.ToJson();
        var changed = after.Select(kv => kv.Key).Where(k => !JsonNode.DeepEquals(before[k], after[k])).ToList();
        if (changed.Count == 0) return;
        var current = _current.ToJson();
        foreach (var k in changed) current[k] = after[k]?.DeepClone();
        _saved = saved;
        _current = BarSettings.FromJson(current, _current);
        LoadIntoControls();
        UpdateButtons();
        if (_page == 4) ShowWidgetMap();
        if (_page == 5) ShowStagePreview(true);
        if (!_current.SameAs(_saved)) SchedulePreview(); // the edits made here, over the new state
    }

    // ---- Controls <-> settings ----

    private void LoadIntoControls()
    {
        _loading = true;
        foreach (var (d, card) in _rows)
            card.SetValue(d.Get(_current));
        (_current.Preset switch { "liquid" => PresetLiquid, "frosted" => PresetFrosted, _ => PresetCustom }).IsChecked = true;
        (Photo.Selected switch { "wallpaper" => BgWallpaper, "paper" => BgPaper, "geometry" => BgGeometry, "layers" => BgLayers, _ => BgSpecimen }).IsChecked = true;
        ClockSwitch.IsOn = _current.ShowClock;
        TraySwitch.IsOn = _current.ShowTray;
        StatusSwitch.IsOn = _current.ShowStatus;
        NotificationsSwitch.IsOn = _current.ShowNotifications;
        AutoWidthSwitch.IsOn = _current.AutoWidth;
        HoverRiseSwitch.IsOn = _current.HoverRise;
        RunningSwitch.IsOn = _current.ShowRunning;
        HideBodySwitch.IsOn = _current.NotifyHideBody;
        (_current.NotifyPlacement == "topRight" ? NotifyAtTopRight : NotifyAtDock).IsChecked = true;
        ListenerSwitch.IsOn = _current.NotifyListener;
        WeChatSwitch.IsOn = _current.NotifyWeChat;
        FeishuSwitch.IsOn = _current.NotifyFeishu;
        BarSwitch.IsOn = _current.Bar;
        UpdateBarModule();
        WidgetsSwitch.IsOn = _current.Widgets;
        (_current.WidgetsOnRight ? WidgetsRight : WidgetsLeft).IsChecked = true;
        HideTaskbarSwitch.IsOn = _current.HideTaskbar;
        StageSwitch.IsOn = _current.StageManager;
        (_current.StageOnRight ? StageRight : StageLeft).IsChecked = true;
        (_current.StageAutoHide ? StageAutoHide : StageAlways).IsChecked = true;
        (_current.WidgetBackdrop == "wallpaper" ? BackdropWallpaper : BackdropDesktop).IsChecked = true;
        (_current.WidgetMaterial switch { "dusk" => MaterialDusk, "color" => MaterialColor, _ => MaterialGlass }).IsChecked = true;
        HideBannersSwitch.IsOn = _current.HideBanners;
        WinKeySwitch.IsOn = _current.WinKeyStart;
        WinNumbersSwitch.IsOn = _current.WinNumbers;
        (_current.StatusStyle == "icons" ? StatusIcons : StatusRing).IsChecked = true;
        (_current.AutoHide switch { "never" => HideNever, "always" => HideAlways, _ => HideFullscreen }).IsChecked = true;
        StartupSwitch.IsOn = _current.StartOnLogin;
        LoadDisplayControls();
        (_current.TextColor switch { "light" => TextLight, "dark" => TextDark, _ => TextAuto }).IsChecked = true;
        UpdateGallery();
        RebuildPins();
        TintPicker.Color = TintColor(_current.Tint);
        ShowTint();
        _loading = false;
        UpdateHero();
        UpdateCustomSwatch();
        if (_page == 3) ShowNotifyPreview(false);
        if (_page == 4) ShowWidgetMap();
        if (_page == 5) ShowStagePreview(false);
    }

    private static Color TintColor(uint rgb) => Color.FromArgb(255, (byte)(rgb >> 16), (byte)(rgb >> 8), (byte)rgb);

    private void OnSliderChanged(SliderDef d, double v)
    {
        if (_loading) return;
        d.Set(_current, v);
        if (d.Material) MarkCustom();
        Changed();
    }

    private void MarkCustom()
    {
        _current.Preset = "custom";
        bool was = _loading;
        _loading = true;
        PresetCustom.IsChecked = true;
        _loading = was;
    }

    private void OnPresetChecked(object sender, RoutedEventArgs e)
    {
        if (_loading || sender is not RadioButton { Tag: string tag }) return;
        if (tag == "liquid") _current.ApplyMaterial(BarSettings.Liquid());
        else if (tag == "frosted") _current.ApplyMaterial(BarSettings.Frosted());
        else _current.Preset = "custom";
        LoadIntoControls();
        Changed();
    }

    private void OnTintChanged(ColorPicker sender, ColorChangedEventArgs args)
    {
        if (_loading) return;
        _current.Tint = ((uint)args.NewColor.R << 16) | ((uint)args.NewColor.G << 8) | args.NewColor.B;
        MarkCustom();
        ShowTint();
        Changed();
    }

    private void OnClockToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.ShowClock = ClockSwitch.IsOn;
        Changed();
    }

    private void OnBarContentToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.ShowTray = TraySwitch.IsOn;
        _current.ShowStatus = StatusSwitch.IsOn;
        _current.ShowNotifications = NotificationsSwitch.IsOn;
        Changed();
    }

    private void OnWidgetAlignmentChecked(object sender, RoutedEventArgs e)
    {
        if (_loading || sender is not RadioButton { Tag: string tag }) return;
        _current.WidgetsOnRight = tag == "right";
        // With Stage Manager on, it keeps to the widgets' other side.
        if (_current.StageManager) SetStageSide(!_current.WidgetsOnRight);
        ShowWidgetMap();
        Changed();
    }

    private void SetStageSide(bool right)
    {
        _current.StageOnRight = right;
        _loading = true;
        (right ? StageRight : StageLeft).IsChecked = true;
        _loading = false;
    }

    private void SetWidgetSide(bool right)
    {
        _current.WidgetsOnRight = right;
        _loading = true;
        (right ? WidgetsRight : WidgetsLeft).IsChecked = true;
        _loading = false;
    }

    private void OnAutoWidthToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.AutoWidth = AutoWidthSwitch.IsOn;
        Changed();
    }

    private void OnHoverRiseToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.HoverRise = HoverRiseSwitch.IsOn;
        Changed();
    }

    private void OnRunningToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.ShowRunning = RunningSwitch.IsOn;
        Changed();
    }

    private void OnHideBodyToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.NotifyHideBody = HideBodySwitch.IsOn;
        Changed();
    }

    private void OnListenerToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.NotifyListener = ListenerSwitch.IsOn;
        Changed();
    }

    private void OnFeishuToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.NotifyFeishu = FeishuSwitch.IsOn;
        Changed();
    }

    // The bar module: off, its own options and the notification cards have nothing to act on.
    private void OnBarToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.Bar = BarSwitch.IsOn;
        UpdateBarModule();
        Changed();
    }

    private void UpdateBarModule()
    {
        foreach (var part in new FrameworkElement[] { DockContent, DockLayout, SideNotify })
        {
            part.Opacity = _current.Bar ? 1 : 0.45;
            part.IsHitTestVisible = _current.Bar;
        }
    }

    private void OnWidgetsToggled(object sender, RoutedEventArgs e)
    {
        UpdateGallery();
        if (_loading) return;
        _current.Widgets = WidgetsSwitch.IsOn;
        Changed();
    }

    // The widgets' material: the map and the gallery show it at once, the core with the preview.
    private void OnWidgetMaterialChecked(object sender, RoutedEventArgs e)
    {
        if (_loading || sender is not RadioButton { Tag: string tag } || tag == _current.WidgetMaterial) return;
        _current.WidgetMaterial = tag;
        Changed();
        if (_page == 4) { ShowWidgetMap(); RefreshGallery(); }
    }

    private void OnWidgetBackdropChecked(object sender, RoutedEventArgs e)
    {
        if (_loading || sender is not RadioButton { Tag: string tag }) return;
        _current.WidgetBackdrop = tag;
        Changed();
    }

    private void OnHideTaskbarToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.HideTaskbar = HideTaskbarSwitch.IsOn;
        Changed();
    }

    private void OnStageToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.StageManager = StageSwitch.IsOn;
        // The widgets go to the other side.
        if (_current.StageManager) SetWidgetSide(!_current.StageOnRight);
        Changed();
        if (_page == 5) ShowStagePreview(true);
    }

    private void OnStageSideChecked(object sender, RoutedEventArgs e)
    {
        if (_loading || sender is not RadioButton { Tag: string tag }) return;
        _current.StageOnRight = tag == "right";
        if (_current.StageManager) SetWidgetSide(!_current.StageOnRight);
        Changed();
        if (_page == 5) ShowStagePreview(true);
    }

    private void OnStageModeChecked(object sender, RoutedEventArgs e)
    {
        if (_loading || sender is not RadioButton { Tag: string tag }) return;
        _current.StageAutoHide = tag == "hover";
        Changed();
        if (_page == 5) ShowStagePreview(true);
    }


    private void OnStartupToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.StartOnLogin = StartupSwitch.IsOn;
        Changed();
    }

    private void OnHideBannersToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.HideBanners = HideBannersSwitch.IsOn;
        Changed();
    }

    private void OnWinKeyToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.WinKeyStart = WinKeySwitch.IsOn;
        Changed();
    }

    private void OnWinNumbersToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.WinNumbers = WinNumbersSwitch.IsOn;
        Changed();
    }

    private void OnStatusStyleChecked(object sender, RoutedEventArgs e)
    {
        if (_loading || sender is not RadioButton { Tag: string tag }) return;
        _current.StatusStyle = tag;
        Changed();
    }

    private void OnAutoHideChecked(object sender, RoutedEventArgs e)
    {
        if (_loading || sender is not RadioButton { Tag: string tag }) return;
        _current.AutoHide = tag;
        Changed();
    }

    private void OnWeChatToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.NotifyWeChat = WeChatSwitch.IsOn;
        Changed();
    }

    private void OnTextColorChecked(object sender, RoutedEventArgs e)
    {
        if (_loading || sender is not RadioButton { Tag: string tag }) return;
        _current.TextColor = tag;
        Changed();
    }

    private void OnNotifyPlacementChecked(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.NotifyPlacement = (string)((RadioButton)sender).Tag;
        _notifyEntrancePending = true;
        Changed();
    }

    private void Changed()
    {
        UpdateButtons();
        UpdateHero();
        UpdateCustomSwatch();
        if (_page == 3) ShowNotifyPreview(false);
        if (_page == 5) StageRenderGlass(StageCanvas.Width, StageCanvas.Height); // the material changed
        SchedulePreview();
    }

    // Title-bar actions appear while there is something to save, and briefly after a save.
    private void UpdateButtons()
    {
        bool dirty = !_current.SameAs(_saved);
        SaveButton.IsEnabled = dirty;
        RevertButton.IsEnabled = dirty;
        _flashId++;
        DirtyText.Text = "未保存";
        DirtyDot.Fill = new SolidColorBrush(Color.FromArgb(255, 0xFF, 0x9F, 0x0A));
        RevertButton.Visibility = SaveButton.Visibility = Visibility.Visible;
        ShowSaveBar(dirty);
    }

    private void ShowSaveBar(bool on)
    {
        SaveBar.OpacityTransition ??= new ScalarTransition { Duration = TimeSpan.FromMilliseconds(160) };
        SaveBar.Opacity = on ? 1 : 0;
        SaveBar.IsHitTestVisible = on;
    }

    private async void FlashSaved()
    {
        int id = ++_flashId;
        DirtyText.Text = "已保存";
        DirtyDot.Fill = new SolidColorBrush(Color.FromArgb(255, 0x30, 0xD1, 0x58));
        RevertButton.Visibility = SaveButton.Visibility = Visibility.Collapsed;
        ShowSaveBar(true);
        await Task.Delay(1600);
        if (id == _flashId) ShowSaveBar(false);
    }

    // At most one preview in flight; the newest state is sent when it returns,
    // so dragging a slider never queues up stale frames.
    private async void SchedulePreview()
    {
        if (!_client.Connected || _updatesClosed) return;
        if (_previewBusy)
        {
            _previewPending = true;
            return;
        }
        _previewBusy = true;
        do
        {
            _previewPending = false;
            await _client.PreviewAsync(_current.Clone());
            await Task.Delay(16);
        } while (_previewPending && !_updatesClosed);
        _previewBusy = false;
        if (!_updatesClosed && !_client.Connected) ShowStatus(false);
    }

    private async void OnSave(object sender, RoutedEventArgs e) => await SaveAsync();

    private async Task<bool> SaveAsync()
    {
        // What is saved is this snapshot: edits made while it is on its way stay unsaved.
        var sent = _current.Clone();
        var error = await _client.CommitAsync(sent.Clone());
        if (_updatesClosed) return false;
        if (error != null)
        {
            ShowMessage(InfoBarSeverity.Error, "保存失败", error);
            return false;
        }
        _saved = sent;
        Status.IsOpen = false;
        UpdateButtons();
        if (_client.LastSaveWarning is { } warning)
            ShowMessage(InfoBarSeverity.Warning, "设置已保存", warning);
        else if (_current.SameAs(_saved)) FlashSaved(); // edits made meanwhile keep the capsule as it is
        ShowStatus(_client.Connected);
        return true;
    }

    private async void OnRevert(object sender, RoutedEventArgs e)
    {
        _current = _saved.Clone();
        Status.IsOpen = false;
        LoadIntoControls();
        UpdateButtons();
        await _client.CancelAsync();
    }

    // A group's "restore defaults": its parameters (and what goes with them) back to the defaults.
    private void OnGroupReset(object sender, RoutedEventArgs e)
    {
        if (sender is not FrameworkElement { Tag: string group }) return;
        var d = BarSettings.Liquid();
        foreach (var (def, _) in _rows)
            if (def.Group == group) def.Set(_current, def.Get(d));
        if (group == "clarity") _current.Tint = d.Tint;
        if (group == "width") _current.AutoWidth = d.AutoWidth;
        if (group == "size") _current.HoverRise = d.HoverRise;
        if (group == "notify")
        {
            _current.NotifyHideBody = d.NotifyHideBody;
            _current.NotifyPlacement = d.NotifyPlacement;
        }
        if (group is "clarity" or "edge" or "light")
            _current.Preset = _current.SameMaterial(BarSettings.Liquid()) ? "liquid" : _current.SameMaterial(BarSettings.Frosted()) ? "frosted" : "custom";
        LoadIntoControls();
        Changed();
    }

    // The material back to the Liquid preset (layout, apps and sources stay as they are).
    private void OnDefaults(object sender, RoutedEventArgs e)
    {
        _current.ApplyMaterial(BarSettings.Liquid());
        LoadIntoControls();
        Changed();
    }

    private async void OnClosing(AppWindow sender, AppWindowClosingEventArgs args)
    {
        args.Cancel = true;
        await CloseRequestedAsync();
    }

    // Window.Close does not raise AppWindow.Closing. Both the title-bar close
    // and update button explicitly use the same save/exit path before calling it.
    private async Task CloseRequestedAsync()
    {
        if (_closePending || _coreChanging) return;
        _closePending = true;
        try
        {
            if (!_current.SameAs(_saved))
            {
                var dialog = new ContentDialog
                {
                    Style = (Style)Application.Current.Resources["AtriumDialogStyle"],
                    XamlRoot = Content.XamlRoot,
                    Title = "保存修改？",
                    Content = "未保存的修改在关闭后会被撤销。",
                    PrimaryButtonText = "保存",
                    SecondaryButtonText = "不保存",
                    CloseButtonText = "取消",
                    DefaultButton = ContentDialogButton.Primary,
                };
                var result = await dialog.ShowAsync();
                if (result == ContentDialogResult.None) { _restartUpdate = null; return; }
                if (result == ContentDialogResult.Primary && !await SaveAsync()) { _restartUpdate = null; return; }
            }
            if (_restartUpdate != null)
            {
                try { ReleaseInstaller.Restart(_restartUpdate, _client.Connected); }
                catch (Exception error) when (error is IOException or UnauthorizedAccessException or System.ComponentModel.Win32Exception)
                {
                    _restartUpdate = null;
                    UpdateStatus.Text = error is UnauthorizedAccessException ? "安装目录不可写，更新未安装" : $"无法重启更新：{error.Message}";
                    return;
                }
            }
            _client.Dispose(); // the core reverts any unsaved preview on disconnect
            Close();
        }
        finally { _closePending = false; }
    }
}
