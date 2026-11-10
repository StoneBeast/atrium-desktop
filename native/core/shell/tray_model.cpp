#include "shell/tray_model.h"

#include "log.h"
#include "shell/tray_broker.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <uiautomation.h>
#include <wincodec.h>

#include <winrt/base.h>

#include <algorithm>
#include <map>
#include <set>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "shlwapi.lib")

namespace atrium {

namespace {

constexpr int kIconPx = 64;

std::wstring Lower(std::wstring s)
{
    if (!s.empty())
        LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, s.c_str(), int(s.size()), s.data(), int(s.size()), nullptr, nullptr, 0);
    return s;
}

// Lower case without spaces and the punctuation tooltips decorate names with.
std::wstring Key(const std::wstring& s)
{
    std::wstring out;
    for (wchar_t c : Lower(s))
        if (!iswspace(c) && !wcschr(L"-_:：·|()（）[]【】.,，", c))
            out += c;
    return out;
}

std::wstring VersionString(const std::wstring& path, const wchar_t* field)
{
    DWORD handle = 0, size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (!size)
        return {};
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()))
        return {};
    struct Lang {
        WORD lang, codepage;
    }* langs = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&langs), &len) || len < sizeof(Lang))
        return {};
    wchar_t sub[96];
    swprintf_s(sub, L"\\StringFileInfo\\%04x%04x\\%s", langs[0].lang, langs[0].codepage, field);
    wchar_t* value = nullptr;
    if (VerQueryValueW(data.data(), sub, reinterpret_cast<void**>(&value), &len) && value && len)
        return value;
    return {};
}

// "{6D809377-...}\Tailscale\tailscale-ipn.exe": a known folder, then the rest.
std::wstring ResolveKnownFolder(const std::wstring& path)
{
    if (path.size() < 39 || path[0] != L'{')
        return path;
    GUID id{};
    if (FAILED(CLSIDFromString(path.substr(0, 38).c_str(), &id)))
        return path;
    PWSTR base = nullptr;
    std::wstring out = path;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &base)))
        out = std::wstring(base) + path.substr(38);
    CoTaskMemFree(base);
    return out;
}

bool DecodePng(const std::vector<BYTE>& bytes, IconImage& out)
{
    winrt::com_ptr<IWICImagingFactory> wic;
    winrt::com_ptr<IWICBitmapDecoder> decoder;
    winrt::com_ptr<IWICBitmapFrameDecode> frame;
    winrt::com_ptr<IWICFormatConverter> conv;
    winrt::com_ptr<IStream> stream;
    stream.attach(SHCreateMemStream(bytes.data(), UINT(bytes.size())));
    if (!stream || FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put()))) ||
        FAILED(wic->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.put())) ||
        FAILED(decoder->GetFrame(0, frame.put())) || FAILED(wic->CreateFormatConverter(conv.put())) ||
        FAILED(conv->Initialize(frame.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        return false;
    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    if (!w || !h || w > 512 || h > 512)
        return false;
    out.width = int(w);
    out.height = int(h);
    out.bgra.resize(size_t(w) * h * 4);
    if (FAILED(conv->CopyPixels(nullptr, w * 4, UINT(out.bgra.size()), out.bgra.data()))) return false;
    for (size_t i = 3; i < out.bgra.size(); i += 4)
        if (out.bgra[i]) return true;
    return false; // an empty startup snapshot must fall back to the executable's artwork
}

// Windows' record of the icons that have been in the tray.
struct Record {
    std::wstring id;      // Windows' record identity; separates multiple icons from one program
    std::wstring exe;     // full path, lower case
    std::wstring tip;     // tooltip when first seen; often empty
    std::vector<BYTE> png; // icon snapshot
    bool promoted = false; // shown on the taskbar
    bool hasUid = false;   // the icon's id within its program (NOTIFYICONDATA.uID)
    UINT uid = 0;
    GUID guid{};
    size_t order = SIZE_MAX; // place in Windows' order of the icons (UIOrderList), if listed
};

// Reads the records; `ordered` tells whether Windows' order of them was there.
std::vector<Record> ReadRecords(bool& ordered)
{
    std::vector<Record> out;
    ordered = false;
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings", 0, KEY_READ, &root) != ERROR_SUCCESS)
        return out;
    // UIOrderList: the records' ids (their key names) as 64-bit numbers, in
    // the order the taskbar and its overflow window show the icons.
    std::map<unsigned long long, size_t> order;
    DWORD bytes = 0;
    if (RegGetValueW(root, nullptr, L"UIOrderList", RRF_RT_REG_BINARY, nullptr, nullptr, &bytes) == ERROR_SUCCESS && bytes >= 8 && bytes < (1u << 16)) {
        std::vector<unsigned long long> ids(bytes / 8);
        DWORD got = DWORD(ids.size() * 8);
        if (RegGetValueW(root, nullptr, L"UIOrderList", RRF_RT_REG_BINARY, nullptr, ids.data(), &got) == ERROR_SUCCESS)
            for (size_t i = 0; i < ids.size(); ++i)
                order.try_emplace(ids[i], i);
    }
    ordered = !order.empty();
    for (DWORD i = 0;; ++i) {
        wchar_t name[128];
        DWORD len = 128;
        if (RegEnumKeyExW(root, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        Record r;
        r.id = name;
        wchar_t buf[1024];
        DWORD size = sizeof(buf);
        if (RegGetValueW(root, name, L"ExecutablePath", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
            r.exe = Lower(AppExecutable(ResolveKnownFolder(buf)));
        size = sizeof(buf);
        if (RegGetValueW(root, name, L"InitialTooltip", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
            r.tip = buf;
        size = 0;
        if (RegGetValueW(root, name, L"IconSnapshot", RRF_RT_REG_BINARY, nullptr, nullptr, &size) == ERROR_SUCCESS && size && size < (1u << 20)) {
            r.png.resize(size);
            RegGetValueW(root, name, L"IconSnapshot", RRF_RT_REG_BINARY, nullptr, r.png.data(), &size);
        }
        DWORD uid = 0;
        size = sizeof(uid);
        if (RegGetValueW(root, name, L"UID", RRF_RT_REG_DWORD, nullptr, &uid, &size) == ERROR_SUCCESS)
            r.hasUid = true, r.uid = uid;
        size = sizeof(buf);
        if (RegGetValueW(root, name, L"IconGuid", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
            CLSIDFromString(buf, &r.guid);
        DWORD promoted = 0;
        size = sizeof(promoted);
        if (RegGetValueW(root, name, L"IsPromoted", RRF_RT_REG_DWORD, nullptr, &promoted, &size) == ERROR_SUCCESS)
            r.promoted = promoted != 0;
        if (auto it = order.find(wcstoull(name, nullptr, 10)); it != order.end())
            r.order = it->second;
        if (!r.exe.empty())
            out.push_back(std::move(r));
    }
    RegCloseKey(root);
    return out;
}

struct Program {
    std::vector<std::wstring> titles; // of its visible windows
    std::vector<HWND> windows;        // all its top-level and message-only windows
};

// Running programs by path (lower case).
std::map<std::wstring, Program> RunningPrograms()
{
    std::map<DWORD, std::wstring> paths;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{sizeof(pe)};
    if (snap != INVALID_HANDLE_VALUE) {
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
            // Elevated/service-owned icons still have a shell window and UID,
            // even when Windows will not expose their full process path.
            paths[pe.th32ProcessID] = L"|" + Lower(pe.szExeFile);
            if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID)) {
                wchar_t buf[MAX_PATH * 2];
                DWORD len = DWORD(std::size(buf));
                if (QueryFullProcessImageNameW(p, 0, buf, &len))
                    paths[pe.th32ProcessID] = Lower(buf);
                CloseHandle(p);
            }
        }
        CloseHandle(snap);
    }
    std::map<std::wstring, Program> out;
    for (const auto& [pid, path] : paths)
        out[path];
    struct Ctx {
        std::map<DWORD, std::wstring>* paths;
        std::map<std::wstring, Program>* out;
        bool titles;
    } ctx{&paths, &out, true};
    auto each = [](HWND h, LPARAM l) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(l);
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        auto it = c->paths->find(pid);
        if (it == c->paths->end())
            return TRUE;
        Program& p = (*c->out)[it->second];
        p.windows.push_back(h);
        wchar_t title[256];
        if (c->titles && IsWindowVisible(h) && InternalGetWindowText(h, title, 256) > 0) // sends no message (a hung window)
            p.titles.push_back(title);
        return TRUE;
    };
    EnumWindows(each, reinterpret_cast<LPARAM>(&ctx));
    // Tray icons often belong to message-only windows, which EnumWindows skips.
    ctx.titles = false;
    for (HWND h = FindWindowExW(HWND_MESSAGE, nullptr, nullptr, nullptr); h; h = FindWindowExW(HWND_MESSAGE, h, nullptr, nullptr))
        each(h, reinterpret_cast<LPARAM>(&ctx));
    return out;
}

// Whether one of a program's windows has the tray icon `uid` now (the shell
// knows the icon's place; it fails for icons that are not there).
bool HasIcon(const std::vector<HWND>& windows, UINT uid, RECT* bounds = nullptr)
{
    for (HWND h : windows) {
        NOTIFYICONIDENTIFIER id{sizeof(id), h, uid, GUID_NULL};
        RECT r{};
        if (SUCCEEDED(Shell_NotifyIconGetRect(&id, &r))) {
            if (bounds) *bounds = r;
            return true;
        }
    }
    return false;
}

bool LiveIcon(const Record& record, const Program& program, RECT& bounds)
{
    if (record.guid != GUID_NULL) {
        NOTIFYICONIDENTIFIER id{sizeof(id), nullptr, 0, record.guid};
        return SUCCEEDED(Shell_NotifyIconGetRect(&id, &bounds));
    }
    return record.hasUid && HasIcon(program.windows, record.uid, &bounds);
}

// How well a tooltip names a program: 0 none .. 100 exactly.
int Score(const std::wstring& tipKey, const std::wstring& candidate)
{
    std::wstring c = Key(candidate);
    if (c.size() < 2 || tipKey.empty())
        return 0;
    if (c == tipKey)
        return 100;
    if (tipKey.rfind(c, 0) == 0 || c.rfind(tipKey, 0) == 0)
        return 80;
    if (tipKey.find(c) != std::wstring::npos || c.find(tipKey) != std::wstring::npos)
        return 60;
    // "Windows 安全中心 - 建议的操作" and "Windows 安全中心通知图标": a long common start.
    size_t common = 0;
    while (common < c.size() && common < tipKey.size() && c[common] == tipKey[common])
        ++common;
    if (common >= 4 && common * 5 >= std::min(c.size(), tipKey.size()) * 3)
        return 40;
    return 0;
}

struct Found {
    winrt::com_ptr<IUIAutomationElement> element;
    std::wstring tip;
    bool promoted;
    int index;
    RECT bounds{};
    std::wstring recordId;
};

// The tray buttons in the taskbar and in the overflow window, in order.
std::vector<Found> FindButtons(IUIAutomation* uia, bool& available)
{
    std::vector<Found> out;
    available = false;
    auto collect = [&](HWND host, bool promoted) {
        HWND bridge = host ? FindWindowExW(host, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr) : nullptr;
        winrt::com_ptr<IUIAutomationElement> root;
        if (!host || FAILED(uia->ElementFromHandle(bridge ? bridge : host, root.put())) || !root)
            return;
        available = true;
        winrt::com_ptr<IUIAutomationCondition> cond;
        VARIANT v;
        v.vt = VT_BSTR;
        v.bstrVal = SysAllocString(L"NotifyItemIcon");
        HRESULT hr = uia->CreatePropertyCondition(UIA_AutomationIdPropertyId, v, cond.put());
        VariantClear(&v);
        winrt::com_ptr<IUIAutomationElementArray> all;
        if (FAILED(hr) || FAILED(root->FindAll(TreeScope_Descendants, cond.get(), all.put())) || !all)
            return;
        int n = 0;
        all->get_Length(&n);
        for (int i = 0; i < n; ++i) {
            Found f;
            all->GetElement(i, f.element.put());
            BSTR name = nullptr;
            if (f.element)
                f.element->get_CurrentName(&name);
            f.tip = name ? name : L"";
            SysFreeString(name);
            f.promoted = promoted;
            f.index = int(std::count_if(out.begin(), out.end(), [&](const Found& o) { return o.promoted == promoted; }));
            if (f.element)
                f.element->get_CurrentBoundingRectangle(&f.bounds);
            out.push_back(std::move(f));
        }
    };
    collect(FindWindowW(L"Shell_TrayWnd", nullptr), true);
    collect(FindWindowW(L"TopLevelWindowForOverflowXamlIsland", nullptr), false);
    return out;
}

// Only on an explicit icon action: let Explorer create its lazy overflow,
// then collapse it again. Reading icons never opens or focuses the system tray.
bool PrepareOverflow(IUIAutomation* uia)
{
    HWND host = FindWindowW(L"Shell_TrayWnd", nullptr);
    HWND overflow = FindWindowW(L"TopLevelWindowForOverflowXamlIsland", nullptr);
    if (!host || (overflow && IsWindowVisible(overflow))) return false;
    winrt::com_ptr<IUIAutomationElement> root, chevron;
    winrt::com_ptr<IUIAutomationCondition> cond, automationId, button;
    HWND bridge = FindWindowExW(host, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr);
    if (FAILED(uia->ElementFromHandle(bridge ? bridge : host, root.put())) || !root) return false;
    VARIANT value;
    value.vt = VT_BSTR;
    value.bstrVal = SysAllocString(L"SystemTray.NormalButton");
    HRESULT hr = uia->CreatePropertyCondition(UIA_ClassNamePropertyId, value, button.put());
    VariantClear(&value);
    if (FAILED(hr)) return false;
    value.vt = VT_BSTR;
    value.bstrVal = SysAllocString(L"SystemTrayIcon");
    hr = uia->CreatePropertyCondition(UIA_AutomationIdPropertyId, value, automationId.put());
    VariantClear(&value);
    if (FAILED(hr) || FAILED(uia->CreateAndCondition(button.get(), automationId.get(), cond.put()))) return false;
    // The overflow chevron is the first normal SystemTrayIcon button;
    // notification buttons use NotifyItemIcon, and the input indicator follows them.
    if (FAILED(root->FindFirst(TreeScope_Descendants, cond.get(), chevron.put())) || !chevron) return false;
    winrt::com_ptr<IUIAutomationInvokePattern> invoke;
    if (FAILED(chevron->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(invoke.put()))) || !invoke || FAILED(invoke->Invoke()))
        return false;
    bool ready = false;
    for (int i = 0; i < 10; ++i) {
        overflow = FindWindowW(L"TopLevelWindowForOverflowXamlIsland", nullptr);
        bool available = false;
        auto buttons = FindButtons(uia, available);
        ready = std::any_of(buttons.begin(), buttons.end(), [](const Found& f) { return !f.promoted && f.element; });
        if (ready) break;
        Sleep(20);
    }
    if (overflow && IsWindowVisible(overflow)) invoke->Invoke();
    return ready;
}

struct Candidate {
        size_t record;
        std::vector<std::wstring> names; // tooltip, description, product, file name, others, then window titles
        size_t firstTitle;
        RECT bounds{};
        bool live = false;
};

std::vector<int> MatchRecords(const std::vector<Found>& buttons, const std::vector<Record>& records,
                              const std::vector<Candidate>& candidates, bool ordered)
{
    // Name and live geometry establish identity before the shell's potentially stale ordering.
    // How well button b's tooltip names candidate c: 0 .. 105.
    auto score = [&](size_t b, size_t c) {
        std::wstring key = Key(TrayModel::Title(buttons[b].tip));
        int best = 0;
        for (size_t k = 0; k < candidates[c].names.size(); ++k) {
            int sc = Score(key, candidates[c].names[k]);
            // Window titles only when they are the tooltip: a browser's
            // page titles would otherwise claim other apps' icons.
            if (k >= candidates[c].firstTitle && sc < 100)
                sc = 0;
            best = std::max(best, sc + (k == 0 && sc ? 5 : 0)); // the recorded tooltip counts most
        }
        return best;
    };
    std::vector<int> match(buttons.size(), -1);
    std::set<int> used;
    for (size_t b = 0; b < buttons.size(); ++b) {
        const RECT& cell = buttons[b].bounds;
        if (cell.right <= cell.left || cell.bottom <= cell.top) continue;
        int record = -1;
        for (const auto& c : candidates) {
            const RECT& icon = c.bounds;
            POINT center{(icon.left + icon.right) / 2, (icon.top + icon.bottom) / 2};
            if (icon.right <= icon.left || icon.bottom <= icon.top || !PtInRect(&cell, center) || used.contains(int(c.record))) continue;
            if (record >= 0) { record = -1; break; } // ambiguous geometry: keep the name/order path
            record = int(c.record);
        }
        if (record >= 0) match[b] = record, used.insert(record);
    }
    // Exact program names must not be displaced by an obsolete saved ordering or promotion setting.
    for (size_t b = 0; b < buttons.size(); ++b) {
        if (match[b] >= 0) continue;
        int best = 99, record = -1;
        for (size_t c = 0; c < candidates.size(); ++c) {
            int r = int(candidates[c].record), sc = score(b, c);
            if (!used.contains(r) && sc > best) best = sc, record = r;
        }
        if (record >= 0) match[b] = record, used.insert(record);
    }
    // Only after live identities: a service-owned icon can use its cached artwork without a main process.
    for (size_t b = 0; b < buttons.size(); ++b) {
        auto key = Key(TrayModel::Title(buttons[b].tip));
        if (match[b] >= 0 || key.empty()) continue;
        for (size_t r = 0; r < records.size(); ++r)
            if (!used.contains(int(r)) && Key(TrayModel::Title(records[r].tip)) == key) {
                match[b] = int(r); used.insert(int(r)); break;
            }
    }
    if (ordered) {
        // Windows shows the icons in the order of its records: line the
        // buttons up with the running programs' records in that order,
        // as many as possible, the names deciding where they disagree.
        // This also places icons without a tooltip.
        for (bool promoted : {true, false}) {
            std::vector<size_t> bs, cs;
            for (size_t b = 0; b < buttons.size(); ++b)
                if (match[b] < 0 && buttons[b].promoted == promoted)
                    bs.push_back(b);
            for (size_t c = 0; c < candidates.size(); ++c)
                if (!used.contains(int(candidates[c].record)) && records[candidates[c].record].promoted == promoted && records[candidates[c].record].order != SIZE_MAX)
                    cs.push_back(c);
            std::stable_sort(cs.begin(), cs.end(),
                             [&](size_t a, size_t b) { return records[candidates[a].record].order < records[candidates[b].record].order; });
            const size_t n = bs.size(), m = cs.size();
            std::vector<std::vector<int>> w(n, std::vector<int>(m)), dp(n + 1, std::vector<int>(m + 1, 0));
            for (size_t i = 0; i < n; ++i)
                for (size_t j = 0; j < m; ++j) {
                    int sc = score(bs[i], cs[j]);
                    // In place, the name agreeing; a named button whose name
                    // fits nothing of the program's counts for less.
                    w[i][j] = 10 + sc - (sc == 0 && !TrayModel::Title(buttons[bs[i]].tip).empty() ? 6 : 0);
                }
            for (size_t i = 1; i <= n; ++i)
                for (size_t j = 1; j <= m; ++j)
                    dp[i][j] = std::max({dp[i - 1][j], dp[i][j - 1], dp[i - 1][j - 1] + w[i - 1][j - 1]});
            // Back from the end; on a tie the earlier record wins.
            for (size_t i = n, j = m; i > 0 && j > 0;) {
                if (dp[i][j] == dp[i][j - 1])
                    --j;
                else if (dp[i][j] == dp[i - 1][j - 1] + w[i - 1][j - 1]) {
                    match[bs[i - 1]] = int(candidates[cs[j - 1]].record);
                    --i;
                    --j;
                } else
                    --i;
            }
        }
    } else {
        // No order: by name alone, best names first, each program once.
        struct Pair {
            int score;
            size_t button, candidate;
        };
        std::vector<Pair> pairs;
        for (size_t b = 0; b < buttons.size(); ++b)
            for (size_t c = 0; c < candidates.size(); ++c)
                if (int sc = score(b, c); sc > 0)
                    pairs.push_back({sc + (records[candidates[c].record].png.empty() ? 0 : 1), b, c});
        std::stable_sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.score > b.score; });
        std::set<std::wstring> usedExe;
        std::set<size_t> usedCandidate;
        for (const Pair& p : pairs)
            if (match[p.button] < 0 && !used.contains(int(candidates[p.candidate].record)) && !usedCandidate.count(p.candidate) && !usedExe.count(records[candidates[p.candidate].record].exe)) {
                match[p.button] = int(candidates[p.candidate].record);
                usedCandidate.insert(p.candidate);
                usedExe.insert(records[candidates[p.candidate].record].exe);
            }
    }
    // Explorer's saved order can lag behind its live buttons. A clearly named,
    // unmatched button still owns its live program's record (also while its main window is hidden).
    for (int r : match) if (r >= 0) used.insert(r);
    for (size_t b = 0; b < buttons.size(); ++b) {
        if (match[b] >= 0 || TrayModel::Title(buttons[b].tip).empty()) continue;
        int best = 79, record = -1;
        for (size_t c = 0; c < candidates.size(); ++c) {
            int r = int(candidates[c].record), sc = score(b, c);
            if (!used.contains(r) && sc > best) best = sc, record = r;
        }
        if (record >= 0) match[b] = record, used.insert(record);
    }
    return match;
}

std::vector<int> MatchButtons(std::vector<Found>& buttons, std::vector<Record>& records, bool& ordered,
                             const std::vector<PinnedApp>& apps = {})
{
    records = ReadRecords(ordered);
    auto running = RunningPrograms();
    std::map<std::wstring, std::vector<std::wstring>> labels;
    for (const auto& app : apps)
        if (!app.path.empty() && !app.name.empty()) labels[Lower(AppExecutable(app.path))].push_back(app.name);
    std::vector<Candidate> candidates;
    std::map<std::wstring, std::pair<std::wstring, std::wstring>> version;
    for (size_t r = 0; r < records.size(); ++r) {
        auto run = running.find(records[r].exe);
        if (run == running.end()) run = running.find(L"|" + Lower(PathFindFileNameW(records[r].exe.c_str())));
        if (run == running.end()) continue;
        RECT bounds{};
        bool live = LiveIcon(records[r], run->second, bounds);
        if ((records[r].hasUid || records[r].guid != GUID_NULL) && !live) continue;
        auto& v = version[records[r].exe];
        if (v.first.empty() && v.second.empty())
            v = {VersionString(records[r].exe, L"FileDescription"), VersionString(records[r].exe, L"ProductName")};
        std::wstring stem = PathFindFileNameW(records[r].exe.c_str());
        PathRemoveExtensionW(stem.data());
        stem.resize(wcslen(stem.c_str()));
        Candidate c{r, {records[r].tip, v.first, v.second, stem}, 0, bounds, live};
        if (auto names = labels.find(records[r].exe); names != labels.end())
            c.names.insert(c.names.end(), names->second.begin(), names->second.end());
        c.firstTitle = c.names.size();
        c.names.insert(c.names.end(), run->second.titles.begin(), run->second.titles.end());
        candidates.push_back(std::move(c));
    }
    auto match = MatchRecords(buttons, records, candidates, ordered);
    std::set<int> used(match.begin(), match.end());
    std::stable_sort(candidates.begin(), candidates.end(), [&](const Candidate& a, const Candidate& b) {
        return records[a.record].order < records[b.record].order;
    });
    // Explorer creates the overflow's UI Automation buttons only after its
    // first opening. Live shell identities exist before then; persisted records
    // alone must never resurrect icons from applications that have exited.
    for (const auto& c : candidates) {
        if (!c.live || used.contains(int(c.record))) continue;
        const auto& r = records[c.record];
        Found f;
        f.tip = r.tip.empty() ? c.names[1] : r.tip;
        if (f.tip.empty()) f.tip = c.names[3];
        f.promoted = r.promoted;
        f.index = int(std::count_if(buttons.begin(), buttons.end(), [&](const Found& b) { return b.promoted == f.promoted; }));
        f.bounds = c.bounds;
        f.recordId = r.id;
        buttons.push_back(std::move(f));
        match.push_back(int(c.record));
    }
    for (size_t b = 0; b < buttons.size(); ++b)
        if (match[b] >= 0) buttons[b].recordId = records[size_t(match[b])].id;
    return match;
}

} // namespace

std::wstring TrayModel::Title(const std::wstring& tip)
{
    size_t a = tip.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos)
        return {};
    size_t b = tip.find_first_of(L"\r\n", a);
    std::wstring line = tip.substr(a, b == std::wstring::npos ? std::wstring::npos : b - a);
    while (!line.empty() && iswspace(line.back()))
        line.pop_back();
    return line;
}

TrayModel::TrayModel(HWND notify, UINT msg, std::function<std::vector<PinnedApp>()> appNames)
    : notify_(notify), msg_(msg), appNames_(std::move(appNames))
{
    thread_ = std::thread([this] { Worker(); });
}

TrayModel::~TrayModel()
{
    {
        std::lock_guard g(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

void TrayModel::Refresh()
{
    std::lock_guard g(mutex_);
    if (std::none_of(jobs_.begin(), jobs_.end(), [](const Job& j) { return j.refresh; }))
        jobs_.push_back({true, {}, false});
    cv_.notify_all();
}

void TrayModel::Click(const TrayIcon& icon, bool right)
{
    // The broker knows the icon's own callback message: that is exactly what
    // Explorer sends, no UI Automation and no opening the overflow window.
    if (Broker().Click(icon.exe, icon.uid, icon.hasGuid ? &icon.guid : nullptr, right))
        return;
    std::lock_guard g(mutex_);
    jobs_.push_back({false, icon, right});
    cv_.notify_all();
}

bool TrayModel::LiveRefresh()
{
    bool changed = false;
    std::lock_guard g(mutex_);
    for (TrayIcon& icon : icons_) {
        if (icon.exe.empty())
            continue;
        auto live = Broker().Image(icon.exe, icon.uid, icon.hasGuid ? &icon.guid : nullptr);
        if (!live || live == icon.image)
            continue;
        icon.image = std::move(live);
        changed = true;
    }
    return changed;
}

bool TrayModel::OpenExisting(const std::wstring& exe)
{
    if (exe.empty()) return false;
    for (const TrayIcon& icon : Icons()) {
        if (!icon.exe.empty() && _wcsicmp(AppExecutable(icon.exe).c_str(), exe.c_str()) == 0) {
            AllowSetForegroundWindow(ASFW_ANY);
            Click(icon, false);
            return true;
        }
    }
    // At startup the tray's first read may still be in progress. A resident messenger
    // must not be launched again: enqueue its exact program identity for the worker's fresh lookup.
    std::wstring name = Lower(PathFindFileNameW(exe.c_str()));
    if (name != L"feishu.exe" && name != L"qq.exe" && name != L"weixin.exe" && name != L"wechat.exe")
        return false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    bool running = false;
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !running; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, name.c_str()) != 0) continue;
        if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID)) {
            wchar_t path[MAX_PATH * 2]; DWORD size = DWORD(std::size(path));
            running = QueryFullProcessImageNameW(process, 0, path, &size) && _wcsicmp(path, exe.c_str()) == 0;
            CloseHandle(process);
        }
    }
    CloseHandle(snap);
    if (running) {
        TrayIcon icon;
        icon.exe = Lower(exe);
        AllowSetForegroundWindow(ASFW_ANY);
        Click(icon, false);
    }
    return running;
}

std::vector<TrayIcon> TrayModel::Icons() const
{
    std::lock_guard g(mutex_);
    return icons_;
}

bool TrayModel::Loaded() const
{
    std::lock_guard g(mutex_);
    return loaded_;
}

bool TrayModel::Available() const
{
    std::lock_guard g(mutex_);
    return available_;
}

void TrayModel::Worker()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    winrt::com_ptr<IUIAutomation> uia;
    if (FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(uia.put()))))
        Log(L"tray: UI Automation unavailable");
    struct Picture {
        std::wstring exe;
        std::vector<BYTE> snapshot;
        std::shared_ptr<const IconImage> image;
    };
    std::map<std::wstring, Picture> pictures;
    for (;;) {
        Job job;
        {
            std::unique_lock g(mutex_);
            cv_.wait(g, [&] { return stop_ || !jobs_.empty(); });
            if (stop_)
                break;
            job = jobs_.front();
            jobs_.pop_front();
        }
        if (!uia)
            continue;
        try {
            bool available = false;
            std::vector<Found> buttons = FindButtons(uia.get(), available);
            auto appNames = appNames_ ? appNames_() : std::vector<PinnedApp>{};
            if (!job.refresh) {
                // Resolve stable identity first; names and positions can change
                // while autostart applications are still adding their icons.
                auto find = [&]() {
                    if (!job.icon.exe.empty()) {
                        bool ordered = false;
                        std::vector<Record> records;
                        auto match = MatchButtons(buttons, records, ordered, appNames);
                        auto it = buttons.end(); // matching can append passive icons
                        std::vector<size_t> same;
                        for (size_t b = 0; b < buttons.size(); ++b)
                            if (match[b] >= 0 && records[size_t(match[b])].exe == job.icon.exe)
                                same.push_back(b);
                        for (size_t b : same)
                            if (it == buttons.end() && !job.icon.recordId.empty() && buttons[b].recordId == job.icon.recordId)
                                it = buttons.begin() + ptrdiff_t(b);
                        for (size_t b : same)
                            if (it == buttons.end() && buttons[b].promoted == job.icon.promoted && buttons[b].index == job.icon.index)
                                it = buttons.begin() + ptrdiff_t(b);
                        for (size_t b : same)
                            if (it == buttons.end() && buttons[b].tip == job.icon.tip)
                                it = buttons.begin() + ptrdiff_t(b);
                        if (it == buttons.end() && !same.empty())
                            it = buttons.begin() + ptrdiff_t(same.front());
                        if (it != buttons.end()) return it;
                    }
                    auto it = buttons.end();
                    if (!job.icon.tip.empty())
                        it = std::find_if(buttons.begin(), buttons.end(), [&](const Found& f) {
                            return f.promoted == job.icon.promoted && f.index == job.icon.index && f.tip == job.icon.tip;
                        });
                    if (it == buttons.end() && !job.icon.tip.empty())
                        it = std::find_if(buttons.begin(), buttons.end(), [&](const Found& f) { return f.tip == job.icon.tip; });
                    return it;
                };
                auto it = find();
                if ((it == buttons.end() || !it->element) && !job.icon.promoted && PrepareOverflow(uia.get())) {
                    buttons = FindButtons(uia.get(), available);
                    it = find();
                }
                HRESULT hr = E_FAIL;
                if (it != buttons.end() && it->element) {
                    if (job.right) {
                        winrt::com_ptr<IUIAutomationElement3> e3 = it->element.try_as<IUIAutomationElement3>();
                        hr = e3 ? e3->ShowContextMenu() : E_NOINTERFACE;
                    } else {
                        winrt::com_ptr<IUIAutomationInvokePattern> invoke;
                        hr = it->element->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(invoke.put()));
                        if (SUCCEEDED(hr) && invoke)
                            hr = invoke->Invoke();
                    }
                }
                Log(L"tray: %s on '%s' (%s) -> 0x%08X", job.right ? L"right click" : L"click", Title(job.icon.tip).c_str(),
                    job.icon.exe.empty() ? L"-" : PathFindFileNameW(job.icon.exe.c_str()), unsigned(hr));
                PostMessageW(notify_, msg_, 1, 0);
                continue;
            }

            bool ordered = false;
            std::vector<Record> records;
            std::vector<int> match = MatchButtons(buttons, records, ordered, appNames);

            std::vector<TrayIcon> icons;
            std::set<std::wstring> livePictures;
            int recognised = 0;
            for (size_t b = 0; b < buttons.size(); ++b) {
                TrayIcon t;
                t.tip = buttons[b].tip;
                t.promoted = buttons[b].promoted;
                t.index = buttons[b].index;
                t.recordId = buttons[b].recordId;
                if (match[b] >= 0) {
                    const Record& r = records[size_t(match[b])];
                    t.exe = r.exe;
                    t.uid = r.hasUid ? r.uid : 0;
                    t.hasGuid = r.guid != GUID_NULL;
                    if (t.hasGuid) t.guid = r.guid;
                    ++recognised;
                    const std::wstring& key = r.id;
                    livePictures.insert(key);
                    // The broker's icon is what the program shows now (a blink sends a new
                    // one with every message); the stored snapshot is the icon of the moment
                    // Windows first saw the program.
                    if (auto live = Broker().Image(r.exe, t.uid, t.hasGuid ? &t.guid : nullptr)) {
                        t.image = std::move(live);
                    } else {
                        auto& pic = pictures[key];
                        if (!pic.image || pic.exe != r.exe || pic.snapshot != r.png) {
                            IconImage img;
                            if (!LoadAtriumIcon(r.exe, kIconPx, img) && (r.png.empty() || !DecodePng(r.png, img)) && !LoadShellImage(r.exe, kIconPx, img))
                                img = {};
                            pic.exe = r.exe;
                            pic.snapshot = r.png;
                            pic.image = img.empty() ? nullptr : std::make_shared<const IconImage>(std::move(img));
                        }
                        t.image = pic.image;
                    }
                }
                icons.push_back(std::move(t));
            }
            std::erase_if(pictures, [&](const auto& item) { return !livePictures.contains(item.first); });
            bool changed;
            {
                std::lock_guard g(mutex_);
                changed = !loaded_ || icons.size() != icons_.size() ||
                          !std::equal(icons.begin(), icons.end(), icons_.begin(), [](const TrayIcon& a, const TrayIcon& b) {
                              return a.tip == b.tip && a.exe == b.exe && a.image == b.image && a.promoted == b.promoted && a.recordId == b.recordId;
                          });
                icons_ = std::move(icons);
                loaded_ = true;
                available_ = available;
            }
            if (changed) {
                Log(L"tray: %zu icons (%d recognised %s)%s", buttons.size(), recognised, ordered ? L"in Windows' order" : L"by name",
                    available ? L"" : L", the shell's tray was not found");
                for (size_t b = 0; b < buttons.size(); ++b) {
                    std::wstring exe = match[b] >= 0 ? std::wstring(PathFindFileNameW(records[size_t(match[b])].exe.c_str())) : L"-";
                    Log(L"tray:   %zu '%s' -> %s", b, Title(buttons[b].tip).substr(0, 24).c_str(), exe.c_str());
                }
                PostMessageW(notify_, msg_, 0, 0);
            }
        } catch (...) {
            Log(L"tray: reading the tray failed");
        }
    }
    uia = nullptr;
    winrt::uninit_apartment();
}

} // namespace atrium
