#include "ui/widget_faces.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace atrium {

namespace {

D2D1_COLOR_F Rgba(uint32_t rgb, float a) { return D2D1::ColorF(rgb, a); }
D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) { return D2D1::ColorF(c.r, c.g, c.b, c.a * a); }

// What the weather's symbols are drawn over: light ink (clear glass, a sky), dark ink (bright
// glass: grey clouds, a gold moon), or the dimmed glass, where everything is white.
enum class Ink { Light, Dark, Mono };

// The colours of a widget: its ink, and macOS's accents.
struct Pal {
    D2D1_COLOR_F text, sub, faint, face, red, orange, green, track, blue; // blue: the calendar's accent
    D2D1_COLOR_F onAccent;  // ink on an accent's disc (today, a done reminder)
    D2D1_COLOR_F nightFace; // the world clock's faces where it is night
    Ink icon;
};

Pal GlassPal(const Theme& t)
{
    return {t.text, t.subtext, t.faint, Alpha(t.text, 0.10f), Rgba(0xFF453A, 0.98f), Rgba(0xFF9F0A, 0.98f), Rgba(0x30D158, 0.98f),
            Alpha(t.text, 0.18f), Rgba(0x0A84FF, 0.98f), Rgba(0xFFFFFF, 0.98f), Rgba(0x000000, 0.42f), t.light ? Ink::Dark : Ink::Light};
}

// Dusk: white only, the accents too (macOS's dimmed widgets are monochrome); a disc's ink is dark.
Pal DuskPal()
{
    const D2D1_COLOR_F white = Rgba(0xFFFFFF, 0.96f);
    return {white, Rgba(0xFFFFFF, 0.72f), Rgba(0xFFFFFF, 0.50f), Rgba(0xFFFFFF, 0.12f), white, white, white, Rgba(0xFFFFFF, 0.22f), white,
            Rgba(0x1C1C1E, 0.92f), Rgba(0x000000, 0.32f), Ink::Mono};
}

// Color, on a white face: dark ink and macOS's light-mode accents.
Pal PaperPal()
{
    const D2D1_COLOR_F ink = Rgba(0x1D1D1F, 0.94f);
    return {ink, Rgba(0x1D1D1F, 0.58f), Rgba(0x1D1D1F, 0.36f), Rgba(0x1D1D1F, 0.05f), Rgba(0xFF3B30, 1), Rgba(0xFF9500, 1), Rgba(0x34C759, 1),
            Rgba(0x1D1D1F, 0.10f), Rgba(0x007AFF, 1), Rgba(0xFFFFFF, 0.98f), Rgba(0x1C1C1E, 1), Ink::Dark};
}

const wchar_t* const kWeekLong[] = {L"星期一", L"星期二", L"星期三", L"星期四", L"星期五", L"星期六", L"星期日"};
const wchar_t* const kWeekShort[] = {L"一", L"二", L"三", L"四", L"五", L"六", L"日"};
const wchar_t* const kMonths[] = {L"一月", L"二月", L"三月", L"四月", L"五月", L"六月",
                                  L"七月", L"八月", L"九月", L"十月", L"十一月", L"十二月"};

bool ZoneInfo(const wchar_t* key, DYNAMIC_TIME_ZONE_INFORMATION& out)
{
    static std::map<std::wstring, DYNAMIC_TIME_ZONE_INFORMATION> zones; // read once
    if (zones.empty()) {
        DYNAMIC_TIME_ZONE_INFORMATION d{};
        for (DWORD i = 0; EnumDynamicTimeZoneInformation(i, &d) == ERROR_SUCCESS; ++i)
            zones[d.TimeZoneKeyName] = d;
    }
    auto it = zones.find(key);
    if (it == zones.end())
        return false;
    out = it->second;
    return true;
}

int64_t Minutes(const SYSTEMTIME& t)
{
    FILETIME f{};
    SystemTimeToFileTime(&t, &f);
    return int64_t((ULARGE_INTEGER{f.dwLowDateTime, f.dwHighDateTime}.QuadPart) / 600000000ULL);
}

// The time in a city, and how far it is from here: "今天 +1小时" (half hours as "+5.5小时").
SYSTEMTIME CityTime(const std::wstring& name, const SYSTEMTIME& now, std::wstring& when)
{
    SYSTEMTIME utc, ct = now;
    GetSystemTime(&utc);
    DYNAMIC_TIME_ZONE_INFORMATION z{};
    for (const WorldCity& city : WorldCities())
        if (name == city.name && ZoneInfo(city.zone, z))
            SystemTimeToTzSpecificLocalTimeEx(&z, &utc, &ct);
    const int64_t minutes = Minutes(ct) - Minutes(now);
    // Minutes() floors the seconds: a quarter-hour zone may come out a minute short.
    const int64_t quarters = (minutes + (minutes >= 0 ? 7 : -7)) / 15;
    int dayDiff = int((Minutes(ct) / 1440) - (Minutes(now) / 1440));
    when = dayDiff > 0 ? L"明天" : dayDiff < 0 ? L"昨天" : L"今天";
    std::wstring hours = std::to_wstring(quarters / 4);
    if (quarters % 4) {
        wchar_t b[16];
        swprintf_s(b, L"%g", double(quarters) / 4);
        hours = b;
    }
    when += (quarters >= 0 ? L" +" : L" ") + hours + L"小时";
    return ct;
}

} // namespace

const std::vector<WorldCity>& WorldCities()
{
    static const std::vector<WorldCity> cities = {
        {L"北京", L"China Standard Time"},          {L"上海", L"China Standard Time"},        {L"香港", L"China Standard Time"},
        {L"台北", L"Taipei Standard Time"},         {L"东京", L"Tokyo Standard Time"},        {L"首尔", L"Korea Standard Time"},
        {L"新加坡", L"Singapore Standard Time"},    {L"曼谷", L"SE Asia Standard Time"},      {L"新德里", L"India Standard Time"},
        {L"迪拜", L"Arabian Standard Time"},        {L"莫斯科", L"Russian Standard Time"},    {L"伊斯坦布尔", L"Turkey Standard Time"},
        {L"开罗", L"Egypt Standard Time"},          {L"约翰内斯堡", L"South Africa Standard Time"}, {L"柏林", L"W. Europe Standard Time"},
        {L"巴黎", L"Romance Standard Time"},        {L"伦敦", L"GMT Standard Time"},          {L"圣保罗", L"E. South America Standard Time"},
        {L"纽约", L"Eastern Standard Time"},        {L"多伦多", L"Eastern Standard Time"},    {L"芝加哥", L"Central Standard Time"},
        {L"丹佛", L"Mountain Standard Time"},       {L"洛杉矶", L"Pacific Standard Time"},    {L"温哥华", L"Pacific Standard Time"},
        {L"安克雷奇", L"Alaskan Standard Time"},    {L"檀香山", L"Hawaiian Standard Time"},   {L"悉尼", L"AUS Eastern Standard Time"},
        {L"墨尔本", L"AUS Eastern Standard Time"},  {L"奥克兰", L"New Zealand Standard Time"},
    };
    return cities;
}

bool HasWorldClocks(WidgetKind kind, WidgetSize size) { return size != WidgetSize::Small && (kind == WidgetKind::Clock || kind == WidgetKind::Digital); }

std::vector<std::wstring> WidgetCities(WidgetKind kind, const std::vector<std::wstring>& chosen)
{
    // macOS's medium Clock shows four cities; the digital one lists three beside the local time.
    std::vector<std::wstring> cities = kind == WidgetKind::Digital ? std::vector<std::wstring>{L"东京", L"伦敦", L"纽约"}
                                                                  : std::vector<std::wstring>{L"北京", L"东京", L"伦敦", L"纽约"};
    for (size_t i = 0; i < cities.size() && i < chosen.size(); ++i)
        if (std::any_of(WorldCities().begin(), WorldCities().end(), [&](const WorldCity& c) { return chosen[i] == c.name; }))
            cities[i] = chosen[i];
    return cities;
}

namespace {

std::wstring HourMinute(const SYSTEMTIME& t)
{
    wchar_t b[16];
    swprintf_s(b, L"%d:%02d", t.wHour, t.wMinute);
    return b;
}

int FirstWeekday()
{
    DWORD fd = 0;
    if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK | LOCALE_RETURN_NUMBER, reinterpret_cast<LPWSTR>(&fd), 2))
        return int(fd % 7);
    return 0;
}

// An analogue face: hands as on macOS (a thin neck, then the blade), the
// second hand in orange over the others. numbers: 1-12 inside the rim, else ticks.
void ClockFace(Canvas& c, float cx, float cy, float R, const SYSTEMTIME& t, const Pal& p, D2D1_COLOR_F face, D2D1_COLOR_F ink, bool numbers,
               const std::wstring& label, D2D1_COLOR_F labelInk)
{
    constexpr float kDeg = 3.14159265f / 180.f;
    c.Circle(cx, cy, R, face);
    for (int h = 1; h <= 12; ++h) {
        float a = (float(h) * 30.f - 90.f) * kDeg;
        if (numbers) {
            float r = R * 0.78f, fs = R * 0.21f;
            std::wstring n = std::to_wstring(h);
            c.Text(n, D2D1::RectF(cx + r * std::cos(a) - fs, cy + r * std::sin(a) - fs * 0.72f, cx + r * std::cos(a) + fs, cy + r * std::sin(a) + fs * 0.72f), fs,
                   ink, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        } else {
            float r0 = R * 0.80f, r1 = R * 0.92f;
            c.Capsule(cx + r0 * std::cos(a), cy + r0 * std::sin(a), cx + r1 * std::cos(a), cy + r1 * std::sin(a), Alpha(ink, h % 3 ? 0.45f : 0.9f),
                      std::max(1.f, R * 0.045f));
        }
    }
    if (!label.empty())
        c.Text(label, D2D1::RectF(cx - R, cy + R * 0.26f, cx + R, cy + R * 0.26f + R * 0.2f), R * 0.15f, labelInk, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    float sec = float(t.wSecond), min = float(t.wMinute) + sec / 60.f, hour = float(t.wHour % 12) + min / 60.f;
    auto hand = [&](float angle, float length, float width) {
        float a = (angle - 90.f) * kDeg, neck = R * 0.16f;
        float dx = std::cos(a), dy = std::sin(a);
        c.Capsule(cx, cy, cx + dx * neck, cy + dy * neck, ink, std::max(1.2f, width * 0.36f));
        c.Capsule(cx + dx * neck, cy + dy * neck, cx + dx * length, cy + dy * length, ink, width);
    };
    hand(hour * 30.f, R * 0.52f, std::max(2.5f, R * 0.075f));
    hand(min * 6.f, R * 0.80f, std::max(2.5f, R * 0.075f));
    c.Circle(cx, cy, std::max(2.2f, R * 0.055f), ink);
    float a = (sec * 6.f - 90.f) * kDeg;
    c.Capsule(cx - std::cos(a) * R * 0.18f, cy - std::sin(a) * R * 0.18f, cx + std::cos(a) * R * 0.86f, cy + std::sin(a) * R * 0.86f, p.orange,
              std::max(1.f, R * 0.02f));
    c.Circle(cx, cy, std::max(1.8f, R * 0.04f), p.orange);
    c.Circle(cx, cy, std::max(0.8f, R * 0.016f), face);
}

constexpr float kPad = 14;

void PaintClock(Canvas& c, WidgetSize size, const WidgetContent& content, const WidgetWorld& w, const Pal& p, float W, float H)
{
    if (size == WidgetSize::Small) {
        ClockFace(c, W / 2, H / 2, H / 2 - 9, w.now, p, p.face, p.text, true, w.city, p.sub);
        return;
    }
    // The world clock: four faces, dark where it is night.
    const float colW = (W - 2 * kPad) / 4, R = 31;
    const std::vector<std::wstring> cities = WidgetCities(WidgetKind::Clock, content.cities);
    for (int i = 0; i < 4; ++i) {
        std::wstring when;
        SYSTEMTIME ct = CityTime(cities[size_t(i)], w.now, when);
        bool night = ct.wHour < 6 || ct.wHour >= 18;
        float cx = kPad + colW * (float(i) + 0.5f), cy = (H - 2 * R - 42) / 2 + R - 2;
        D2D1_COLOR_F face = night ? p.nightFace : p.face, ink = night ? Rgba(0xFFFFFF, 0.96f) : p.text;
        ClockFace(c, cx, cy, R, ct, p, face, ink, false, {}, ink);
        c.Text(cities[size_t(i)], D2D1::RectF(cx - colW / 2, cy + R + 8, cx + colW / 2, cy + R + 26), 12.5f, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        c.Text(when, D2D1::RectF(cx - colW / 2, cy + R + 26, cx + colW / 2, cy + R + 42), 10.5f, p.sub, DWRITE_FONT_WEIGHT_NORMAL, 1);
    }
}

// Digits instead of hands: the time large, the day under it; the medium one
// lists three more cities on the right.
void PaintDigital(Canvas& c, WidgetSize size, const WidgetContent& content, const WidgetWorld& w, const Pal& p, float W, float H)
{
    const int wd = Weekday(w.now.wYear, w.now.wMonth, w.now.wDay);
    const float leftW = size == WidgetSize::Small ? W - 2 * kPad : 150;
    c.Text(w.city.empty() ? L"本地" : w.city, D2D1::RectF(kPad + 2, kPad, kPad + leftW, kPad + 18), 12.5f, p.sub, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    // Keep one size all day. Leave one DIP for glyph placement / rounding:
    // tabular figures can still differ slightly at the exact trimming boundary.
    const float fit = c.TextWidth(L"00:00", 50, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const float timeW = leftW - 1;
    const float timeSize = fit > timeW ? 50 * timeW / fit : 50.f;
    c.Text(HourMinute(w.now), D2D1::RectF(kPad, kPad + 22, kPad + leftW, kPad + 84), timeSize, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    c.Text(kWeekLong[wd], D2D1::RectF(kPad + 2, H - kPad - 40, kPad + leftW, H - kPad - 20), 13, p.blue, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    c.Text(std::to_wstring(w.now.wMonth) + L"月" + std::to_wstring(w.now.wDay) + L"日", D2D1::RectF(kPad + 2, H - kPad - 20, kPad + leftW, H - kPad), 13,
           p.text, DWRITE_FONT_WEIGHT_NORMAL);
    if (size == WidgetSize::Small)
        return;
    const float x0 = kPad + leftW + 8, rowH = (H - 2 * kPad) / 3;
    const std::vector<std::wstring> cities = WidgetCities(WidgetKind::Digital, content.cities);
    for (int i = 0; i < 3; ++i) {
        std::wstring when;
        SYSTEMTIME ct = CityTime(cities[size_t(i)], w.now, when);
        float y = kPad + rowH * float(i);
        if (i > 0)
            c.Line(x0, y, W - kPad, y, Alpha(p.text, 0.12f));
        c.Text(cities[size_t(i)], D2D1::RectF(x0, y + 4, W - kPad - 70, y + rowH / 2 + 2), 13, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        c.Text(when, D2D1::RectF(x0, y + rowH / 2, W - kPad - 70, y + rowH - 4), 10.5f, p.sub);
        c.Text(HourMinute(ct), D2D1::RectF(W - kPad - 80, y, W - kPad, y + rowH), 22, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 2);
    }
}

// The day: the weekday in red, the date large, the lunar date (a festival in
// red); the medium one adds the month, the large one is the month with the
// lunar day under every date.
void PaintCalendar(Canvas& c, WidgetSize size, const WidgetWorld& w, const Pal& p, float W, float H)
{
    const SYSTEMTIME& now = w.now;
    const int first = FirstWeekday();
    const int lead = (Weekday(now.wYear, now.wMonth, 1) - first + 7) % 7, days = DaysIn(now.wYear, now.wMonth);
    if (size == WidgetSize::Large) {
        c.Text(kMonths[now.wMonth - 1], D2D1::RectF(kPad + 4, kPad, W / 2, kPad + 22), 16, p.blue, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        std::wstring lunarToday = w.lunar ? w.lunar->MonthDay(now.wYear, now.wMonth, now.wDay) : std::wstring();
        c.Text(lunarToday.empty() ? std::to_wstring(now.wYear) + L"年" : std::to_wstring(now.wYear) + L"年 · 农历" + lunarToday,
               D2D1::RectF(W / 2 - 40, kPad, W - kPad - 4, kPad + 22), 11.5f, p.sub, DWRITE_FONT_WEIGHT_NORMAL, 2);
        const float gx = kPad, gw = W - 2 * kPad, cw = gw / 7;
        float y = kPad + 28;
        for (int i = 0; i < 7; ++i)
            c.Text(kWeekShort[(first + i) % 7], D2D1::RectF(gx + float(i) * cw, y, gx + float(i + 1) * cw, y + 16), 10.5f, p.sub,
                   DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        y += 20;
        const int rows = (lead + days + 6) / 7;
        const float rh = (H - kPad - y) / float(std::max(rows, 5));
        for (int d = 1; d <= days; ++d) {
            int cell = lead + d - 1;
            float x0 = gx + float(cell % 7) * cw, y0 = y + float(cell / 7) * rh, cx = x0 + cw / 2;
            bool today = d == now.wDay, weekend = (first + cell % 7) % 7 >= 5;
            if (today)
                c.Fill(D2D1::RectF(cx - 18, y0 + 1, cx + 18, y0 + rh - 1), 10, p.blue);
            D2D1_COLOR_F ink = today ? p.onAccent : weekend ? p.sub : p.text;
            c.Text(std::to_wstring(d), D2D1::RectF(x0, y0, x0 + cw, y0 + rh * 0.56f), 14, ink, today ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_SEMI_BOLD,
                   1);
            bool festival = false;
            std::wstring label = w.lunar ? w.lunar->DayLabel(now.wYear, now.wMonth, WORD(d), &festival) : std::wstring();
            c.Text(label, D2D1::RectF(x0, y0 + rh * 0.48f, x0 + cw, y0 + rh - 5), 9, today ? Alpha(p.onAccent, 0.85f) : festival ? p.blue : p.sub,
                   DWRITE_FONT_WEIGHT_NORMAL, 1);
        }
        return;
    }
    const int wd = Weekday(now.wYear, now.wMonth, now.wDay);
    const float leftW = size == WidgetSize::Small ? W - 2 * kPad : 126;
    c.Text(kWeekLong[wd], D2D1::RectF(kPad + 2, kPad, kPad + leftW, kPad + 20), 14, p.blue, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    c.Text(std::to_wstring(now.wDay), D2D1::RectF(kPad, kPad + 16, kPad + leftW, kPad + 80), 54, p.text, DWRITE_FONT_WEIGHT_NORMAL);
    bool festival = false;
    std::wstring label = w.lunar ? w.lunar->DayLabel(now.wYear, now.wMonth, now.wDay, &festival) : std::wstring();
    std::wstring lunar = w.lunar ? w.lunar->MonthDay(now.wYear, now.wMonth, now.wDay) : std::wstring();
    c.Text(lunar.empty() ? std::wstring(kMonths[now.wMonth - 1]) : L"农历" + lunar, D2D1::RectF(kPad + 2, H - kPad - 40, kPad + leftW, H - kPad - 20), 12.5f,
           p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    c.Text(festival ? label : std::to_wstring(now.wYear) + L"年" + kMonths[now.wMonth - 1], D2D1::RectF(kPad + 2, H - kPad - 20, kPad + leftW, H - kPad), 12,
           festival ? p.blue : p.sub, DWRITE_FONT_WEIGHT_NORMAL);
    if (size == WidgetSize::Small)
        return;
    // The month: weekday initials, the days, today in a red disc.
    const float gx = kPad + leftW + 6, gw = W - kPad - gx, cw = gw / 7;
    c.Text(kMonths[now.wMonth - 1], D2D1::RectF(gx + 4, kPad - 1, gx + gw, kPad + 17), 12.5f, p.blue, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    float y = kPad + 20;
    for (int i = 0; i < 7; ++i)
        c.Text(kWeekShort[(first + i) % 7], D2D1::RectF(gx + float(i) * cw, y, gx + float(i + 1) * cw, y + 14), 9.5f, p.sub, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    y += 16;
    int rows = (lead + days + 6) / 7;
    float rh = std::min(17.f, (H - kPad - y) / float(rows));
    for (int d = 1; d <= days; ++d) {
        int cell = lead + d - 1;
        float x0 = gx + float(cell % 7) * cw, y0 = y + float(cell / 7) * rh, cx = x0 + cw / 2, cy = y0 + rh / 2;
        bool today = d == now.wDay;
        if (today)
            c.Circle(cx, cy, rh / 2 + 0.5f, p.blue);
        bool weekend = (first + cell % 7) % 7 >= 5;
        D2D1_COLOR_F ink = today ? p.onAccent : weekend ? p.sub : p.text;
        // MiSans centres its figures in the line box (ascent 1.044, descent 0.282, figures 0.76
        // high): the box centred on the disc centres the digits.
        c.Text(std::to_wstring(d), D2D1::RectF(x0, cy - rh / 2, x0 + cw, cy + rh / 2), 10.5f, ink,
               today ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    }
}

// Batteries: a ring round the computer, the level under it.
void PaintBattery(Canvas& c, const WidgetWorld& w, const Pal& p, float W, float H)
{
    const float cx = W / 2, cy = H / 2 - 12, R = 44;
    c.Arc(cx, cy, R, -90, 270, p.track, 9);
    if (w.status.battery) {
        int pct = std::clamp(w.status.batteryPercent, 0, 100);
        D2D1_COLOR_F level = pct <= 20 && !w.status.charging ? p.red : p.green;
        c.Arc(cx, cy, R, -90, -90 + 360.f * float(pct) / 100.f, level, 9);
        c.Glyph(L"\uE7F8", D2D1::RectF(cx - 22, cy - 22, cx + 22, cy + 22), 24, p.text);
        // The level, with a bolt before it while charging.
        std::wstring n = std::to_wstring(pct) + L"%";
        float tw = c.TextWidth(n, 19, DWRITE_FONT_WEIGHT_SEMI_BOLD), bolt = w.status.charging ? 16.f : 0.f, x0 = (W - tw - bolt) / 2;
        if (w.status.charging)
            c.Glyph(L"\uE945", D2D1::RectF(x0 - 2, cy + R + 10, x0 + bolt, cy + R + 36), 13, level);
        c.Text(n, D2D1::RectF(x0 + bolt, cy + R + 10, x0 + bolt + tw + 4, cy + R + 36), 19, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    } else {
        c.Arc(cx, cy, R, -90, 270, p.green, 9);
        c.Glyph(L"\uE83E", D2D1::RectF(cx - 22, cy - 22, cx + 22, cy + 22), 22, p.text);
        c.Text(L"已接通电源", D2D1::RectF(0, cy + R + 12, W, cy + R + 34), 13, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    }
    (void)H;
}

// The processor, the memory and the system disk: bars (small) or rings
// (medium), green, orange from 70 %, red from 90 %.
void PaintSystem(Canvas& c, WidgetSize size, const WidgetWorld& w, const Pal& p, float W, float H)
{
    struct Gauge {
        const wchar_t* name;
        float value;
        std::wstring detail;
    };
    wchar_t mem[32], disk[32];
    swprintf_s(mem, L"%.1f / %.0f GB", w.memoryGb, w.memoryTotalGb);
    swprintf_s(disk, L"%.0f GB 可用", w.diskFreeGb);
    const Gauge gauges[] = {{L"CPU", w.cpu, L"处理器"}, {L"内存", w.memory, mem}, {L"磁盘", w.disk, disk}};
    auto colour = [&](float v) { return v >= 0.9f ? p.red : v >= 0.7f ? p.orange : p.green; };
    auto pct = [](float v) { return std::to_wstring(int(std::lround(std::clamp(v, 0.f, 1.f) * 100))) + L"%"; };
    if (size == WidgetSize::Small) {
        c.Text(L"系统", D2D1::RectF(kPad + 2, kPad - 2, W - kPad, kPad + 16), 12.5f, p.sub, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        const float rowH = (H - 2 * kPad - 18) / 3;
        for (int i = 0; i < 3; ++i) {
            float y = kPad + 18 + rowH * float(i);
            c.Text(gauges[i].name, D2D1::RectF(kPad + 2, y + 2, W / 2, y + 22), 13, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            c.Text(pct(gauges[i].value), D2D1::RectF(W / 2, y + 2, W - kPad - 2, y + 22), 13, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 2);
            D2D1_RECT_F track = D2D1::RectF(kPad + 2, y + 26, W - kPad - 2, y + 32);
            c.Fill(track, 3, p.track);
            float v = std::clamp(gauges[i].value, 0.f, 1.f);
            if (v > 0.01f)
                c.Fill(D2D1::RectF(track.left, track.top, track.left + std::max(6.f, (track.right - track.left) * v), track.bottom), 3, colour(v));
        }
        return;
    }
    const float colW = (W - 2 * kPad) / 3, R = 34, cy = kPad + 6 + R;
    for (int i = 0; i < 3; ++i) {
        float cx = kPad + colW * (float(i) + 0.5f), v = std::clamp(gauges[i].value, 0.f, 1.f);
        c.Arc(cx, cy, R, -90, 270, p.track, 8);
        if (v > 0.005f)
            c.Arc(cx, cy, R, -90, -90 + 360.f * v, colour(v), 8);
        c.Text(pct(v), D2D1::RectF(cx - R, cy - 12, cx + R, cy + 12), 17, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        c.Text(gauges[i].name, D2D1::RectF(cx - colW / 2, cy + R + 8, cx + colW / 2, cy + R + 26), 13, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        c.Text(gauges[i].detail, D2D1::RectF(cx - colW / 2, cy + R + 26, cx + colW / 2, cy + R + 42), 10.5f, p.sub, DWRITE_FONT_WEIGHT_NORMAL, 1);
    }
}

// Bluetooth devices' batteries (macOS's Batteries widget): small, four rings, each the device's
// symbol over its level; medium, four rows of symbol, name, bar and level. Red at 20 % or less.
const wchar_t* BluetoothGlyph(BluetoothKind k)
{
    switch (k) {
    case BluetoothKind::Headset: return L"\uE7F6";
    case BluetoothKind::Gamepad: return L"\uE7FC";
    case BluetoothKind::Keyboard: return L"\uE765";
    case BluetoothKind::Mouse: return L"\uE962";
    case BluetoothKind::Pen: return L"\uEDC6";
    case BluetoothKind::Phone: return L"\uE8EA";
    default: return L"\uE702";
    }
}

void PaintBluetooth(Canvas& c, WidgetSize size, const WidgetWorld& w, const Pal& p, float W, float H)
{
    const auto& devices = w.bluetooth;
    const size_t n = std::min<size_t>(devices.size(), 4);
    auto colour = [&](int pct) { return pct <= 20 ? p.red : p.green; };
    if (w.bluetoothRead && n == 0) {
        c.Glyph(L"\uE702", D2D1::RectF(0, H / 2 - 40, W, H / 2 - 4), 26, p.sub);
        c.Text(L"没有已连接的设备", D2D1::RectF(kPad, H / 2 + 4, W - kPad, H / 2 + 24), 12.5f, p.sub, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        return;
    }
    if (size == WidgetSize::Small) {
        const float cw = (W - 2 * kPad) / 2, ch = (H - 2 * kPad) / 2, R = 21;
        for (size_t i = 0; i < 4; ++i) {
            const float cx = kPad + cw * (float(i % 2) + 0.5f), cy = kPad + ch * float(i / 2) + 26;
            c.Arc(cx, cy, R, -90, 270, p.track, 5);
            if (i >= n)
                continue;
            const int pct = std::clamp(devices[i].percent, 0, 100);
            if (pct > 0)
                c.Arc(cx, cy, R, -90, -90 + 360.f * float(pct) / 100.f, colour(pct), 5);
            c.Glyph(BluetoothGlyph(devices[i].kind), D2D1::RectF(cx - R, cy - R, cx + R, cy + R), 15, p.text);
            c.Text(std::to_wstring(pct) + L"%", D2D1::RectF(cx - cw / 2, cy + R + 4, cx + cw / 2, cy + R + 20), 11.5f, p.text,
                   DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        }
        return;
    }
    const float rowH = (H - 2 * kPad) / 4;
    for (size_t i = 0; i < n; ++i) {
        const float y = kPad + rowH * float(i), mid = y + rowH / 2;
        const int pct = std::clamp(devices[i].percent, 0, 100);
        c.Glyph(BluetoothGlyph(devices[i].kind), D2D1::RectF(kPad, mid - 12, kPad + 26, mid + 12), 16, p.text);
        c.Text(devices[i].name, D2D1::RectF(kPad + 34, mid - 10, W - kPad - 112, mid + 10), 13, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        const D2D1_RECT_F track = D2D1::RectF(W - kPad - 106, mid - 3, W - kPad - 50, mid + 3);
        c.Fill(track, 3, p.track);
        if (pct > 0)
            c.Fill(D2D1::RectF(track.left, track.top, track.left + std::max(6.f, (track.right - track.left) * float(pct) / 100.f), track.bottom), 3,
                   colour(pct));
        c.Text(std::to_wstring(pct) + L"%", D2D1::RectF(W - kPad - 46, mid - 10, W - kPad, mid + 10), 13, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 2);
    }
}

// A note: its title, then the text; while editing, the caret, and the text
// scrolled up when the caret goes below the card.
void PaintNotes(Canvas& c, const WidgetContent& n, const Pal& p, float W, float H, std::vector<WidgetHit>& hits)
{
    c.Text(L"备忘录", D2D1::RectF(kPad + 2, kPad - 2, W - kPad, kPad + 18), 13.5f, p.orange, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const D2D1_RECT_F box = D2D1::RectF(kPad + 2, kPad + 24, W - kPad - 2, H - kPad);
    hits.push_back({D2D1::RectF(0, 0, W, H), L"text"});
    const float size = 13.5f;
    if (n.text.empty() && !n.editing) {
        c.Text(L"点按以记录…", box, size, p.faint, DWRITE_FONT_WEIGHT_NORMAL, 0, true);
        return;
    }
    if (!n.editing) {
        c.Text(n.text, box, size, p.text, DWRITE_FONT_WEIGHT_NORMAL, 0, true);
        return;
    }
    D2D1_RECT_F caret = c.Caret(n.text, box, size, DWRITE_FONT_WEIGHT_NORMAL, n.caret);
    float scroll = std::max(0.f, caret.bottom - box.bottom);
    c.Clip(box);
    c.Text(n.text, D2D1::RectF(box.left, box.top - scroll, box.right, box.top - scroll + 10000), size, p.text, DWRITE_FONT_WEIGHT_NORMAL, 0, true);
    caret.top -= scroll;
    caret.bottom -= scroll;
    if (n.caretOn)
        c.Fill(caret, 0.75f, p.orange);
    c.Unclip();
    hits.push_back({caret, L"caret"});
}

// Reminders: the title and the count still to do, then the list
// in their original order, each with its circle; a last row adds one.
void PaintReminders(Canvas& c, const WidgetContent& r, const Pal& p, float W, float H, std::vector<WidgetHit>& hits)
{
    int open = int(std::count_if(r.items.begin(), r.items.end(), [](const Reminder& i) { return !i.done; }));
    c.Text(L"提醒事项", D2D1::RectF(kPad + 2, kPad - 2, W - kPad - 40, kPad + 18), 13.5f, p.orange, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    c.Text(std::to_wstring(open), D2D1::RectF(W - kPad - 48, kPad - 6, W - kPad - 2, kPad + 22), 22, p.orange, DWRITE_FONT_WEIGHT_SEMI_BOLD, 2);
    const float bottom = H - kReminderBottom, view = bottom - kReminderTop;
    const float limit = std::max(0.f, float(r.items.size() + 1) * kReminderRow - view);
    const float scroll = std::clamp(r.scroll, 0.f, limit);
    auto hit = [&](D2D1_RECT_F rect, std::wstring id) {
        rect.top = std::max(rect.top, kReminderTop);
        rect.bottom = std::min(rect.bottom, bottom);
        if (rect.bottom > rect.top)
            hits.push_back({rect, std::move(id)});
    };
    c.Clip(D2D1::RectF(kPad - 4, kReminderTop, W - kPad, bottom));
    int first = int(scroll / kReminderRow);
    int last = std::min(int(r.items.size()), int(std::ceil((scroll + view) / kReminderRow)));
    for (int idx = first; idx < last; ++idx) {
        const Reminder& it = r.items[size_t(idx)];
        float y = kReminderTop + kReminderRow * float(idx) - scroll;
        float cx = kPad + 9, cy = y + kReminderRow / 2;
        if (it.done) {
            c.Circle(cx, cy, 8, p.orange);
            c.Circle(cx, cy, 3.2f, p.onAccent);
        } else {
            c.Arc(cx, cy, 7.4f, -90, 270, p.sub, 1.5f);
        }
        hit(D2D1::RectF(kPad - 4, y, kPad + 22, y + kReminderRow), L"toggle:" + std::to_wstring(idx));
        D2D1_RECT_F text = D2D1::RectF(kPad + 26, y, W - kPad, y + kReminderRow);
        bool editing = r.editing && r.item == idx;
        c.Text(it.text.empty() && !editing ? L"新提醒" : it.text, text, 13.5f, it.done ? p.sub : it.text.empty() ? p.faint : p.text);
        if (it.done && !it.text.empty())
            c.Line(text.left, cy, std::min(text.right, text.left + c.TextWidth(it.text, 13.5f)), cy, p.sub);
        if (editing) {
            // One line: the caret after the text up to it.
            float x = text.left + c.TextWidth(it.text.substr(0, std::min(r.caret, it.text.size())), 13.5f);
            D2D1_RECT_F caret = D2D1::RectF(std::min(x, text.right - 2), cy - 9, std::min(x, text.right - 2) + 1.5f, cy + 9);
            if (r.caretOn)
                c.Fill(caret, 0.75f, p.orange);
            hit(caret, L"caret");
        }
        hit(text, L"item:" + std::to_wstring(idx));
    }
    if (!r.editing) {
        float y = kReminderTop + kReminderRow * float(r.items.size()) - scroll;
        if (y < bottom) {
            c.Text(L"+", D2D1::RectF(kPad - 1, y, kPad + 19, y + kReminderRow), 17, p.faint, DWRITE_FONT_WEIGHT_NORMAL, 1);
            c.Text(r.items.empty() ? L"点按以添加提醒" : L"新提醒", D2D1::RectF(kPad + 26, y, W - kPad, y + kReminderRow), 13.5f, p.faint);
            hit(D2D1::RectF(kPad - 4, y, W - kPad, y + kReminderRow), L"add");
        }
    }
    c.Unclip();
    if (limit > 0) {
        float thumb = std::max(18.f, view * view / (view + limit));
        float y = kReminderTop + (view - thumb) * scroll / limit;
        c.Fill(D2D1::RectF(W - 7, y, W - 5, y + thumb), 1, p.faint);
    }
}

// A photo filling the card; without one, how to choose a folder.
void PaintPhotos(Canvas& c, const WidgetContent& ph, const Pal& p, float W, float H)
{
    if (ph.image && !ph.image->empty()) {
        c.Image(ph.imageKey, *ph.image, D2D1::RectF(0, 0, W, H)); // the widget masks its complete content once
        return;
    }
    c.Glyph(L"\uE91B", D2D1::RectF(0, H / 2 - 46, W, H / 2 - 6), 30, p.sub);
    c.Text(L"照片", D2D1::RectF(0, H / 2, W, H / 2 + 20), 14, p.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    c.Text(L"右键选择照片文件夹", D2D1::RectF(0, H / 2 + 20, W, H / 2 + 38), 11.5f, p.sub, DWRITE_FONT_WEIGHT_NORMAL, 1);
}

// ---- weather -------------------------------------------------------------------

// What the sky does, for the icon and the widget's tint.
enum class Sky { Clear, Partly, Cloudy, Fog, Rain, Snow, Storm };

Sky SkyOf(int code)
{
    if (code <= 1)
        return Sky::Clear;
    if (code == 2)
        return Sky::Partly;
    if (code == 3)
        return Sky::Cloudy;
    if (code == 45 || code == 48)
        return Sky::Fog;
    if ((code >= 71 && code <= 77) || code == 85 || code == 86)
        return Sky::Snow;
    if (code >= 95)
        return Sky::Storm;
    return Sky::Rain;
}

// The symbols' colours over each ink. White that is not the glass's ink stays white over bright
// glass (InkAt only switches the theme's own ink).
struct IconInk {
    D2D1_COLOR_F sun, moon, cloud, grey, rain, arrow;
};

IconInk IconColors(Ink ink)
{
    switch (ink) {
    case Ink::Dark:
        return {Rgba(0xFFD60A, 1), Rgba(0xF5C451, 1), Rgba(0xA4ADB8, 1), Rgba(0x7D8794, 1), Rgba(0x64D2FF, 1), Rgba(0x6E7681, 1)};
    case Ink::Mono:
        return {Rgba(0xFFFFFF, 0.72f), Rgba(0xFFFFFF, 0.9f), Rgba(0xFFFFFF, 0.96f), Rgba(0xFFFFFF, 0.8f), Rgba(0xFFFFFF, 0.85f), Rgba(0xFFFFFF, 0.96f)};
    default:
        return {Rgba(0xFFD60A, 1), Rgba(0xF2F2F7, 1), Rgba(0xF7F8FA, 1), Rgba(0xD1D5DB, 1), Rgba(0x64D2FF, 1), Rgba(0xF2F5F9, 1)};
    }
}

// A cloud filling (about) a box s wide, its middle at (cx, cy).
void Cloud(Canvas& c, float cx, float cy, float s, D2D1_COLOR_F ink)
{
    c.Fill(D2D1::RectF(cx - 0.42f * s, cy - 0.02f * s, cx + 0.42f * s, cy + 0.26f * s), 0.14f * s, ink);
    c.Circle(cx - 0.17f * s, cy + 0.02f * s, 0.19f * s, ink);
    c.Circle(cx + 0.08f * s, cy - 0.06f * s, 0.25f * s, ink);
    c.Circle(cx + 0.29f * s, cy + 0.09f * s, 0.15f * s, ink);
}

void Sun(Canvas& c, float cx, float cy, float s, D2D1_COLOR_F gold)
{
    constexpr float kDeg = 3.14159265f / 180.f;
    c.Circle(cx, cy, 0.2f * s, gold);
    for (int i = 0; i < 8; ++i) {
        float a = float(i) * 45.f * kDeg;
        c.Capsule(cx + std::cos(a) * 0.31f * s, cy + std::sin(a) * 0.31f * s, cx + std::cos(a) * 0.43f * s, cy + std::sin(a) * 0.43f * s, gold,
                  std::max(1.f, 0.075f * s));
    }
}

void Moon(Canvas& c, float cx, float cy, float s, D2D1_COLOR_F ink)
{
    c.Crescent(cx, cy, 0.3f * s, 0.17f * s, -0.12f * s, 0.25f * s, ink);
}

// macOS's weather symbols in their colours: a yellow sun, a pale moon, white clouds, blue rain
// (IconColors: grey clouds over bright glass, all white on the dimmed glass).
// event: 1 a sunrise, 2 a sunset (a half sun on the horizon, an arrow up or down).
void WeatherIcon(Canvas& c, int code, bool day, float cx, float cy, float s, int event, Ink ink)
{
    const IconInk k = IconColors(ink);
    if (event) {
        c.Clip(D2D1::RectF(cx - s, cy - s, cx + s, cy + 0.12f * s));
        Sun(c, cx, cy + 0.12f * s, s * 0.9f, k.sun);
        c.Unclip();
        c.Capsule(cx - 0.42f * s, cy + 0.2f * s, cx + 0.42f * s, cy + 0.2f * s, k.sun, std::max(1.f, 0.07f * s));
        const float y0 = cy - 0.46f * s, y1 = cy - 0.2f * s, w = std::max(1.f, 0.07f * s);
        c.Capsule(cx, y0, cx, y1, k.arrow, w);
        float tip = event == 1 ? y0 : y1, back = event == 1 ? y0 + 0.1f * s : y1 - 0.1f * s;
        c.Capsule(cx - 0.1f * s, back, cx, tip, k.arrow, w);
        c.Capsule(cx + 0.1f * s, back, cx, tip, k.arrow, w);
        return;
    }
    switch (SkyOf(code)) {
    case Sky::Clear:
        day ? Sun(c, cx, cy, s, k.sun) : Moon(c, cx, cy, s, k.moon);
        break;
    case Sky::Partly:
        day ? Sun(c, cx - 0.16f * s, cy - 0.14f * s, s * 0.78f, k.sun) : Moon(c, cx - 0.14f * s, cy - 0.14f * s, s * 0.8f, k.moon);
        Cloud(c, cx + 0.08f * s, cy + 0.06f * s, s * 0.82f, k.cloud);
        break;
    case Sky::Cloudy:
        Cloud(c, cx, cy - 0.04f * s, s, k.cloud);
        break;
    case Sky::Fog:
        for (int i = 0; i < 3; ++i) {
            float y = cy - 0.2f * s + float(i) * 0.2f * s, inset = i == 1 ? 0.f : 0.08f * s;
            c.Capsule(cx - 0.4f * s + inset, y, cx + 0.4f * s - inset, y, k.cloud, std::max(1.f, 0.09f * s));
        }
        break;
    case Sky::Rain:
    case Sky::Snow:
    case Sky::Storm: {
        const bool snow = SkyOf(code) == Sky::Snow, storm = SkyOf(code) == Sky::Storm;
        Cloud(c, cx, cy - 0.16f * s, s, storm ? k.grey : k.cloud);
        for (int i = 0; i < 3; ++i) {
            float x = cx - 0.2f * s + float(i) * 0.2f * s, y = cy + 0.24f * s;
            if (snow)
                c.Circle(x, y + (i % 2 ? 0.08f : 0.f) * s, 0.055f * s, k.cloud);
            else if (!storm || i != 1)
                c.Capsule(x + 0.03f * s, y - 0.04f * s, x - 0.04f * s, y + 0.12f * s, k.rain, std::max(1.f, 0.07f * s));
        }
        if (storm) {
            const D2D1_COLOR_F bolt = ink == Ink::Mono ? k.arrow : k.sun;
            const float w = std::max(1.f, 0.08f * s);
            c.Capsule(cx + 0.04f * s, cy + 0.08f * s, cx - 0.06f * s, cy + 0.26f * s, bolt, w);
            c.Capsule(cx - 0.06f * s, cy + 0.26f * s, cx + 0.05f * s, cy + 0.26f * s, bolt, w);
            c.Capsule(cx + 0.05f * s, cy + 0.26f * s, cx - 0.05f * s, cy + 0.44f * s, bolt, w);
        }
        break;
    }
    }
}

// The full-colour weather widget's sky, top and bottom: blue by day, navy by night, greyer
// under clouds, rain and fog (as macOS's Weather).
std::pair<uint32_t, uint32_t> SkyFace(const Weather& w)
{
    if (!w.ok)
        return {0x2F7BD6, 0x63A9EA};
    switch (SkyOf(w.code)) {
    case Sky::Clear:
    case Sky::Partly:
        return w.day ? std::pair<uint32_t, uint32_t>{0x2F7BD6, 0x63A9EA} : std::pair<uint32_t, uint32_t>{0x0C1631, 0x2A3E68};
    case Sky::Cloudy:
    case Sky::Fog:
        return w.day ? std::pair<uint32_t, uint32_t>{0x6A7E92, 0x98A8B8} : std::pair<uint32_t, uint32_t>{0x262F3C, 0x47525F};
    case Sky::Snow:
        return w.day ? std::pair<uint32_t, uint32_t>{0x7F95AC, 0xAFC0D0} : std::pair<uint32_t, uint32_t>{0x2B3646, 0x4C5868};
    default: // rain, storms
        return w.day ? std::pair<uint32_t, uint32_t>{0x4A5D70, 0x728699} : std::pair<uint32_t, uint32_t>{0x1D2530, 0x3A4552};
    }
}

std::wstring Degrees(float t) { return std::to_wstring(int(std::lround(t))) + L"°"; }

// A day's span of the week's temperatures: blue when cold, through green and yellow, orange when hot.
D2D1_COLOR_F TempColor(float t)
{
    struct Stop {
        float t;
        uint32_t rgb;
    };
    const Stop stops[] = {{-5, 0x5AC8FA}, {8, 0x63E6BE}, {18, 0xFFD60A}, {28, 0xFF9F0A}, {36, 0xFF453A}};
    if (t <= stops[0].t)
        return Rgba(stops[0].rgb, 1);
    for (size_t i = 1; i < std::size(stops); ++i)
        if (t <= stops[i].t) {
            float k = (t - stops[i - 1].t) / (stops[i].t - stops[i - 1].t);
            D2D1_COLOR_F a = Rgba(stops[i - 1].rgb, 1), b = Rgba(stops[i].rgb, 1);
            return D2D1::ColorF(a.r + (b.r - a.r) * k, a.g + (b.g - a.g) * k, a.b + (b.b - a.b) * k, 1);
        }
    return Rgba(stops[std::size(stops) - 1].rgb, 1);
}

// macOS's weather widget: the place, the temperature large, the sky and the day's high and low;
// the medium one adds the next hours, the large one the next days with their spans.
void PaintWeather(Canvas& c, WidgetSize size, const WidgetWorld& world, const Pal& p, float W, float H)
{
    const Weather& w = world.weather;
    const D2D1_COLOR_F text = p.text, sub = p.sub, faint = p.track;
    const Ink ink = p.icon;
    const bool dark = ink == Ink::Dark;
    const float pad = 16;
    const std::wstring city = w.city.empty() ? (world.city.empty() ? L"天气" : world.city) : w.city;
    c.Text(city, D2D1::RectF(pad, 12, W - pad - (size == WidgetSize::Small ? 0 : 120), 32), 14.5f, text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    if (!w.ok) {
        c.Text(w.error.empty() ? L"正在获取天气…" : w.error, D2D1::RectF(pad, H / 2 - 10, W - pad, H / 2 + 10), 13, sub,
               DWRITE_FONT_WEIGHT_SEMI_BOLD, size == WidgetSize::Small ? 0 : 1);
        return;
    }
    c.Text(Degrees(w.temp), D2D1::RectF(pad - 2, 30, W / 2 + 40, 82), 44, text, DWRITE_FONT_WEIGHT_LIGHT);
    const std::wstring range = L"最高 " + Degrees(w.hi) + L"  最低 " + Degrees(w.lo);
    if (size == WidgetSize::Small) {
        WeatherIcon(c, w.code, w.day, pad + 11, H - 62, 22, 0, ink);
        c.Text(WeatherCondition(w.code), D2D1::RectF(pad, H - 48, W - pad, H - 30), 13, text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        c.Text(range, D2D1::RectF(pad, H - 30, W - pad, H - 12), 12, text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        return;
    }
    // The sky and the day's span at the top right.
    WeatherIcon(c, w.code, w.day, W - pad - 10, 24, 20, 0, ink);
    c.Text(WeatherCondition(w.code), D2D1::RectF(W / 2, 40, W - pad, 58), 13, text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 2);
    c.Text(range, D2D1::RectF(W / 2 - 20, 58, W - pad, 76), 12.5f, text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 2);
    // The next hours.
    const float hy = size == WidgetSize::Large ? 92.f : 90.f, colW = (W - 2 * pad) / 6;
    if (size == WidgetSize::Large)
        c.Line(pad, hy - 6, W - pad, hy - 6, faint);
    for (size_t i = 0; i < w.hours.size() && i < 6; ++i) {
        const WeatherHour& h = w.hours[i];
        float x0 = pad + colW * float(i), cx = x0 + colW / 2;
        c.Text(h.label, D2D1::RectF(x0 - 4, hy, x0 + colW + 4, hy + 16), 11.5f, sub, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        WeatherIcon(c, h.code, h.day, cx, hy + 31, 22, h.event, ink);
        c.Text(h.event ? (h.event == 1 ? L"日出" : L"日落") : Degrees(h.temp), D2D1::RectF(x0 - 4, hy + 46, x0 + colW + 4, hy + 64),
               h.event ? 12.f : 13.5f, text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    }
    if (size != WidgetSize::Large)
        return;
    // The next days: each one's low, its span on the week's scale, its high.
    const float top = hy + 76;
    c.Line(pad, top - 6, W - pad, top - 6, faint);
    const size_t n = std::min<size_t>(w.days.size(), 5);
    float weekLo = 1e9f, weekHi = -1e9f;
    for (size_t i = 0; i < n; ++i)
        weekLo = std::min(weekLo, w.days[i].lo), weekHi = std::max(weekHi, w.days[i].hi);
    const float rowH = (H - 12 - top) / float(std::max<size_t>(n, 1));
    const float barL = 170, barR = W - pad - 44, span = std::max(1.f, weekHi - weekLo);
    for (size_t i = 0; i < n; ++i) {
        const WeatherDay& d = w.days[i];
        float y = top + rowH * float(i), cy = y + rowH / 2;
        c.Text(d.label, D2D1::RectF(pad, y, pad + 60, y + rowH), 14, text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        WeatherIcon(c, d.code, true, pad + 84, cy, 22, 0, ink);
        c.Text(Degrees(d.lo), D2D1::RectF(barL - 46, y, barL - 8, y + rowH), 14, sub, DWRITE_FONT_WEIGHT_SEMI_BOLD, 2);
        c.Fill(D2D1::RectF(barL, cy - 2.5f, barR, cy + 2.5f), 2.5f, faint);
        float x0 = barL + (barR - barL) * (d.lo - weekLo) / span, x1 = barL + (barR - barL) * (d.hi - weekLo) / span;
        x1 = std::max(x1, x0 + 5);
        if (ink == Ink::Mono)
            c.Fill(D2D1::RectF(x0, cy - 2.5f, x1, cy + 2.5f), 2.5f, text);
        else
            c.Gradient(D2D1::RectF(x0, cy - 2.5f, x1, cy + 2.5f), 2.5f, TempColor(d.lo), TempColor(d.hi), false);
        if (i == 0) {
            // Today: where it is now on the span.
            float x = barL + (barR - barL) * (std::clamp(w.temp, d.lo, d.hi) - weekLo) / span;
            c.Circle(x, cy, 4.5f, dark ? Rgba(0xFFFFFF, 1) : Rgba(0x1C2333, 0.55f));
            c.Circle(x, cy, 3.2f, dark ? Rgba(0x1C2333, 0.9f) : Rgba(0xFFFFFF, 1));
        }
        c.Text(Degrees(d.hi), D2D1::RectF(barR + 6, y, W - pad, y + rowH), 14, text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 2);
    }
}

// The material's face under the content and the ink over it. Faces cover the largest size: while
// a widget resizes its viewport outgrows the size it lays out.
Pal Face(Canvas& c, WidgetKind kind, WidgetMaterial material, const WidgetWorld& world, float H)
{
    if (material == WidgetMaterial::Glass)
        return GlassPal(c.theme);
    // One ink everywhere: the face, not the desktop, decides it.
    c.ink = {};
    const D2D1_RECT_F all = D2D1::RectF(0, 0, WidgetWidth(WidgetSize::Large), WidgetHeight(WidgetSize::Large));
    if (material == WidgetMaterial::Dusk) {
        c.theme = Theme::For(false);
        c.theme.halo = Rgba(0x000000, 0.5f); // a deeper shadow under the white keeps it legible over bright glass
        c.inkDark = 0;
        c.Fill(all, 0, Rgba(0x000000, 0.22f)); // a light veil: the glass stays clear
        return DuskPal();
    }
    c.halo = false; // an opaque face needs none
    if (kind == WidgetKind::Weather) {
        c.theme = Theme::For(false);
        c.inkDark = 0;
        auto [top, bottom] = SkyFace(world.weather);
        c.Fill(all, 0, Rgba(bottom, 1));
        c.Gradient(D2D1::RectF(0, 0, all.right, H), 0, Rgba(top, 1), Rgba(bottom, 1));
        Pal p = GlassPal(c.theme);
        p.icon = Ink::Light;
        return p;
    }
    c.theme = Theme::For(true);
    c.inkDark = 1;
    c.Fill(all, 0, Rgba(0xFFFFFF, 1));
    return PaperPal();
}

} // namespace

float ReminderScrollMax(const WidgetContent& content, WidgetSize size)
{
    return std::max(0.f, float(content.items.size() + 1) * kReminderRow - (WidgetHeight(size) - kReminderTop - kReminderBottom));
}

bool WidgetSupports(WidgetKind kind, WidgetSize size)
{
    switch (kind) {
    case WidgetKind::Battery:
        return size == WidgetSize::Small;
    case WidgetKind::Bluetooth:
        return size != WidgetSize::Large;
    case WidgetKind::Clock:
    case WidgetKind::Digital:
    case WidgetKind::System:
        return size != WidgetSize::Large;
    default:
        return true;
    }
}

void PaintWidget(Canvas& c, WidgetKind kind, WidgetSize size, WidgetMaterial material, const WidgetContent& content, const WidgetWorld& world,
                 std::vector<WidgetHit>& hits)
{
    const float W = WidgetWidth(size), H = WidgetHeight(size);
    const Pal p = Face(c, kind, material, world, H);
    hits.clear();
    switch (kind) {
    case WidgetKind::Clock:
        PaintClock(c, size, content, world, p, W, H);
        break;
    case WidgetKind::Digital:
        PaintDigital(c, size, content, world, p, W, H);
        break;
    case WidgetKind::Calendar:
        PaintCalendar(c, size, world, p, W, H);
        break;
    case WidgetKind::Battery:
        PaintBattery(c, world, p, W, H);
        break;
    case WidgetKind::System:
        PaintSystem(c, size, world, p, W, H);
        break;
    case WidgetKind::Notes:
        PaintNotes(c, content, p, W, H, hits);
        break;
    case WidgetKind::Reminders:
        PaintReminders(c, content, p, W, H, hits);
        break;
    case WidgetKind::Photos:
        PaintPhotos(c, content, p, W, H);
        break;
    case WidgetKind::Weather:
        PaintWeather(c, size, world, p, W, H);
        break;
    case WidgetKind::Bluetooth:
        PaintBluetooth(c, size, world, p, W, H);
        break;
    }
}

} // namespace atrium
