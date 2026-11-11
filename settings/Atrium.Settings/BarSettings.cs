using System.Globalization;
using System.Text.Json.Nodes;

namespace Atrium.Settings;

// One pinned app of the bar. Either Path (.lnk / .exe) or Aumid identifies it.
internal sealed record PinnedApp(string Name, string Path, string Args, string Aumid, bool? MultipleWindows = null)
{
    public bool AllowsMultipleWindows
    {
        get
        {
            if (MultipleWindows is bool multiple) return multiple;
            string path = ShellIcons.ExecutablePath(Path);
            string exe = System.IO.Path.GetFileNameWithoutExtension(path).ToLowerInvariant();
            return exe is "explorer" or "chrome" or "msedge" or "firefox" or "brave" or "opera" or "vivaldi" or "chromium" ||
                   Aumid.Equals("Microsoft.Windows.Explorer", StringComparison.OrdinalIgnoreCase);
        }
    }
}

// Mirrors native/core/settings.h (same keys, ranges and presets). Lengths in DIP.
internal sealed class BarSettings
{
    public string Preset { get; set; } = "liquid";
    public double Transparency { get; set; } = 0.95;
    public double CornerRadius { get; set; } = 30;
    public double Blur { get; set; } = 0.50;
    public double Refraction { get; set; } = 30;
    public double Edge { get; set; } = 24;
    public double Dispersion { get; set; } = 0.30;
    public double Highlight { get; set; } = 0.80;
    public double Shadow { get; set; } = 0.30;
    public uint Tint { get; set; } = 0xFFFFFF;
    public double Width { get; set; } = 1200;
    public double Height { get; set; } = 60;
    public double Margin { get; set; } = 10;
    public double IconSize { get; set; } = 30;
    public double Magnify { get; set; } = 1.7;         // icon scale under the pointer, 1 = off
    public bool HoverRise { get; set; }               // glass follows the magnification wave (off unless chosen)
    public bool AutoWidth { get; set; } = true;
    public bool ShowClock { get; set; } = true;
    public bool ShowTray { get; set; } = true;
    public bool StartTray { get; set; } = true;        // the tray's button in the start menu
    public bool StartStatus { get; set; } = true;      // its network, volume and battery buttons
    public string StartSize { get; set; } = "";        // small | medium | large; empty: not known yet (the core fills it in)
    public bool ShowStatus { get; set; } = true;
    public bool ShowNotifications { get; set; } = true;
    public bool ShowRunning { get; set; } = true;
    public string TextColor { get; set; } = "auto"; // auto | light | dark
    public string AutoHide { get; set; } = "fullscreen"; // never | fullscreen (a maximized or full-screen app in front) | always
    public string StatusStyle { get; set; } = "ring"; // ring | icons (network, volume, battery as on the Windows taskbar)
    public string DockDisplayMode { get; set; } = "primary"; // primary | follow | fixed | all
    public double DockDisplayDelay { get; set; } = 450; // milliseconds at another display's bottom edge
    public string DockDisplay { get; set; } = "";
    public string DesktopDisplay { get; set; } = ""; // empty follows the system primary
    public string TaskbarApps { get; set; } = "all"; // all | primaryAndWindow | window
    public bool TaskbarRightPrimaryOnly { get; set; } // hide the right-side group on non-primary displays
    public double NotifyTimeout { get; set; } = 6;    // seconds a popup stays
    public string NotifyPlacement { get; set; } = "dock"; // dock (default) | topRight
    public bool NotifyHideBody { get; set; }           // privacy: no message text, excluded from capture
    public bool NotifyListener { get; set; } = true;   // Windows notifications as Atrium cards
    public bool NotifyFeishu { get; set; } = true;     // Feishu messages as Atrium cards (its own popup, read only)
    public bool NotifyWeChat { get; set; } = true;     // WeChat messages as Atrium cards (its chat list, read only)
    public bool Bar { get; set; } = true;              // the glass bar module (off: Windows' taskbar and banners)
    public bool Widgets { get; set; } = true;          // desktop widgets (their layout is the core's widgets.json)
    public bool WidgetsOnRight { get; set; }
    public string WidgetBackdrop { get; set; } = "desktop"; // desktop (captured live) | wallpaper (the files, least work)
    public bool StageManager { get; set; }             // the app in front on the stage, the others in a strip at the side
    public bool StageOnRight { get; set; }             // the strip's side; left by default (the widgets take the other side)
    public bool StageAutoHide { get; set; }            // the strip only comes out while the pointer rests on its edge
    public bool StageMinimize { get; set; }            // a window minimized on the stage goes to the strip (off unless chosen)
    public string WidgetMaterial { get; set; } = "glass"; // glass (the bar's) | dusk (dimmed glass, white ink) | color (their own faces)
    public bool HideTaskbar { get; set; }              // the system taskbar is hidden while Atrium runs
    public bool HideBanners { get; set; }              // Do Not Disturb: no system banners, notifications still become cards
    public bool WinKeyStart { get; set; }              // the Windows key alone opens Atrium's start menu
    public bool WinNumbers { get; set; }               // Win + 1..9, 0 open the bar's apps
    public bool StartOnLogin { get; set; } = true;
    // Null = the key was absent (never saved); the core then imports the Windows pins.
    public List<PinnedApp>? Pins { get; set; }

    public static BarSettings Liquid() => new();

    public static BarSettings Frosted() => new()
    {
        Preset = "frosted", Transparency = 0.80, Blur = 1.0, Refraction = 6, Edge = 10, Dispersion = 0.04, Highlight = 0.45, Shadow = 0.30, Tint = 0xF5F5FA,
    };

    // Copies the material of `preset`, keeping layout and content options.
    public void ApplyMaterial(BarSettings preset)
    {
        Preset = preset.Preset;
        Transparency = preset.Transparency;
        Blur = preset.Blur;
        Refraction = preset.Refraction;
        Edge = preset.Edge;
        Dispersion = preset.Dispersion;
        Highlight = preset.Highlight;
        Shadow = preset.Shadow;
        Tint = preset.Tint;
    }

    public BarSettings Clone()
    {
        var copy = (BarSettings)MemberwiseClone();
        copy.Pins = Pins == null ? null : new List<PinnedApp>(Pins);
        return copy;
    }

    public JsonObject ToJson()
    {
        var o = new JsonObject
        {
            ["version"] = 2,
            ["preset"] = Preset,
            ["transparency"] = Transparency,
            ["cornerRadius"] = CornerRadius,
            ["blur"] = Blur,
            ["refraction"] = Refraction,
            ["edge"] = Edge,
            ["dispersion"] = Dispersion,
            ["highlight"] = Highlight,
            ["shadow"] = Shadow,
            ["tint"] = "#" + Tint.ToString("X6", CultureInfo.InvariantCulture),
            ["width"] = Width,
            ["height"] = Height,
            ["margin"] = Margin,
            ["iconSize"] = IconSize,
            ["magnify"] = Magnify,
            ["hoverRise"] = HoverRise,
            ["showClock"] = ShowClock,
            ["showTray"] = ShowTray,
            ["startTray"] = StartTray,
            ["startStatus"] = StartStatus,
            ["showStatus"] = ShowStatus,
            ["showNotifications"] = ShowNotifications,
            ["autoWidth"] = AutoWidth,
            ["showRunning"] = ShowRunning,
            ["textColor"] = TextColor,
            ["autoHide"] = AutoHide,
            ["statusStyle"] = StatusStyle,
            ["dockDisplayMode"] = DockDisplayMode,
            ["dockDisplayDelay"] = DockDisplayDelay,
            ["dockDisplay"] = DockDisplay,
            ["desktopDisplay"] = DesktopDisplay,
            ["taskbarApps"] = TaskbarApps,
            ["taskbarRightPrimaryOnly"] = TaskbarRightPrimaryOnly,
            ["notifyTimeout"] = NotifyTimeout,
            ["notifyPlacement"] = NotifyPlacement,
            ["notifyHideBody"] = NotifyHideBody,
            ["notifyListener"] = NotifyListener,
            ["notifyWeChat"] = NotifyWeChat,
            ["notifyFeishu"] = NotifyFeishu,
            ["bar"] = Bar,
            ["widgets"] = Widgets,
            ["widgetsOnRight"] = WidgetsOnRight,
            ["widgetBackdrop"] = WidgetBackdrop,
            ["stageManager"] = StageManager,
            ["stageOnRight"] = StageOnRight,
            ["stageAutoHide"] = StageAutoHide,
            ["stageMinimize"] = StageMinimize,
            ["widgetMaterial"] = WidgetMaterial,
            ["hideTaskbar"] = HideTaskbar,
            ["hideBanners"] = HideBanners,
            ["winKeyStart"] = WinKeyStart,
            ["winNumbers"] = WinNumbers,
            ["startOnLogin"] = StartOnLogin,
        };
        if (StartSize.Length > 0)
            o["startSize"] = StartSize;
        if (Pins != null)
        {
            var a = new JsonArray();
            foreach (var p in Pins)
            {
                var pin = new JsonObject { ["name"] = p.Name, ["path"] = p.Path, ["args"] = p.Args, ["aumid"] = p.Aumid };
                if (p.MultipleWindows is bool multiple) pin["multipleWindows"] = multiple;
                a.Add(pin);
            }
            o["pins"] = a;
        }
        return o;
    }

    // Missing or malformed keys keep the values of `fallback`.
    public static BarSettings FromJson(JsonObject? o, BarSettings fallback)
    {
        var s = new BarSettings();
        var f = fallback;
        double Num(string k, double d) => o?[k] is JsonValue v && v.TryGetValue(out double x) && double.IsFinite(x) ? x : d;
        s.Preset = o?["preset"] is JsonValue p && p.TryGetValue(out string? ps) && ps != null ? ps : f.Preset;
        s.Transparency = Math.Clamp(Num("transparency", f.Transparency), 0, 1);
        s.CornerRadius = Math.Clamp(Num("cornerRadius", f.CornerRadius), 0, 200);
        s.Blur = Math.Clamp(Num("blur", f.Blur), 0, 1);
        s.Refraction = Math.Clamp(Num("refraction", f.Refraction), 0, 60);
        s.Edge = Math.Clamp(Num("edge", f.Edge), 1, 80);
        s.Dispersion = Math.Clamp(Num("dispersion", f.Dispersion), 0, 0.5);
        s.Highlight = Math.Clamp(Num("highlight", f.Highlight), 0, 1);
        s.Shadow = Math.Clamp(Num("shadow", f.Shadow), 0, 1);
        s.Width = Math.Clamp(Num("width", f.Width), 200, 4000);
        s.Height = Math.Clamp(Num("height", f.Height), 32, 160);
        s.Margin = Math.Clamp(Num("margin", f.Margin), 0, 400);
        s.IconSize = Math.Clamp(Num("iconSize", f.IconSize), 16, 64);
        s.Magnify = Math.Clamp(Num("magnify", f.Magnify), 1, 2);
        s.Tint = f.Tint;
        if (o?["tint"] is JsonValue t && t.TryGetValue(out string? ts) && ts is { Length: 7 } && ts[0] == '#' &&
            uint.TryParse(ts.AsSpan(1), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var tint))
            s.Tint = tint;
        bool Flag(string k, bool d) => o?[k] is JsonValue v && v.TryGetValue(out bool b) ? b : d;
        s.HoverRise = Flag("hoverRise", f.HoverRise);
        s.ShowClock = Flag("showClock", f.ShowClock);
        s.ShowTray = Flag("showTray", f.ShowTray);
        s.StartTray = Flag("startTray", f.StartTray);
        s.StartStatus = Flag("startStatus", f.StartStatus);
        s.StartSize = o?["startSize"] is JsonValue ss0 && ss0.TryGetValue(out string? size) && size is "small" or "medium" or "large" ? size! : f.StartSize;
        s.ShowStatus = Flag("showStatus", f.ShowStatus);
        s.ShowNotifications = Flag("showNotifications", f.ShowNotifications);
        s.AutoWidth = Flag("autoWidth", f.AutoWidth);
        s.ShowRunning = Flag("showRunning", f.ShowRunning);
        s.NotifyTimeout = Math.Clamp(Num("notifyTimeout", f.NotifyTimeout), 3, 30);
        s.NotifyHideBody = Flag("notifyHideBody", f.NotifyHideBody);
        // Version 1 files saved the old default (off): from version 2 it is on (as in the core).
        bool v1 = o?["version"] is JsonValue ver && ver.TryGetValue(out double vn) && vn < 2;
        s.NotifyListener = v1 ? f.NotifyListener : Flag("notifyListener", f.NotifyListener);
        s.NotifyWeChat = Flag("notifyWeChat", f.NotifyWeChat);
        s.NotifyFeishu = Flag("notifyFeishu", f.NotifyFeishu);
        s.Bar = Flag("bar", f.Bar);
        s.Widgets = Flag("widgets", f.Widgets);
        s.WidgetsOnRight = Flag("widgetsOnRight", f.WidgetsOnRight);
        s.WidgetBackdrop = o?["widgetBackdrop"] is JsonValue wb && wb.TryGetValue(out string? wbs) && wbs != null
            ? (wbs == "wallpaper" ? "wallpaper" : "desktop")
            : f.WidgetBackdrop;
        s.StageManager = Flag("stageManager", f.StageManager);
        s.StageOnRight = Flag("stageOnRight", f.StageOnRight);
        s.StageAutoHide = Flag("stageAutoHide", f.StageAutoHide);
        s.StageMinimize = Flag("stageMinimize", f.StageMinimize);
        s.WidgetMaterial = o?["widgetMaterial"] is JsonValue wm && wm.TryGetValue(out string? wms) && wms is "dusk" or "color" or "glass"
            ? wms : f.WidgetMaterial;
        s.HideTaskbar = Flag("hideTaskbar", f.HideTaskbar);
        s.HideBanners = Flag("hideBanners", f.HideBanners);
        s.WinKeyStart = Flag("winKeyStart", f.WinKeyStart);
        s.WinNumbers = Flag("winNumbers", f.WinNumbers);
        s.StartOnLogin = Flag("startOnLogin", f.StartOnLogin);
        string Display(string key, string value) => o?[key] is JsonValue v && v.TryGetValue(out string? text) && text != null ? text : value;
        string mode = Display("dockDisplayMode", f.DockDisplayMode);
        s.DockDisplayMode = mode is "follow" or "fixed" or "all" ? mode : "primary";
        s.DockDisplayDelay = Math.Clamp(Num("dockDisplayDelay", f.DockDisplayDelay), 100, 2000);
        s.DockDisplay = Display("dockDisplay", f.DockDisplay);
        s.DesktopDisplay = Display("desktopDisplay", f.DesktopDisplay);
        string apps = Display("taskbarApps", f.TaskbarApps);
        s.TaskbarApps = apps is "primaryAndWindow" or "window" ? apps : "all";
        s.TaskbarRightPrimaryOnly = Flag("taskbarRightPrimaryOnly", f.TaskbarRightPrimaryOnly);
        s.NotifyPlacement = Display("notifyPlacement", f.NotifyPlacement) == "topRight" ? "topRight" : "dock";
        s.TextColor = o?["textColor"] is JsonValue tc && tc.TryGetValue(out string? tcs) && tcs != null
            ? (tcs is "light" or "dark" ? tcs : "auto")
            : f.TextColor;
        s.AutoHide = o?["autoHide"] is JsonValue ah && ah.TryGetValue(out string? ahs) && ahs != null
            ? (ahs is "never" or "always" ? ahs : "fullscreen")
            : f.AutoHide;
        s.StatusStyle = o?["statusStyle"] is JsonValue ss && ss.TryGetValue(out string? sss) && sss == "icons" ? "icons"
            : o?["statusStyle"] is JsonValue ? "ring" : f.StatusStyle;
        s.Pins = f.Pins == null ? null : new List<PinnedApp>(f.Pins);
        if (o?["pins"] is JsonArray arr)
        {
            var pins = new List<PinnedApp>();
            foreach (var node in arr)
            {
                if (node is not JsonObject e) continue;
                string Str(string k) => e[k] is JsonValue v && v.TryGetValue(out string? x) && x != null ? x : "";
                bool? multiple = e["multipleWindows"] is JsonValue flag && flag.TryGetValue(out bool value) ? value : null;
                var pin = new PinnedApp(Str("name"), Str("path"), Str("args"), Str("aumid"), multiple);
                if (pin.Path.Length > 0 || pin.Aumid.Length > 0) pins.Add(pin);
            }
            if (pins.Count > 64) pins.RemoveRange(64, pins.Count - 64);
            s.Pins = pins;
        }
        return s;
    }

    public bool SameAs(BarSettings o) =>
        SameMaterial(o) && Preset == o.Preset && CornerRadius == o.CornerRadius && Width == o.Width && Height == o.Height &&
        Margin == o.Margin && IconSize == o.IconSize && Magnify == o.Magnify && HoverRise == o.HoverRise && AutoWidth == o.AutoWidth && ShowClock == o.ShowClock &&
        ShowTray == o.ShowTray && StartTray == o.StartTray && StartStatus == o.StartStatus && StartSize == o.StartSize && ShowStatus == o.ShowStatus && ShowNotifications == o.ShowNotifications &&
        ShowRunning == o.ShowRunning && TextColor == o.TextColor && AutoHide == o.AutoHide && StatusStyle == o.StatusStyle && NotifyTimeout == o.NotifyTimeout && NotifyHideBody == o.NotifyHideBody &&
        NotifyListener == o.NotifyListener && NotifyWeChat == o.NotifyWeChat && NotifyFeishu == o.NotifyFeishu && Bar == o.Bar && Widgets == o.Widgets &&
        WidgetsOnRight == o.WidgetsOnRight && WidgetBackdrop == o.WidgetBackdrop && StageManager == o.StageManager && StageOnRight == o.StageOnRight &&
        StageAutoHide == o.StageAutoHide && StageMinimize == o.StageMinimize && WidgetMaterial == o.WidgetMaterial &&
        HideTaskbar == o.HideTaskbar && HideBanners == o.HideBanners && WinKeyStart == o.WinKeyStart && WinNumbers == o.WinNumbers &&
        StartOnLogin == o.StartOnLogin &&
        DockDisplayMode == o.DockDisplayMode && DockDisplayDelay == o.DockDisplayDelay && DockDisplay == o.DockDisplay && DesktopDisplay == o.DesktopDisplay &&
        TaskbarApps == o.TaskbarApps && TaskbarRightPrimaryOnly == o.TaskbarRightPrimaryOnly &&
        NotifyPlacement == o.NotifyPlacement &&
        (Pins == o.Pins || (Pins != null && o.Pins != null && Pins.SequenceEqual(o.Pins)));

    public bool SameMaterial(BarSettings o) =>
        Transparency == o.Transparency && Blur == o.Blur && Refraction == o.Refraction && Edge == o.Edge && Dispersion == o.Dispersion &&
        Highlight == o.Highlight && Shadow == o.Shadow && Tint == o.Tint;
}
