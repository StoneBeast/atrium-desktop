#include "settings.h"

#include "log.h"
#include "shell/logon_task.h"

#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <fstream>
#include <sstream>

namespace json = winrt::Windows::Data::Json;

namespace atrium {

void Settings::Clamp()
{
    auto c = [](double& v, double lo, double hi, double fallback) {
        v = v != v ? fallback : std::clamp(v, lo, hi); // NaN -> fallback
    };
    Settings d;
    c(transparency, 0, 1, d.transparency);
    c(cornerRadius, 0, 200, d.cornerRadius);
    c(blur, 0, 1, d.blur);
    c(refraction, 0, 60, d.refraction);
    c(edge, 1, 80, d.edge);
    c(dispersion, 0, 0.5, d.dispersion);
    c(highlight, 0, 1, d.highlight);
    c(shadow, 0, 1, d.shadow);
    c(width, 200, 4000, d.width);
    c(height, 32, 160, d.height);
    c(margin, 0, 400, d.margin);
    c(iconSize, 16, 64, d.iconSize);
    c(magnify, 1, 2, d.magnify);
    c(notifyTimeout, 3, 30, d.notifyTimeout);
    c(dockDisplayDelay, 100, 2000, d.dockDisplayDelay);
    tint &= 0xFFFFFF;
    if (textColor != L"light" && textColor != L"dark")
        textColor = L"auto";
    if (autoHide != L"never" && autoHide != L"always")
        autoHide = L"fullscreen";
    if (statusStyle != L"icons")
        statusStyle = L"ring";
    if (startSize != L"small" && startSize != L"medium" && startSize != L"large")
        startSize.clear();
    if (dockDisplayMode != L"follow" && dockDisplayMode != L"fixed" && dockDisplayMode != L"all")
        dockDisplayMode = L"primary";
    if (taskbarApps != L"primaryAndWindow" && taskbarApps != L"window")
        taskbarApps = L"all";
    if (notifyPlacement != L"topRight")
        notifyPlacement = L"dock";
    if (pins && pins->size() > 64)
        pins->resize(64);
    if (preset != L"liquid" && preset != L"frosted")
        preset = L"custom";
}

std::wstring Settings::ToJson() const
{
    json::JsonObject o;
    auto num = [&](const wchar_t* k, double v) { o.SetNamedValue(k, json::JsonValue::CreateNumberValue(v)); };
    wchar_t tintText[8];
    swprintf_s(tintText, L"#%06X", tint);
    num(L"version", kVersion);
    o.SetNamedValue(L"preset", json::JsonValue::CreateStringValue(preset));
    num(L"transparency", transparency);
    num(L"cornerRadius", cornerRadius);
    num(L"blur", blur);
    num(L"refraction", refraction);
    num(L"edge", edge);
    num(L"dispersion", dispersion);
    num(L"highlight", highlight);
    num(L"shadow", shadow);
    o.SetNamedValue(L"tint", json::JsonValue::CreateStringValue(tintText));
    num(L"width", width);
    num(L"height", height);
    num(L"margin", margin);
    num(L"iconSize", iconSize);
    num(L"magnify", magnify);
    o.SetNamedValue(L"hoverRise", json::JsonValue::CreateBooleanValue(hoverRise));
    o.SetNamedValue(L"startTray", json::JsonValue::CreateBooleanValue(startTray));
    o.SetNamedValue(L"startStatus", json::JsonValue::CreateBooleanValue(startStatus));
    if (!startSize.empty())
        o.SetNamedValue(L"startSize", json::JsonValue::CreateStringValue(startSize));
    o.SetNamedValue(L"showClock", json::JsonValue::CreateBooleanValue(showClock));
    o.SetNamedValue(L"showTray", json::JsonValue::CreateBooleanValue(showTray));
    o.SetNamedValue(L"showStatus", json::JsonValue::CreateBooleanValue(showStatus));
    o.SetNamedValue(L"showNotifications", json::JsonValue::CreateBooleanValue(showNotifications));
    o.SetNamedValue(L"autoWidth", json::JsonValue::CreateBooleanValue(autoWidth));
    o.SetNamedValue(L"showRunning", json::JsonValue::CreateBooleanValue(showRunning));
    o.SetNamedValue(L"textColor", json::JsonValue::CreateStringValue(textColor));
    o.SetNamedValue(L"autoHide", json::JsonValue::CreateStringValue(autoHide));
    o.SetNamedValue(L"statusStyle", json::JsonValue::CreateStringValue(statusStyle));
    o.SetNamedValue(L"dockDisplayMode", json::JsonValue::CreateStringValue(dockDisplayMode));
    num(L"dockDisplayDelay", dockDisplayDelay);
    o.SetNamedValue(L"dockDisplay", json::JsonValue::CreateStringValue(dockDisplay));
    o.SetNamedValue(L"desktopDisplay", json::JsonValue::CreateStringValue(desktopDisplay));
    o.SetNamedValue(L"taskbarApps", json::JsonValue::CreateStringValue(taskbarApps));
    o.SetNamedValue(L"taskbarRightPrimaryOnly", json::JsonValue::CreateBooleanValue(taskbarRightPrimaryOnly));
    num(L"notifyTimeout", notifyTimeout);
    o.SetNamedValue(L"notifyPlacement", json::JsonValue::CreateStringValue(notifyPlacement));
    o.SetNamedValue(L"notifyHideBody", json::JsonValue::CreateBooleanValue(notifyHideBody));
    o.SetNamedValue(L"notifyListener", json::JsonValue::CreateBooleanValue(notifyListener));
    o.SetNamedValue(L"notifyWeChat", json::JsonValue::CreateBooleanValue(notifyWeChat));
    o.SetNamedValue(L"notifyFeishu", json::JsonValue::CreateBooleanValue(notifyFeishu));
    o.SetNamedValue(L"bar", json::JsonValue::CreateBooleanValue(bar));
    o.SetNamedValue(L"widgets", json::JsonValue::CreateBooleanValue(widgets));
    o.SetNamedValue(L"widgetsOnRight", json::JsonValue::CreateBooleanValue(widgetsOnRight));
    o.SetNamedValue(L"widgetBackdrop", json::JsonValue::CreateStringValue(widgetBackdrop));
    o.SetNamedValue(L"widgetMaterial", json::JsonValue::CreateStringValue(widgetMaterial));
    o.SetNamedValue(L"stageManager", json::JsonValue::CreateBooleanValue(stageManager));
    o.SetNamedValue(L"stageOnRight", json::JsonValue::CreateBooleanValue(stageOnRight));
    o.SetNamedValue(L"stageAutoHide", json::JsonValue::CreateBooleanValue(stageAutoHide));
    o.SetNamedValue(L"stageMinimize", json::JsonValue::CreateBooleanValue(stageMinimize));
    o.SetNamedValue(L"hideTaskbar", json::JsonValue::CreateBooleanValue(hideTaskbar));
    o.SetNamedValue(L"hideBanners", json::JsonValue::CreateBooleanValue(hideBanners));
    o.SetNamedValue(L"winKeyStart", json::JsonValue::CreateBooleanValue(winKeyStart));
    o.SetNamedValue(L"winNumbers", json::JsonValue::CreateBooleanValue(winNumbers));
    o.SetNamedValue(L"startOnLogin", json::JsonValue::CreateBooleanValue(startOnLogin));
    if (pins) {
        json::JsonArray a;
        for (const auto& p : *pins) {
            json::JsonObject e;
            e.SetNamedValue(L"name", json::JsonValue::CreateStringValue(p.name));
            e.SetNamedValue(L"path", json::JsonValue::CreateStringValue(p.path));
            e.SetNamedValue(L"args", json::JsonValue::CreateStringValue(p.args));
            e.SetNamedValue(L"aumid", json::JsonValue::CreateStringValue(p.aumid));
            if (p.multipleWindows)
                e.SetNamedValue(L"multipleWindows", json::JsonValue::CreateBooleanValue(*p.multipleWindows));
            a.Append(e);
        }
        o.SetNamedValue(L"pins", a);
    }
    return std::wstring(o.Stringify());
}

bool Settings::FromJson(const std::wstring& text, const Settings& base, Settings& out)
{
    json::JsonObject o{nullptr};
    if (!json::JsonObject::TryParse(text, o))
        return false;
    Settings s = base;
    auto num = [&](const wchar_t* k, double& v) {
        if (o.HasKey(k) && o.GetNamedValue(k).ValueType() == json::JsonValueType::Number)
            v = o.GetNamedNumber(k);
    };
    if (o.HasKey(L"preset") && o.GetNamedValue(L"preset").ValueType() == json::JsonValueType::String)
        s.preset = std::wstring(o.GetNamedString(L"preset"));
    num(L"transparency", s.transparency);
    num(L"cornerRadius", s.cornerRadius);
    num(L"blur", s.blur);
    num(L"refraction", s.refraction);
    num(L"edge", s.edge);
    num(L"dispersion", s.dispersion);
    num(L"highlight", s.highlight);
    num(L"shadow", s.shadow);
    num(L"width", s.width);
    num(L"height", s.height);
    num(L"margin", s.margin);
    if (o.HasKey(L"tint") && o.GetNamedValue(L"tint").ValueType() == json::JsonValueType::String) {
        std::wstring t(o.GetNamedString(L"tint"));
        if (t.size() == 7 && t[0] == L'#' && std::all_of(t.begin() + 1, t.end(), [](wchar_t c) { return iswxdigit(c) != 0; }))
            s.tint = static_cast<uint32_t>(wcstoul(t.c_str() + 1, nullptr, 16));
    }
    num(L"iconSize", s.iconSize);
    num(L"magnify", s.magnify);
    auto flag = [&](const wchar_t* k, bool& v) {
        if (o.HasKey(k) && o.GetNamedValue(k).ValueType() == json::JsonValueType::Boolean)
            v = o.GetNamedBoolean(k);
    };
    flag(L"hoverRise", s.hoverRise);
    flag(L"startTray", s.startTray);
    flag(L"startStatus", s.startStatus);
    if (o.HasKey(L"startSize") && o.GetNamedValue(L"startSize").ValueType() == json::JsonValueType::String)
        s.startSize = std::wstring(o.GetNamedString(L"startSize"));
    flag(L"showClock", s.showClock);
    flag(L"showTray", s.showTray);
    flag(L"showStatus", s.showStatus);
    flag(L"showNotifications", s.showNotifications);
    flag(L"autoWidth", s.autoWidth);
    flag(L"showRunning", s.showRunning);
    num(L"notifyTimeout", s.notifyTimeout);
    flag(L"notifyHideBody", s.notifyHideBody);
    // Version 1 saved the old default (off) along with everything else; from
    // version 2 the notifications are on unless turned off since.
    bool v1 = o.HasKey(L"version") && o.GetNamedValue(L"version").ValueType() == json::JsonValueType::Number && o.GetNamedNumber(L"version") < 2;
    if (!v1)
        flag(L"notifyListener", s.notifyListener);
    flag(L"notifyWeChat", s.notifyWeChat);
    flag(L"notifyFeishu", s.notifyFeishu);
    flag(L"bar", s.bar);
    flag(L"widgets", s.widgets);
    flag(L"widgetsOnRight", s.widgetsOnRight);
    flag(L"stageManager", s.stageManager);
    flag(L"stageOnRight", s.stageOnRight);
    flag(L"stageAutoHide", s.stageAutoHide);
    flag(L"stageMinimize", s.stageMinimize);
    flag(L"hideTaskbar", s.hideTaskbar);
    flag(L"hideBanners", s.hideBanners);
    flag(L"winKeyStart", s.winKeyStart);
    flag(L"winNumbers", s.winNumbers);
    flag(L"startOnLogin", s.startOnLogin);
    if (o.HasKey(L"widgetBackdrop") && o.GetNamedValue(L"widgetBackdrop").ValueType() == json::JsonValueType::String)
        s.widgetBackdrop = std::wstring(o.GetNamedString(L"widgetBackdrop")) == L"wallpaper" ? L"wallpaper" : L"desktop";
    if (o.HasKey(L"widgetMaterial") && o.GetNamedValue(L"widgetMaterial").ValueType() == json::JsonValueType::String) {
        const std::wstring m(o.GetNamedString(L"widgetMaterial"));
        s.widgetMaterial = m == L"dusk" || m == L"color" ? m : L"glass";
    }
    if (o.HasKey(L"textColor") && o.GetNamedValue(L"textColor").ValueType() == json::JsonValueType::String)
        s.textColor = std::wstring(o.GetNamedString(L"textColor"));
    if (o.HasKey(L"autoHide") && o.GetNamedValue(L"autoHide").ValueType() == json::JsonValueType::String)
        s.autoHide = std::wstring(o.GetNamedString(L"autoHide"));
    if (o.HasKey(L"statusStyle") && o.GetNamedValue(L"statusStyle").ValueType() == json::JsonValueType::String)
        s.statusStyle = std::wstring(o.GetNamedString(L"statusStyle"));
    auto display = [&](const wchar_t* key, std::wstring& value) {
        if (o.HasKey(key) && o.GetNamedValue(key).ValueType() == json::JsonValueType::String)
            value = std::wstring(o.GetNamedString(key));
    };
    display(L"dockDisplayMode", s.dockDisplayMode);
    num(L"dockDisplayDelay", s.dockDisplayDelay);
    display(L"dockDisplay", s.dockDisplay);
    display(L"desktopDisplay", s.desktopDisplay);
    display(L"taskbarApps", s.taskbarApps);
    flag(L"taskbarRightPrimaryOnly", s.taskbarRightPrimaryOnly);
    display(L"notifyPlacement", s.notifyPlacement);
    if (o.HasKey(L"pins") && o.GetNamedValue(L"pins").ValueType() == json::JsonValueType::Array) {
        std::vector<PinnedApp> pins;
        for (const auto& v : o.GetNamedArray(L"pins")) {
            if (v.ValueType() != json::JsonValueType::Object)
                continue;
            auto e = v.GetObject();
            auto str = [&](const wchar_t* k) {
                return e.HasKey(k) && e.GetNamedValue(k).ValueType() == json::JsonValueType::String ? std::wstring(e.GetNamedString(k))
                                                                                                    : std::wstring();
            };
            PinnedApp p{str(L"name"), str(L"path"), str(L"args"), str(L"aumid")};
            if (e.HasKey(L"multipleWindows") && e.GetNamedValue(L"multipleWindows").ValueType() == json::JsonValueType::Boolean)
                p.multipleWindows = e.GetNamedBoolean(L"multipleWindows");
            if (!p.path.empty() || !p.aumid.empty())
                pins.push_back(std::move(p));
        }
        s.pins = std::move(pins);
    }
    s.Clamp();
    out = s;
    return true;
}

std::wstring SettingsPath()
{
    PWSTR base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base)))
        dir = std::wstring(base) + L"\\Atrium";
    CoTaskMemFree(base);
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\settings.json";
}

std::wstring ReadTextFile(const std::wstring& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string bytes = ss.str();
    if (bytes.size() >= 3 && bytes.compare(0, 3, "\xEF\xBB\xBF") == 0)
        bytes.erase(0, 3); // UTF-8 BOM, e.g. saved by Notepad or Windows PowerShell
    if (bytes.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), int(bytes.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), int(bytes.size()), w.data(), n);
    return w;
}

bool WriteTextFile(const std::wstring& path, const std::wstring& text)
{
    std::wstring tmp = path + L".tmp";
    int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string bytes(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), bytes.data(), n, nullptr, nullptr);
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << bytes;
        out.flush();
        if (!out)
            return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

Settings LoadSettings()
{
    Settings s;
    std::wstring text = ReadTextFile(SettingsPath());
    if (text.empty())
        return s;
    if (!Settings::FromJson(text, Settings{}, s))
        Log(L"settings: %s is not valid JSON, using defaults", SettingsPath().c_str());
    return s;
}

bool SetStartOnLogin(bool enabled)
{
    if (IsDevelopmentBuild())
        return true;
    constexpr auto key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    wchar_t exe[32768]{};
    DWORD length = GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)));
    if (!length || length == std::size(exe))
        return false;
    std::wstring command = L"\"" + std::wstring(exe) + L"\"";
    wchar_t current[32771]{};
    DWORD size = sizeof(current);
    LSTATUS read = RegGetValueW(HKEY_CURRENT_USER, key, L"Atrium", RRF_RT_REG_SZ, nullptr, current, &size);
    const bool runEntry = read == ERROR_SUCCESS;
    const std::wstring task = LogonTaskProgram();
    if (!enabled) {
        // Another distribution's entries are not ours to remove.
        if ((read != ERROR_SUCCESS && read != ERROR_FILE_NOT_FOUND && read != ERROR_PATH_NOT_FOUND) ||
            (runEntry && _wcsicmp(command.c_str(), current) != 0) || (!task.empty() && _wcsicmp(task.c_str(), exe) != 0))
            return false;
        if (!task.empty() && !RemoveLogonTask())
            return false;
        if (!runEntry)
            return true;
        LSTATUS result = RegDeleteKeyValueW(HKEY_CURRENT_USER, key, L"Atrium");
        return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
    }
    // The logon task, and no Run entry left to start Atrium a second time (also from
    // earlier versions, which started from the Run key).
    if ((_wcsicmp(task.c_str(), exe) == 0 || RegisterLogonTask(exe)) && (!runEntry || RegDeleteKeyValueW(HKEY_CURRENT_USER, key, L"Atrium") == ERROR_SUCCESS))
        return true;
    if (read == ERROR_SUCCESS && command == current)
        return true;
    return RegSetKeyValueW(HKEY_CURRENT_USER, key, L"Atrium", REG_SZ, command.c_str(), DWORD((command.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool StartupTurnedOff()
{
    // The first byte is odd when the switch is off (2 on, 3 off).
    BYTE state[12]{};
    DWORD size = sizeof(state);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run", L"Atrium",
                        RRF_RT_REG_BINARY, nullptr, state, &size) == ERROR_SUCCESS &&
           size > 0 && (state[0] & 1);
}

bool IsDevelopmentBuild()
{
#ifdef ATRIUM_DEVELOPMENT
    return true;
#else
    static const bool development = [] {
        int count = 0;
        auto args = CommandLineToArgvW(GetCommandLineW(), &count);
        bool explicitDevelopment = false;
        for (int i = 1; args && i < count; ++i)
            explicitDevelopment |= wcscmp(args[i], L"--development") == 0;
        LocalFree(args);
        wchar_t exe[32768]{};
        GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)));
        std::wstring path(exe);
        path = path.substr(0, path.find_last_of(L'\\')) + L"\\development.txt";
        return explicitDevelopment || GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    }();
    return development;
#endif
}

bool SaveSettings(const Settings& s, std::wstring* warning)
{
    if (warning) warning->clear();
    const bool startupChanged = LoadSettings().startOnLogin != s.startOnLogin;
    if (!WriteTextFile(SettingsPath(), s.ToJson()))
        return false;
    if (startupChanged && !SetStartOnLogin(s.startOnLogin)) {
        Log(L"settings: settings saved, but Windows startup could not be changed");
        if (warning) *warning = L"设置已保存，但未能修改 Windows 开机启动。请重新切换开机启动后再保存。";
    }
    return true;
}

} // namespace atrium
