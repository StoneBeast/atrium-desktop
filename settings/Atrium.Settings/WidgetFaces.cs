using System.Globalization;
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

// A widget's face as the core paints it (native/core/ui/widget_faces.cpp): in its material's ink, with
// macOS's few accents (orange second hand and notes, green / red battery, blue calendar), in widget
// DIP (small 164 x 164, medium 344 x 164, large 344 x 344). The glass itself is rendered under it.
internal static partial class WidgetFaces
{
    private const double Pad = 14; // kPad
    public static readonly Color Red = Color.FromArgb(255, 0xFF, 0x45, 0x3A), Orange = Color.FromArgb(255, 0xFF, 0x9F, 0x0A),
                                 Green = Color.FromArgb(255, 0x30, 0xD1, 0x58), Blue = Color.FromArgb(255, 0x0A, 0x84, 0xFF);
    private static readonly string[] WeekLong = { "星期一", "星期二", "星期三", "星期四", "星期五", "星期六", "星期日" };
    private static readonly string[] WeekShort = { "一", "二", "三", "四", "五", "六", "日" };
    private static readonly string[] Months = { "一月", "二月", "三月", "四月", "五月", "六月", "七月", "八月", "九月", "十月", "十一月", "十二月" };

    // What the weather's symbols are drawn over (widget_faces.cpp, Ink).
    private enum Ink { Light, Dark, Mono }

    // GlassPal / DuskPal / PaperPal: the inks and the accents of a material.
    private sealed record Pal(Color Text, Color Sub, Color Faint, Color Face, Color Track, Color Halo, bool Dark,
                              Color Red, Color Orange, Color Green, Color Blue, Color OnAccent, Color NightFace, Ink Icon)
    {
        private static Color White(double a) => Color.FromArgb((byte)Math.Round(255 * a), 255, 255, 255);

        // The glass's inks (Theme::For) and macOS's dark-mode accents.
        public static Pal For(bool dark)
        {
            Color ink = dark ? Color.FromArgb(255, 0x11, 0x13, 0x17) : Color.FromArgb(255, 255, 255, 255);
            Color A(double a) => Color.FromArgb((byte)Math.Round(255 * a), ink.R, ink.G, ink.B);
            double text = dark ? 0.92 : 0.96;
            return new Pal(A(text), A(dark ? 0.66 : 0.76), A(dark ? 0.46 : 0.56), A(text * 0.10), A(text * 0.18),
                           dark ? Color.FromArgb(89, 255, 255, 255) : Color.FromArgb(82, 0, 0, 0), dark,
                           WidgetFaces.Red, WidgetFaces.Orange, WidgetFaces.Green, WidgetFaces.Blue, White(0.98), Color.FromArgb(107, 0, 0, 0),
                           dark ? Ink.Dark : Ink.Light);
        }

        // Dusk: white only, the accents too; a disc's ink is dark.
        public static Pal Dusk() =>
            new(White(0.96), White(0.72), White(0.50), White(0.12), White(0.22), Color.FromArgb(128, 0, 0, 0), false,
                White(0.96), White(0.96), White(0.96), White(0.96), Color.FromArgb(235, 0x1C, 0x1C, 0x1E), Color.FromArgb(82, 0, 0, 0), Ink.Mono);

        // Color on a white face: dark ink, macOS's light-mode accents, no halo.
        public static Pal Paper()
        {
            Color A(double a) => Color.FromArgb((byte)Math.Round(255 * a), 0x1D, 0x1D, 0x1F);
            return new Pal(A(0.94), A(0.58), A(0.36), A(0.05), A(0.10), Colors.Transparent, true,
                           Color.FromArgb(255, 0xFF, 0x3B, 0x30), Color.FromArgb(255, 0xFF, 0x95, 0x00), Color.FromArgb(255, 0x34, 0xC7, 0x59),
                           Color.FromArgb(255, 0x00, 0x7A, 0xFF), White(0.98), Color.FromArgb(255, 0x1C, 0x1C, 0x1E), Ink.Dark);
        }

        // Color on the weather's sky: the light glass ink without its halo.
        public static Pal Sky() => For(false) with { Halo = Colors.Transparent };
    }

    public static (double W, double H) Size(string size) => size switch
    {
        "small" => (164, 164),
        "large" => (344, 344),
        _ => (344, 164),
    };

    // material: glass (the glass's ink: darkInk), dusk (a dark veil, white only) or color (an opaque face).
    // The world clocks' cities and their Windows time zones (widget_faces.cpp, WorldCities).
    private static readonly (string Name, string Zone)[] WorldCities =
    {
        ("北京", "China Standard Time"), ("上海", "China Standard Time"), ("香港", "China Standard Time"), ("台北", "Taipei Standard Time"),
        ("东京", "Tokyo Standard Time"), ("首尔", "Korea Standard Time"), ("新加坡", "Singapore Standard Time"), ("曼谷", "SE Asia Standard Time"),
        ("新德里", "India Standard Time"), ("迪拜", "Arabian Standard Time"), ("莫斯科", "Russian Standard Time"), ("伊斯坦布尔", "Turkey Standard Time"),
        ("开罗", "Egypt Standard Time"), ("约翰内斯堡", "South Africa Standard Time"), ("柏林", "W. Europe Standard Time"), ("巴黎", "Romance Standard Time"),
        ("伦敦", "GMT Standard Time"), ("圣保罗", "E. South America Standard Time"), ("纽约", "Eastern Standard Time"), ("多伦多", "Eastern Standard Time"),
        ("芝加哥", "Central Standard Time"), ("丹佛", "Mountain Standard Time"), ("洛杉矶", "Pacific Standard Time"), ("温哥华", "Pacific Standard Time"),
        ("安克雷奇", "Alaskan Standard Time"), ("檀香山", "Hawaiian Standard Time"), ("悉尼", "AUS Eastern Standard Time"), ("墨尔本", "AUS Eastern Standard Time"),
        ("奥克兰", "New Zealand Standard Time"),
    };

    // WidgetCities: the widget's own choice where it made one, else the defaults.
    private static string[] CitiesOf(string kind, IReadOnlyList<string>? chosen)
    {
        string[] cities = kind == "digital" ? new[] { "东京", "伦敦", "纽约" } : new[] { "北京", "东京", "伦敦", "纽约" };
        for (int i = 0; i < cities.Length && chosen != null && i < chosen.Count; i++)
            if (WorldCities.Any(c => c.Name == chosen[i])) cities[i] = chosen[i];
        return cities;
    }

    // The time in a city, and how far it is from here: "今天 +1小时" (CityTime).
    private static (DateTime Time, string When) CityTime(string name, DateTime now)
    {
        var t = now;
        try
        {
            var zone = WorldCities.FirstOrDefault(c => c.Name == name).Zone;
            if (zone != null) t = TimeZoneInfo.ConvertTimeFromUtc(DateTime.UtcNow, TimeZoneInfo.FindSystemTimeZoneById(zone));
        }
        catch (Exception e) when (e is TimeZoneNotFoundException or InvalidTimeZoneException) { }
        double hours = Math.Round((t - now).TotalMinutes / 15) / 4;
        int dd = t.Date.CompareTo(now.Date);
        return (t, (dd > 0 ? "明天" : dd < 0 ? "昨天" : "今天") + (hours >= 0 ? " +" : " ") + hours.ToString("0.##") + "小时");
    }

    public static FrameworkElement Face(string kind, string size, bool darkInk, Brush? photo = null, string material = "glass", IReadOnlyList<string>? cities = null)
    {
        var (w, h) = Size(size);
        var c = new Canvas { Width = w, Height = h, IsHitTestVisible = false };
        var p = material switch
        {
            "dusk" => Pal.Dusk(),
            "color" => kind == "weather" ? Pal.Sky() : Pal.Paper(),
            _ => Pal.For(darkInk),
        };
        if (material == "dusk") c.Children.Add(new Rectangle { Width = w, Height = h, RadiusX = 22, RadiusY = 22, Fill = new SolidColorBrush(Color.FromArgb(56, 0, 0, 0)) });
        else if (material == "color") c.Children.Add(new Rectangle { Width = w, Height = h, RadiusX = 22, RadiusY = 22, Fill = kind == "weather" ? SkyBrush() : new SolidColorBrush(Colors.White) });
        var now = DateTime.Now;
        switch (kind)
        {
            case "clock" when size == "small":
                ClockFace(c, w / 2, h / 2, h / 2 - 9, now, p.Face, p.Text, true, "北京", p.Sub, p);
                break;
            case "clock":
            {
                var names = CitiesOf(kind, cities);
                double colW = (w - 2 * Pad) / 4, R = 31;
                for (int i = 0; i < 4; i++)
                {
                    var (t, when) = CityTime(names[i], now);
                    bool night = t.Hour < 6 || t.Hour >= 18;
                    double cx = Pad + colW * (i + 0.5), cy = (h - 2 * R - 42) / 2 + R - 2;
                    Color face = night ? p.NightFace : p.Face, ink = night ? Color.FromArgb(245, 255, 255, 255) : p.Text;
                    ClockFace(c, cx, cy, R, t, face, ink, false, null, ink, p);
                    Text(c, names[i], cx - colW / 2, cy + R + 8, cx + colW / 2, cy + R + 26, 12.5, p.Text, 2, 1, p);
                    Text(c, when, cx - colW / 2, cy + R + 26, cx + colW / 2, cy + R + 42, 10.5, p.Sub, 0, 1, p);
                }
                break;
            }
            case "digital":
            {
                double leftW = size == "small" ? w - 2 * Pad : 150;
                Text(c, "本地", Pad + 2, Pad, Pad + leftW, Pad + 18, 12.5, p.Sub, 2, 0, p);
                // Match the core's fixed daily size, with one DIP of glyph-rounding headroom.
                double fit = TextWidth("00:00", 50, "TitleFont");
                double timeW = leftW - 1;
                Text(c, now.ToString("H:mm"), Pad, Pad + 22, Pad + leftW, Pad + 84, fit > timeW ? 50 * timeW / fit : 50, p.Text, 2, 1, p);
                Text(c, WeekLong[Weekday(now)], Pad + 2, h - Pad - 40, Pad + leftW, h - Pad - 20, 13, p.Blue, 2, 0, p);
                Text(c, $"{now.Month}月{now.Day}日", Pad + 2, h - Pad - 20, Pad + leftW, h - Pad, 13, p.Text, 0, 0, p);
                if (size == "small") break;
                double x0 = Pad + leftW + 8, rowH = (h - 2 * Pad) / 3;
                var more = CitiesOf(kind, cities);
                for (int i = 0; i < 3; i++)
                {
                    double y = Pad + rowH * i;
                    var (t, when) = CityTime(more[i], now);
                    if (i > 0) Line(c, x0, y, w - Pad, y, A(p.Text, 0.12), 1);
                    Text(c, more[i], x0, y + 4, w - Pad - 70, y + rowH / 2 + 2, 13, p.Text, 2, 0, p);
                    Text(c, when, x0, y + rowH / 2, w - Pad - 70, y + rowH - 4, 10.5, p.Sub, 0, 0, p);
                    Text(c, t.ToString("H:mm"), w - Pad - 80, y, w - Pad, y + rowH, 22, p.Text, 2, 2, p);
                }
                break;
            }
            case "calendar":
                Calendar(c, size, w, h, now, p);
                break;
            case "battery":
            {
                double cx = w / 2, cy = h / 2 - 12, R = 44;
                Arc(c, cx, cy, R, -90, 360, p.Track, 9, false);
                if (!BarFace.HasBattery())
                {
                    // A desktop: the ring full, a plug, plugged in (PaintBattery).
                    Arc(c, cx, cy, R, -90, 360, p.Green, 9, false);
                    Glyph(c, "\uE83E", cx - 22, cy - 22, cx + 22, cy + 22, 22, p.Text);
                    Text(c, "已接通电源", 0, cy + R + 12, w, cy + R + 34, 13, p.Text, 2, 1, p);
                    break;
                }
                var (fill, charging, low) = BarFace.Battery();
                Color level = low ? p.Red : p.Green;
                Arc(c, cx, cy, R, -90, 360 * fill, level, 9, true);
                Glyph(c, "\uE7F8", cx - 22, cy - 22, cx + 22, cy + 22, 24, p.Text);
                string n = $"{Math.Round(fill * 100)}%";
                double tw = TextWidth(n, 19, "TitleFont"), bolt = charging ? 16 : 0, x0 = (w - tw - bolt) / 2;
                if (charging) Glyph(c, "\uE945", x0 - 2, cy + R + 10, x0 + bolt, cy + R + 36, 13, level);
                Text(c, n, x0 + bolt, cy + R + 10, x0 + bolt + tw + 4, cy + R + 36, 19, p.Text, 2, 0, p);
                break;
            }
            case "bluetooth":
            {
                // PaintBluetooth, with examples: the gallery does not read the devices.
                (string Glyph, string Name, int Pct)[] d = { ("\uE7F6", "耳机", 82), ("\uE765", "键盘", 64), ("\uE962", "鼠标", 18), ("\uE7FC", "手柄", 45) };
                Color Col(int v) => v <= 20 ? p.Red : p.Green;
                if (size == "small")
                {
                    double cw = (w - 2 * Pad) / 2, ch = (h - 2 * Pad) / 2, R = 21;
                    for (int i = 0; i < 4; i++)
                    {
                        double cx = Pad + cw * (i % 2 + 0.5), cy = Pad + ch * (i / 2) + 26;
                        Arc(c, cx, cy, R, -90, 360, p.Track, 5, false);
                        Arc(c, cx, cy, R, -90, 360 * d[i].Pct / 100.0, Col(d[i].Pct), 5, true);
                        Glyph(c, d[i].Glyph, cx - R, cy - R, cx + R, cy + R, 15, p.Text);
                        Text(c, $"{d[i].Pct}%", cx - cw / 2, cy + R + 4, cx + cw / 2, cy + R + 20, 11.5, p.Text, 2, 1, p);
                    }
                    break;
                }
                double rowH = (h - 2 * Pad) / 4;
                for (int i = 0; i < 4; i++)
                {
                    double mid = Pad + rowH * i + rowH / 2;
                    Glyph(c, d[i].Glyph, Pad, mid - 12, Pad + 26, mid + 12, 16, p.Text);
                    Text(c, d[i].Name, Pad + 34, mid - 10, w - Pad - 112, mid + 10, 13, p.Text, 2, 0, p);
                    Fill(c, w - Pad - 106, mid - 3, w - Pad - 50, mid + 3, 3, p.Track);
                    Fill(c, w - Pad - 106, mid - 3, w - Pad - 106 + Math.Max(6, 56 * d[i].Pct / 100.0), mid + 3, 3, Col(d[i].Pct));
                    Text(c, $"{d[i].Pct}%", w - Pad - 46, mid - 10, w - Pad, mid + 10, 13, p.Text, 2, 2, p);
                }
                break;
            }
            case "system":
            {
                (string Name, double V, string Detail)[] g = { ("CPU", 0.23, "处理器"), ("内存", 0.58, "9.2 / 16 GB"), ("磁盘", 0.74, "214 GB 可用") };
                Color Col(double v) => v >= 0.9 ? p.Red : v >= 0.7 ? p.Orange : p.Green;
                if (size == "small")
                {
                    Text(c, "系统", Pad + 2, Pad - 2, w - Pad, Pad + 16, 12.5, p.Sub, 2, 0, p);
                    double rowH = (h - 2 * Pad - 18) / 3;
                    for (int i = 0; i < 3; i++)
                    {
                        double y = Pad + 18 + rowH * i;
                        Text(c, g[i].Name, Pad + 2, y + 2, w / 2, y + 22, 13, p.Text, 2, 0, p);
                        Text(c, $"{Math.Round(g[i].V * 100)}%", w / 2, y + 2, w - Pad - 2, y + 22, 13, p.Text, 2, 2, p);
                        Fill(c, Pad + 2, y + 26, w - Pad - 2, y + 32, 3, p.Track);
                        Fill(c, Pad + 2, y + 26, Pad + 2 + Math.Max(6, (w - 2 * Pad - 4) * g[i].V), y + 32, 3, Col(g[i].V));
                    }
                    break;
                }
                double colW = (w - 2 * Pad) / 3, R = 34, cy = Pad + 6 + R;
                for (int i = 0; i < 3; i++)
                {
                    double cx = Pad + colW * (i + 0.5);
                    Arc(c, cx, cy, R, -90, 360, p.Track, 8, false);
                    Arc(c, cx, cy, R, -90, 360 * g[i].V, Col(g[i].V), 8, true);
                    Text(c, $"{Math.Round(g[i].V * 100)}%", cx - R, cy - 12, cx + R, cy + 12, 17, p.Text, 2, 1, p);
                    Text(c, g[i].Name, cx - colW / 2, cy + R + 8, cx + colW / 2, cy + R + 26, 13, p.Text, 2, 1, p);
                    Text(c, g[i].Detail, cx - colW / 2, cy + R + 26, cx + colW / 2, cy + R + 42, 10.5, p.Sub, 0, 1, p);
                }
                break;
            }
            case "notes":
                Text(c, "备忘录", Pad + 2, Pad - 2, w - Pad, Pad + 18, 13.5, p.Orange, 2, 0, p);
                Paragraph(c, size == "small" ? "周五前交设计评审\n换上新的壁纸" : "周五前交设计评审\n换上新的壁纸\n试试磨砂预设",
                          Pad + 2, Pad + 24, w - Pad - 2, h - Pad, 13.5, p.Text, p);
                break;
            case "reminders":
            {
                (string Text, bool Done)[] items = { ("给桌面拍一组截图", false), ("整理本周的会议记录", false), ("订周末的电影票", true) };
                Text(c, "提醒事项", Pad + 2, Pad - 2, w - Pad - 40, Pad + 18, 13.5, p.Orange, 2, 0, p);
                Text(c, "2", w - Pad - 48, Pad - 6, w - Pad - 2, Pad + 22, 22, p.Orange, 2, 2, p);
                const double RowH = 27, Top = Pad + 28;
                int slots = Math.Max(1, (int)((h - Pad - Top) / RowH));
                int shown = 0;
                foreach (var it in items)
                {
                    if (shown >= slots - 1) break;
                    double y = Top + RowH * shown, cx = Pad + 9, cy = y + RowH / 2;
                    if (it.Done)
                    {
                        Circle(c, cx, cy, 8, p.Orange);
                        Circle(c, cx, cy, 3.2, p.OnAccent);
                    }
                    else Arc(c, cx, cy, 7.4, -90, 360, p.Sub, 1.5, false);
                    Text(c, it.Text, Pad + 26, y, w - Pad, y + RowH, 13.5, it.Done ? p.Sub : p.Text, 0, 0, p);
                    if (it.Done) Line(c, Pad + 26, cy, Math.Min(w - Pad, Pad + 26 + TextWidth(it.Text, 13.5, "TextFont")), cy, p.Sub, 1);
                    shown++;
                }
                if (shown < slots)
                {
                    double y = Top + RowH * shown;
                    Text(c, "+", Pad - 1, y, Pad + 19, y + RowH, 17, p.Faint, 0, 1, p);
                    Text(c, "新提醒", Pad + 26, y, w - Pad, y + RowH, 13.5, p.Faint, 0, 0, p);
                }
                break;
            }
            case "photos":
                c.Children.Add(new Rectangle { Width = w, Height = h, RadiusX = 22, RadiusY = 22, Fill = photo });
                break;
            case "weather":
                Weather(c, size, w, h, p);
                break;
        }
        return c;
    }

    // Keep a preview face while its displayed data is unchanged. This only invalidates an
    // existing render request; it does not add a clock or a polling timer to the gallery.
    public static long Stamp(string kind) => kind switch
    {
        "clock" => DateTime.Now.Ticks / TimeSpan.TicksPerSecond,
        "digital" => DateTime.Now.Ticks / TimeSpan.TicksPerMinute,
        "calendar" => DateTime.Today.Ticks,
        "battery" => BatteryStamp(),
        "weather" => WeatherStamp(),
        _ => 0,
    };

    private static long BatteryStamp()
    {
        var (fill, charging, low) = BarFace.Battery();
        return (long)Math.Round(fill * 100) | (charging ? 128L : 0) | (low ? 256L : 0);
    }

    // ---- The calendar: small, medium, and the large month with a lunar day under every date ----

    private static readonly ChineseLunisolarCalendar Lunar = new();
    private static readonly string[] LunarMonths = { "正", "二", "三", "四", "五", "六", "七", "八", "九", "十", "冬", "腊" };

    private static string LunarDay(int d)
    {
        string[] digits = { "一", "二", "三", "四", "五", "六", "七", "八", "九", "十" };
        if (d == 10) return "初十";
        if (d == 20) return "二十";
        if (d == 30) return "三十";
        return (d < 10 ? "初" : d < 20 ? "十" : d < 30 ? "廿" : "三") + digits[(d - 1) % 10];
    }

    // (month, day, leap) of a date in the Chinese calendar.
    private static (int M, int D, bool Leap) ToLunar(DateTime t)
    {
        try
        {
            int y = Lunar.GetYear(t), m = Lunar.GetMonth(t), d = Lunar.GetDayOfMonth(t), leap = Lunar.GetLeapMonth(y);
            bool isLeap = leap > 0 && m == leap;
            if (leap > 0 && m >= leap) m--;
            return (m, d, isLeap);
        }
        catch (ArgumentOutOfRangeException) { return (1, 1, false); }
    }

    internal static string LunarMonthDay(DateTime t)
    {
        var (m, d, leap) = ToLunar(t);
        return (leap ? "闰" : "") + LunarMonths[Math.Clamp(m, 1, 12) - 1] + "月" + LunarDay(d);
    }

    // ChineseCalendar::Line: "农历丙午年 九月廿三".
    internal static string LunarLine(DateTime t)
    {
        const string stems = "甲乙丙丁戊己庚辛壬癸", branches = "子丑寅卯辰巳午未申酉戌亥";
        try
        {
            int n = Lunar.GetSexagenaryYear(t);
            return $"农历{stems[(n - 1) % 10]}{branches[(n - 1) % 12]}年 {LunarMonthDay(t)}";
        }
        catch (ArgumentOutOfRangeException) { return ""; }
    }

    // ChineseCalendar::DayLabel: the festivals, else the day (the month's name on its first day).
    internal static string DayLabel(DateTime t, out bool festival)
    {
        festival = true;
        if (t.Month == 1 && t.Day == 1) return "元旦";
        if (t.Month == 5 && t.Day == 1) return "劳动节";
        if (t.Month == 10 && t.Day == 1) return "国庆节";
        var (m, d, leap) = ToLunar(t);
        if (!leap)
        {
            (int M, int D, string Name)[] fests = { (1, 1, "春节"), (1, 15, "元宵节"), (5, 5, "端午节"), (7, 7, "七夕"), (8, 15, "中秋节"), (9, 9, "重阳节"), (12, 8, "腊八节") };
            foreach (var f in fests) if (f.M == m && f.D == d) return f.Name;
            if (m == 12 && ToLunar(t.AddDays(1)) is (1, 1, false)) return "除夕";
        }
        festival = false;
        return d == 1 ? (leap ? "闰" : "") + LunarMonths[Math.Clamp(m, 1, 12) - 1] + "月" : LunarDay(d);
    }

    private static int Weekday(DateTime t) => ((int)t.DayOfWeek + 6) % 7; // 0 = Monday

    private static void Calendar(Canvas c, string size, double w, double h, DateTime now, Pal p)
    {
        int first = ((int)CultureInfo.CurrentCulture.DateTimeFormat.FirstDayOfWeek + 6) % 7; // 0 = Monday
        var firstDay = new DateTime(now.Year, now.Month, 1);
        int lead = (Weekday(firstDay) - first + 7) % 7, days = DateTime.DaysInMonth(now.Year, now.Month);
        if (size == "large")
        {
            Text(c, Months[now.Month - 1], Pad + 4, Pad, w / 2, Pad + 22, 16, p.Blue, 2, 0, p);
            Text(c, $"{now.Year}年 · 农历{LunarMonthDay(now)}", w / 2 - 40, Pad, w - Pad - 4, Pad + 22, 11.5, p.Sub, 0, 2, p);
            double gx = Pad, cw = (w - 2 * Pad) / 7, y = Pad + 28;
            for (int i = 0; i < 7; i++) Text(c, WeekShort[(first + i) % 7], gx + i * cw, y, gx + (i + 1) * cw, y + 16, 10.5, p.Sub, 2, 1, p);
            y += 20;
            int rows = (lead + days + 6) / 7;
            double rh = (h - Pad - y) / Math.Max(rows, 5);
            for (int d = 1; d <= days; d++)
            {
                int cell = lead + d - 1;
                double x0 = gx + (cell % 7) * cw, y0 = y + (cell / 7) * rh, cx = x0 + cw / 2;
                bool today = d == now.Day, weekend = (first + cell % 7) % 7 >= 5;
                if (today) Fill(c, cx - 18, y0 + 1, cx + 18, y0 + rh - 1, 10, p.Blue);
                Color ink = today ? p.OnAccent : weekend ? p.Sub : p.Text;
                Text(c, d.ToString(), x0, y0, x0 + cw, y0 + rh * 0.56, 14, ink, 2, 1, p, halo: !today);
                string label = DayLabel(new DateTime(now.Year, now.Month, d), out bool festival);
                Text(c, label, x0, y0 + rh * 0.48, x0 + cw, y0 + rh - 5, 9, today ? A(p.OnAccent, 0.85) : festival ? p.Blue : p.Sub, 0, 1, p, halo: !today);
            }
            return;
        }
        double leftW = size == "small" ? w - 2 * Pad : 126;
        Text(c, WeekLong[Weekday(now)], Pad + 2, Pad, Pad + leftW, Pad + 20, 14, p.Blue, 2, 0, p);
        Text(c, now.Day.ToString(), Pad, Pad + 16, Pad + leftW, Pad + 80, 54, p.Text, 0, 0, p);
        string todayLabel = DayLabel(now, out bool fest);
        Text(c, "农历" + LunarMonthDay(now), Pad + 2, h - Pad - 40, Pad + leftW, h - Pad - 20, 12.5, p.Text, 2, 0, p);
        Text(c, fest ? todayLabel : $"{now.Year}年{Months[now.Month - 1]}", Pad + 2, h - Pad - 20, Pad + leftW, h - Pad, 12, fest ? p.Blue : p.Sub, 0, 0, p);
        if (size == "small") return;
        double gx2 = Pad + leftW + 6, gw = w - Pad - gx2, cw2 = gw / 7;
        Text(c, Months[now.Month - 1], gx2 + 4, Pad - 1, gx2 + gw, Pad + 17, 12.5, p.Blue, 2, 0, p);
        double yy = Pad + 20;
        for (int i = 0; i < 7; i++) Text(c, WeekShort[(first + i) % 7], gx2 + i * cw2, yy, gx2 + (i + 1) * cw2, yy + 14, 9.5, p.Sub, 2, 1, p);
        yy += 16;
        int rows2 = (lead + days + 6) / 7;
        double rh2 = Math.Min(17, (h - Pad - yy) / rows2);
        for (int d = 1; d <= days; d++)
        {
            int cell = lead + d - 1;
            double x0 = gx2 + (cell % 7) * cw2, y0 = yy + (cell / 7) * rh2, cx = x0 + cw2 / 2, cy = y0 + rh2 / 2;
            bool today = d == now.Day, weekend = (first + cell % 7) % 7 >= 5;
            if (today) Circle(c, cx, cy, rh2 / 2 + 0.5, p.Blue);
            Text(c, d.ToString(), x0, cy - rh2 / 2, x0 + cw2, cy + rh2 / 2, 10.5,
                 today ? p.OnAccent : weekend ? p.Sub : p.Text, 2, 1, p, halo: !today);
        }
    }

    // ---- The analogue face: numbers or ticks, hands with a thin neck, the orange second hand ----

    private static void ClockFace(Canvas c, double cx, double cy, double R, DateTime t, Color face, Color ink, bool numbers, string? label, Color labelInk, Pal p)
    {
        Circle(c, cx, cy, R, face);
        for (int hr = 1; hr <= 12; hr++)
        {
            double a = (hr * 30 - 90) * Math.PI / 180;
            if (numbers)
            {
                double r = R * 0.78, fs = R * 0.21, x = cx + r * Math.Cos(a), y = cy + r * Math.Sin(a);
                Text(c, hr.ToString(), x - fs, y - fs * 0.72, x + fs, y + fs * 0.72, fs, ink, 2, 1, p);
            }
            else
            {
                double r0 = R * 0.80, r1 = R * 0.92;
                Line(c, cx + r0 * Math.Cos(a), cy + r0 * Math.Sin(a), cx + r1 * Math.Cos(a), cy + r1 * Math.Sin(a), A(ink, hr % 3 != 0 ? 0.45 : 0.9), Math.Max(1, R * 0.045));
            }
        }
        if (label != null) Text(c, label, cx - R, cy + R * 0.26, cx + R, cy + R * 0.26 + R * 0.2, R * 0.15, labelInk, 2, 1, p);
        double sec = t.Second, min = t.Minute + sec / 60, hour = t.Hour % 12 + min / 60;
        void Hand(double angle, double length, double width)
        {
            double a = (angle - 90) * Math.PI / 180, neck = R * 0.16, dx = Math.Cos(a), dy = Math.Sin(a);
            Line(c, cx, cy, cx + dx * neck, cy + dy * neck, ink, Math.Max(1.2, width * 0.36));
            Line(c, cx + dx * neck, cy + dy * neck, cx + dx * length, cy + dy * length, ink, width);
        }
        Hand(hour * 30, R * 0.52, Math.Max(2.5, R * 0.075));
        Hand(min * 6, R * 0.80, Math.Max(2.5, R * 0.075));
        Circle(c, cx, cy, Math.Max(2.2, R * 0.055), ink);
        double sa = (sec * 6 - 90) * Math.PI / 180;
        Line(c, cx - Math.Cos(sa) * R * 0.18, cy - Math.Sin(sa) * R * 0.18, cx + Math.Cos(sa) * R * 0.86, cy + Math.Sin(sa) * R * 0.86, p.Orange, Math.Max(1, R * 0.02));
        Circle(c, cx, cy, Math.Max(1.8, R * 0.04), p.Orange);
    }

    // ---- Drawing helpers (Canvas in surface.cpp) ----

    private static Color A(Color c, double a) => Color.FromArgb((byte)Math.Round(c.A * a), c.R, c.G, c.B);

    // weight: 0 regular, 1 medium, 2 semibold; align 0 left, 1 centre, 2 right; one line, centred in the rect.
    // On glass, 12.5 DIP and below a regular is set a step heavier, and every line has its halo below.
    private static void Text(Canvas c, string s, double l, double t, double r, double b, double size, Color ink, int weight, int align, Pal p, bool halo = true)
    {
        if (size <= 12.5 && weight == 0) weight = 1;
        string font = weight switch { 0 => "TextFont", 1 => "EmphasisFont", _ => "TitleFont" };
        double o = 0.6;
        for (int pass = halo ? 0 : 1; pass < 2; pass++)
        {
            var tb = new TextBlock
            {
                Text = s, FontSize = size, Foreground = new SolidColorBrush(pass == 0 ? A(p.Halo, ink.A / 255.0) : ink),
                FontFamily = (FontFamily)Application.Current.Resources[font], TextWrapping = TextWrapping.NoWrap,
                TextAlignment = align == 1 ? TextAlignment.Center : align == 2 ? TextAlignment.Right : TextAlignment.Left,
                VerticalAlignment = VerticalAlignment.Center, IsTextScaleFactorEnabled = false,
            };
            // Left-aligned lines may run past their box (DirectWrite lays them out unwrapped too); the others align in it.
            if (align != 0) tb.Width = Math.Max(1, r - l);
            Typography.SetNumeralAlignment(tb, FontNumeralAlignment.Tabular);
            // Centred in its box: MiSans' line box is 1.326 em with the figures in its middle.
            double line = size * 1.326;
            tb.Height = line;
            Canvas.SetLeft(tb, l);
            Canvas.SetTop(tb, t + (b - t - line) / 2 + (pass == 0 ? o : 0));
            c.Children.Add(tb);
        }
    }

    private static double TextWidth(string s, double size, string font)
    {
        var tb = new TextBlock { Text = s, FontSize = size, FontFamily = (FontFamily)Application.Current.Resources[font], IsTextScaleFactorEnabled = false };
        Typography.SetNumeralAlignment(tb, FontNumeralAlignment.Tabular);
        tb.Measure(new Windows.Foundation.Size(double.PositiveInfinity, double.PositiveInfinity));
        return tb.DesiredSize.Width;
    }

    private static void Paragraph(Canvas c, string s, double l, double t, double r, double b, double size, Color ink, Pal p)
    {
        var tb = new TextBlock
        {
            Text = s, FontSize = size, Foreground = new SolidColorBrush(ink), FontFamily = (FontFamily)Application.Current.Resources["TextFont"],
            TextWrapping = TextWrapping.Wrap, Width = r - l, MaxHeight = b - t, IsTextScaleFactorEnabled = false,
        };
        Canvas.SetLeft(tb, l);
        Canvas.SetTop(tb, t);
        c.Children.Add(tb);
    }

    private static void Glyph(Canvas c, string g, double l, double t, double r, double b, double size, Color ink)
    {
        var f = new FontIcon { Glyph = g, FontSize = size, Foreground = new SolidColorBrush(ink), Width = r - l, Height = b - t, FontFamily = new FontFamily("Segoe Fluent Icons") };
        Canvas.SetLeft(f, l);
        Canvas.SetTop(f, t);
        c.Children.Add(f);
    }

    private static void Circle(Canvas c, double cx, double cy, double r, Color fill)
    {
        var e = new Ellipse { Width = 2 * r, Height = 2 * r, Fill = new SolidColorBrush(fill) };
        Canvas.SetLeft(e, cx - r);
        Canvas.SetTop(e, cy - r);
        c.Children.Add(e);
    }

    private static void Fill(Canvas c, double l, double t, double r, double b, double radius, Color fill)
    {
        var e = new Rectangle { Width = Math.Max(0, r - l), Height = Math.Max(0, b - t), RadiusX = radius, RadiusY = radius, Fill = new SolidColorBrush(fill) };
        Canvas.SetLeft(e, l);
        Canvas.SetTop(e, t);
        c.Children.Add(e);
    }

    private static void Line(Canvas c, double x1, double y1, double x2, double y2, Color ink, double width) =>
        c.Children.Add(new Line { X1 = x1, Y1 = y1, X2 = x2, Y2 = y2, Stroke = new SolidColorBrush(ink), StrokeThickness = width,
                                  StrokeStartLineCap = PenLineCap.Round, StrokeEndLineCap = PenLineCap.Round });

    // An arc from `from` degrees (0 = right, -90 = top) over `sweep`, clockwise.
    private static void Arc(Canvas c, double cx, double cy, double r, double from, double sweep, Color ink, double width, bool caps)
    {
        if (sweep <= 0.01) return;
        if (sweep >= 359.99)
        {
            var ring = new Ellipse { Width = 2 * r + width, Height = 2 * r + width, Stroke = new SolidColorBrush(ink), StrokeThickness = width };
            Canvas.SetLeft(ring, cx - r - width / 2);
            Canvas.SetTop(ring, cy - r - width / 2);
            c.Children.Add(ring);
            return;
        }
        Point At(double deg) => new(cx + r * Math.Cos(deg * Math.PI / 180), cy + r * Math.Sin(deg * Math.PI / 180));
        var f = new PathFigure { StartPoint = At(from), IsClosed = false, IsFilled = false };
        f.Segments.Add(new ArcSegment { Point = At(from + sweep), Size = new Windows.Foundation.Size(r, r), SweepDirection = SweepDirection.Clockwise, IsLargeArc = sweep > 180 });
        c.Children.Add(new Path
        {
            Data = new PathGeometry { Figures = { f } }, Stroke = new SolidColorBrush(ink), StrokeThickness = width,
            StrokeStartLineCap = caps ? PenLineCap.Round : PenLineCap.Flat, StrokeEndLineCap = caps ? PenLineCap.Round : PenLineCap.Flat,
        });
    }
}
