#include "shell/app_catalog.h"

#include "log.h"
#include "shell/start_layout.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <knownfolders.h>
#include <propsys.h>
#include <propkey.h>

#include <winrt/base.h>

#include <algorithm>
#include <cwctype>
#include <string_view>

namespace atrium {

namespace {

std::wstring Fold(const std::wstring& s)
{
    std::wstring out(s);
    if (!out.empty())
        LCMapStringEx(LOCALE_NAME_USER_DEFAULT, LCMAP_LOWERCASE, s.c_str(), int(s.size()), out.data(), int(out.size()), nullptr,
                      nullptr, 0);
    return out;
}

std::wstring PropString(IShellItem2* item, const PROPERTYKEY& key)
{
    PWSTR v = nullptr;
    std::wstring s;
    if (SUCCEEDED(item->GetString(key, &v)) && v)
        s = v;
    CoTaskMemFree(v);
    return s;
}

std::vector<CatalogApp> Enumerate()
{
    std::vector<CatalogApp> apps;
    winrt::com_ptr<IShellItem> folder;
    if (FAILED(SHGetKnownFolderItem(FOLDERID_AppsFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(folder.put()))))
        return apps;
    winrt::com_ptr<IEnumShellItems> items;
    if (FAILED(folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(items.put()))))
        return apps;
    for (;;) {
        winrt::com_ptr<IShellItem> item;
        if (items->Next(1, item.put(), nullptr) != S_OK)
            break;
        CatalogApp app;
        PWSTR s = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &s)) && s)
            app.name = s;
        CoTaskMemFree(s);
        s = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &s)) && s)
            app.id = s;
        CoTaskMemFree(s);
        if (auto item2 = item.try_as<IShellItem2>()) {
            app.aumid = PropString(item2.get(), PKEY_AppUserModel_ID);
            app.target = PropString(item2.get(), PKEY_Link_TargetParsingPath);
            app.args = PropString(item2.get(), PKEY_Link_Arguments);
        }
        if (!app.name.empty() && !app.id.empty())
            apps.push_back(std::move(app));
    }
    std::sort(apps.begin(), apps.end(), [](const CatalogApp& a, const CatalogApp& b) {
        return CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS, a.name.c_str(), int(a.name.size()), b.name.c_str(),
                               int(b.name.size()), nullptr, nullptr, 0) == CSTR_LESS_THAN;
    });
    return apps;
}

bool IsPath(const std::wstring& id) { return id.size() > 2 && id[1] == L':' && (id[2] == L'\\' || id[2] == L'/'); }

bool SameText(const std::wstring& a, const std::wstring& b)
{
    return !a.empty() && CompareStringOrdinal(a.c_str(), int(a.size()), b.c_str(), int(b.size()), TRUE) == CSTR_EQUAL;
}

std::wstring ReadUtf8File(const std::wstring& path)
{
    std::wstring text;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return text;
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    std::string bytes(size_t(std::min<LONGLONG>(size.QuadPart, 4 << 20)), '\0');
    DWORD read = 0;
    if (!bytes.empty() && ReadFile(f, bytes.data(), DWORD(bytes.size()), &read, nullptr)) {
        bytes.resize(read);
        size_t skip = bytes.rfind("\xEF\xBB\xBF", 0) == 0 ? 3 : 0;
        int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data() + skip, int(bytes.size() - skip), nullptr, 0);
        text.resize(size_t(std::max(n, 0)));
        MultiByteToWideChar(CP_UTF8, 0, bytes.data() + skip, int(bytes.size() - skip), text.data(), n);
    }
    CloseHandle(f);
    return text;
}

// The Windows start menu's layout file (start2.bin) is encrypted; the
// supported way to read it is Export-StartLayout, which writes JSON:
// {"pinnedList":[{"desktopAppLink":"%APPDATA%\\...lnk"},{"packagedAppId":"...!App"},...]}.
std::wstring ExportStartLayout()
{
    wchar_t temp[MAX_PATH] = {}, sys[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring file = std::wstring(temp) + L"atrium-start-layout-" + std::to_wstring(GetCurrentProcessId()) + L".json";
    std::wstring quoted; // a PowerShell single-quoted string
    for (wchar_t ch : file)
        quoted += ch == L'\'' ? L"''" : std::wstring(1, ch);
    std::wstring cmd = L"\"" + std::wstring(sys) + L"\\WindowsPowerShell\\v1.0\\powershell.exe\" -NoProfile -NonInteractive -ExecutionPolicy Bypass "
                       L"-Command \"Export-StartLayout -Path '" + quoted + L"'\"";
    DeleteFileW(file.c_str());
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        Log(L"catalog: Export-StartLayout could not start (%lu)", GetLastError());
        return {};
    }
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, 20000) != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
        Log(L"catalog: Export-StartLayout timed out");
    } else {
        GetExitCodeProcess(pi.hProcess, &code);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    std::wstring text;
    if (code == 0)
        text = ReadUtf8File(file);
    else
        Log(L"catalog: Export-StartLayout failed (exit code %lu)", code);
    DeleteFileW(file.c_str());
    return text;
}

// Every "key": "string" pair of a JSON text, in order (all the layout needs).
std::vector<std::pair<std::wstring, std::wstring>> JsonStringPairs(const std::wstring& j)
{
    std::vector<std::pair<std::wstring, std::wstring>> out;
    auto readString = [&](size_t& i) {
        std::wstring s;
        for (++i; i < j.size() && j[i] != L'"'; ++i) {
            if (j[i] != L'\\' || i + 1 >= j.size()) {
                s += j[i];
                continue;
            }
            wchar_t e = j[++i];
            if (e == L'u' && i + 4 < j.size()) {
                s += wchar_t(std::wcstoul(j.substr(i + 1, 4).c_str(), nullptr, 16));
                i += 4;
            } else {
                s += e == L'n' ? L'\n' : e == L't' ? L'\t' : e == L'r' ? L'\r' : e == L'b' ? L'\b' : e == L'f' ? L'\f' : e;
            }
        }
        ++i; // the closing quote
        return s;
    };
    auto skipSpace = [&](size_t& i) {
        while (i < j.size() && iswspace(j[i]))
            ++i;
    };
    for (size_t i = 0; i < j.size();) {
        if (j[i] != L'"') {
            ++i;
            continue;
        }
        std::wstring key = readString(i);
        skipSpace(i);
        if (i >= j.size() || j[i] != L':')
            continue;
        ++i;
        skipSpace(i);
        if (i < j.size() && j[i] == L'"')
            out.emplace_back(key, readString(i));
    }
    return out;
}

// %ALLUSERSPROFILE%, %APPDATA% and the like, from the shell's known folders:
// the process environment may lack them (Atrium started from a service or a
// sandbox), and then those pins would seem to point nowhere.
std::wstring ExpandFolders(const std::wstring& value)
{
    static const std::pair<const wchar_t*, const KNOWNFOLDERID*> kFolders[] = {
        {L"%ALLUSERSPROFILE%", &FOLDERID_ProgramData}, {L"%PROGRAMDATA%", &FOLDERID_ProgramData},
        {L"%APPDATA%", &FOLDERID_RoamingAppData},      {L"%LOCALAPPDATA%", &FOLDERID_LocalAppData},
        {L"%USERPROFILE%", &FOLDERID_Profile},         {L"%PUBLIC%", &FOLDERID_Public},
        {L"%PROGRAMFILES%", &FOLDERID_ProgramFiles},   {L"%PROGRAMFILES(X86)%", &FOLDERID_ProgramFilesX86},
        {L"%SYSTEMROOT%", &FOLDERID_Windows},          {L"%WINDIR%", &FOLDERID_Windows}};
    std::wstring out = value;
    for (const auto& [token, id] : kFolders) {
        size_t len = wcslen(token);
        if (out.size() < len || CompareStringOrdinal(out.c_str(), int(len), token, int(len), TRUE) != CSTR_EQUAL)
            continue;
        PWSTR path = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(*id, KF_FLAG_DEFAULT, nullptr, &path)))
            out = std::wstring(path) + out.substr(len);
        CoTaskMemFree(path);
        break;
    }
    wchar_t expanded[MAX_PATH * 2] = {};
    if (out.find(L'%') != std::wstring::npos && ExpandEnvironmentStringsW(out.c_str(), expanded, DWORD(std::size(expanded))))
        out = expanded;
    return out;
}

// The layout's entries as apps. A pinned shortcut is the catalog's entry only
// when that is the same app (its AUMID, or the same target under the same
// name); otherwise the shortcut itself, which keeps its arguments and keeps
// two shortcuts to one program apart.
std::vector<CatalogApp> ResolvePins(const std::vector<std::pair<std::wstring, std::wstring>>& layout, const std::vector<CatalogApp>& apps)
{
    std::vector<CatalogApp> out;
    auto add = [&](const CatalogApp& a) {
        if (std::none_of(out.begin(), out.end(), [&](const CatalogApp& o) { return SameText(o.id, a.id); }))
            out.push_back(a);
    };
    for (const auto& [key, value] : layout) {
        if (key == L"packagedAppId") {
            auto it = std::find_if(apps.begin(), apps.end(), [&](const CatalogApp& a) { return SameText(a.aumid, value) || SameText(a.id, value); });
            if (it != apps.end())
                add(*it);
            else
                Log(L"catalog: start pin %s is not installed", value.c_str());
            continue;
        }
        if (key != L"desktopAppLink")
            continue;
        std::wstring expanded = ExpandFolders(value);
        // Start layouts exported before the rename still refer to this shortcut.
        constexpr std::wstring_view legacy = L"\\Atrium.Core.lnk";
        if (GetFileAttributesW(expanded.c_str()) == INVALID_FILE_ATTRIBUTES && expanded.size() >= legacy.size() &&
            SameText(expanded.substr(expanded.size() - legacy.size()), std::wstring(legacy)))
            expanded.replace(expanded.size() - legacy.size(), legacy.size(), L"\\Atrium.lnk");
        if (GetFileAttributesW(expanded.c_str()) == INVALID_FILE_ATTRIBUTES) {
            Log(L"catalog: start pin %s not found", value.c_str());
            continue;
        }
        CatalogApp link;
        link.id = expanded;
        winrt::com_ptr<IShellItem2> item;
        if (SUCCEEDED(SHCreateItemFromParsingName(expanded.c_str(), nullptr, IID_PPV_ARGS(item.put())))) {
            PWSTR s = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &s)) && s)
                link.name = s; // localised ("File Explorer.lnk" shows as 文件资源管理器)
            CoTaskMemFree(s);
            link.aumid = PropString(item.get(), PKEY_AppUserModel_ID);
            link.target = PropString(item.get(), PKEY_Link_TargetParsingPath);
            link.args = PropString(item.get(), PKEY_Link_Arguments);
        }
        if (link.name.empty()) {
            std::wstring file = link.id.substr(link.id.find_last_of(L"\\/") + 1);
            link.name = file.size() > 4 ? file.substr(0, file.size() - 4) : file;
        }
        auto it = std::find_if(apps.begin(), apps.end(), [&](const CatalogApp& a) { return SameText(a.aumid, link.aumid) || SameText(a.id, link.aumid); });
        if (it == apps.end())
            it = std::find_if(apps.begin(), apps.end(), [&](const CatalogApp& a) { return SameText(a.target, link.target) && SameText(a.name, link.name); });
        add(it != apps.end() ? *it : link);
    }
    return out;
}

} // namespace

AppCatalog::AppCatalog(HWND notify, UINT readyMsg) : notify_(notify), msg_(readyMsg)
{
    refresh_ = true;
    thread_ = std::thread([this] { Worker(); });
}

AppCatalog::~AppCatalog()
{
    {
        std::lock_guard g(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

void AppCatalog::Refresh()
{
    {
        std::lock_guard g(mutex_);
        refresh_ = true;
    }
    cv_.notify_all();
}

bool AppCatalog::Loaded() const
{
    std::lock_guard g(mutex_);
    return loaded_;
}

std::vector<CatalogApp> AppCatalog::Apps() const
{
    std::lock_guard g(mutex_);
    return apps_;
}

std::vector<CatalogApp> AppCatalog::StartPins() const
{
    std::lock_guard g(mutex_);
    return startPins_;
}

void AppCatalog::PrefetchStartIcons(int px)
{
    std::lock_guard g(mutex_);
    prefetchPx_ = std::clamp(px, 8, 512);
    QueuePrefetch();
}

void AppCatalog::SetStartIds(std::vector<std::wstring> ids)
{
    std::lock_guard g(mutex_);
    startIds_ = std::set<std::wstring>(ids.begin(), ids.end());
    QueuePrefetch();
}

void AppCatalog::QueuePrefetch()
{
    if (prefetchPx_ <= 0)
        return;
    auto queue = [&](const std::wstring& id) {
        auto key = std::make_pair(id, prefetchPx_);
        if (!icons_.contains(key)) QueueIcon(key, true);
    };
    for (const CatalogApp& app : startPins_)
        queue(app.id);
    for (const std::wstring& id : startIds_)
        queue(id);
    cv_.notify_all();
}

IconImage AppCatalog::Icon(const std::wstring& id, int px)
{
    px = std::clamp(px, 8, 512);
    std::lock_guard g(mutex_);
    auto key = std::make_pair(id, px);
    if (auto it = icons_.find(key); it != icons_.end()) {
        it->second.used = ++iconUse_;
        return it->second.image;
    }
    if (QueueIcon(key, false)) cv_.notify_one();
    return {};
}

bool AppCatalog::QueueIcon(const std::pair<std::wstring, int>& key, bool prefetch)
{
    if (requested_.contains(key)) return false;
    if (auto it = failed_.find(key); it != failed_.end() &&
        (it->second.attempts >= 3 || GetTickCount64() - it->second.time < 2000)) return false;
    requested_.insert(key);
    if (prefetch) iconJobs_.push_front(key); else iconJobs_.push_back(key);
    return true;
}

void AppCatalog::SetVisibleIcons(std::vector<std::pair<std::wstring, int>> icons)
{
    std::lock_guard g(mutex_);
    visibleIcons_.clear();
    visibleIcons_.insert(icons.begin(), icons.end());
    PruneIcons();
}

void AppCatalog::PruneIcons()
{
    auto protectedIcon = [&](const auto& key) {
        return visibleIcons_.contains(key) || requested_.contains(key) ||
            (key.second == prefetchPx_ && (startIds_.contains(key.first) ||
                                           std::any_of(startPins_.begin(), startPins_.end(), [&](const auto& app) { return app.id == key.first; })));
    };
    std::erase_if(icons_, [&](const auto& entry) { return loaded_ && !liveIds_.contains(entry.first.first) && !protectedIcon(entry.first); });
    std::erase_if(failed_, [&](const auto& entry) { return loaded_ && !liveIds_.contains(entry.first.first) && !protectedIcon(entry.first); });
    size_t bytes = 0;
    for (const auto& [key, icon] : icons_) bytes += icon.image.bgra.size();
    while (bytes > 16 * 1024 * 1024 || icons_.size() > 512) {
        auto victim = icons_.end();
        for (auto it = icons_.begin(); it != icons_.end(); ++it)
            if (!protectedIcon(it->first) && (victim == icons_.end() || it->second.used < victim->second.used)) victim = it;
        if (victim == icons_.end()) break;
        bytes -= victim->second.image.bgra.size();
        icons_.erase(victim);
    }
    while (failed_.size() > 512) {
        auto victim = std::min_element(failed_.begin(), failed_.end(), [](const auto& a, const auto& b) { return a.second.time < b.second.time; });
        failed_.erase(victim);
    }
}

void AppCatalog::Worker()
{
    // STA: shell item image factories of some handlers need it.
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    for (;;) {
        bool refresh = false;
        std::pair<std::wstring, int> job;
        {
            std::unique_lock g(mutex_);
            cv_.wait(g, [&] { return stop_ || refresh_ || !iconJobs_.empty(); });
            if (stop_)
                break;
            if (!iconJobs_.empty()) {
                // Icons before a refresh (it takes about a second); newest
                // requests first: they are what is on screen now.
                job = iconJobs_.back();
                iconJobs_.pop_back();
            } else {
                refresh = true;
                refresh_ = false;
            }
        }
        if (refresh) {
            ULONGLONG t0 = GetTickCount64();
            std::vector<CatalogApp> apps;
            try {
                apps = Enumerate();
            } catch (...) {
                Log(L"catalog: enumeration failed");
            }
            Log(L"catalog: %zu apps in %llu ms", apps.size(), GetTickCount64() - t0);
            // The start menu's pins: exported again only when its layout file changed.
            std::vector<CatalogApp> pins;
            try {
                wchar_t local[MAX_PATH] = {};
                GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
                std::wstring bin = std::wstring(local) + L"\\Packages\\Microsoft.Windows.StartMenuExperienceHost_cw5n1h2txyewy\\LocalState\\start2.bin";
                WIN32_FILE_ATTRIBUTE_DATA fa{};
                GetFileAttributesExW(bin.c_str(), GetFileExInfoStandard, &fa);
                if (CompareFileTime(&fa.ftLastWriteTime, &layoutTime_) != 0 || layout_.empty()) {
                    ULONGLONG t1 = GetTickCount64();
                    std::wstring json = ExportStartLayout();
                    // Only a complete export replaces the pins; otherwise the old ones stay and the next refresh tries again.
                    if (json.find(L"\"pinnedList\"") != std::wstring::npos) {
                        layout_ = JsonStringPairs(json);
                        layoutTime_ = fa.ftLastWriteTime;
                    }
                    Log(L"catalog: start layout exported (%zu entries) in %llu ms", layout_.size(), GetTickCount64() - t1);
                }
                pins = ResolvePins(layout_, apps);
            } catch (...) {
                Log(L"catalog: start pins failed");
            }
            {
                std::lock_guard g(mutex_);
                apps_ = std::move(apps);
                startPins_ = std::move(pins);
                liveIds_.clear();
                for (const auto& app : apps_) liveIds_.insert(app.id);
                for (const auto& app : startPins_) liveIds_.insert(app.id);
                failed_.clear(); // catalogue changes allow another bounded attempt
                loaded_ = true;
                PruneIcons();
                QueuePrefetch();
            }
            PostMessageW(notify_, msg_, 0, 0);
            continue;
        }
        IconImage img;
        LoadShellImage(IsPath(job.first) ? job.first : L"shell:AppsFolder\\" + job.first, job.second, img);
        {
            std::lock_guard g(mutex_);
            requested_.erase(job);
            if (!img.empty()) {
                icons_[job] = {std::move(img), ++iconUse_};
                failed_.erase(job);
            } else {
                auto& failure = failed_[job];
                ++failure.attempts;
                failure.time = GetTickCount64();
            }
            PruneIcons();
        }
        PostMessageW(notify_, msg_, 1, 0);
    }
    winrt::uninit_apartment();
}

bool AppCatalog::Launch(const CatalogApp& app)
{
    std::wstring path = IsPath(app.id) ? app.id : L"shell:AppsFolder\\" + app.id;
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_FLAG_NO_UI;
    sei.lpFile = path.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        Log(L"catalog: launch of %s failed (%lu)", app.id.c_str(), GetLastError());
        return false;
    }
    Log(L"catalog: launched %s", app.id.c_str());
    return true;
}

PinnedApp AppCatalog::PinFor(const CatalogApp& app)
{
    PinnedApp p;
    p.name = app.name;
    // Desktop apps pin by their exe so the pin matches their windows; the rest
    // by the AppsFolder id, which also starts them (shell:AppsFolder\<id>).
    std::wstring lower = app.target;
    for (auto& c : lower)
        c = wchar_t(towlower(c));
    if (!app.target.empty() && lower.size() > 4 && lower.substr(lower.size() - 4) == L".exe") {
        p.path = app.target;
        p.args = app.args;
    }
    else if (IsPath(app.id))
        p.path = app.id; // a shortcut outside the catalog
    else
        p.aumid = !app.aumid.empty() ? app.aumid : app.id;
    return p;
}

bool AppCatalog::Matches(const CatalogApp& app, const std::wstring& query)
{
    if (query.empty())
        return true;
    std::wstring q = Fold(query);
    if (Fold(app.name).find(q) != std::wstring::npos)
        return true;
    // Localised names hide the English one ("计算器"); the id and the exe
    // usually carry it (Microsoft.WindowsCalculator_..., notepad.exe).
    if (std::all_of(q.begin(), q.end(), [](wchar_t c) { return c < 128 && iswalnum(c); }) && NameInitials(app.name).rfind(q, 0) == 0)
        return true;
    auto tail = [](const std::wstring& s) { size_t p = s.find_last_of(L"\\/"); return p == std::wstring::npos ? s : s.substr(p + 1); };
    return q.size() >= 3 && (Fold(tail(app.id)).find(q) != std::wstring::npos || Fold(tail(app.target)).find(q) != std::wstring::npos);
}

} // namespace atrium
