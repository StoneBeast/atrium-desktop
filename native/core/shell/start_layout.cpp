#include "shell/start_layout.h"

#include "log.h"
#include "settings.h"

#include <windows.h>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <cwctype>

namespace json = winrt::Windows::Data::Json;

namespace atrium {

namespace {

std::wstring LayoutPath()
{
    std::wstring path = SettingsPath(); // ...\Atrium\settings.json
    return path.substr(0, path.find_last_of(L'\\') + 1) + L"start.json";
}

std::wstring Str(const json::JsonObject& o, const wchar_t* key)
{
    return o.HasKey(key) && o.GetNamedValue(key).ValueType() == json::JsonValueType::String ? std::wstring(o.GetNamedString(key)) : std::wstring();
}

// GB2312 orders its level-one characters by pinyin: where each initial's run starts.
constexpr struct {
    unsigned code;
    wchar_t letter;
} kPinyin[] = {{0xB0A1, L'a'}, {0xB0C5, L'b'}, {0xB2C1, L'c'}, {0xB4EE, L'd'}, {0xB6EA, L'e'}, {0xB7A2, L'f'}, {0xB8C1, L'g'}, {0xB9FE, L'h'},
               {0xBBF7, L'j'}, {0xBFA6, L'k'}, {0xC0AC, L'l'}, {0xC2E8, L'm'}, {0xC4C3, L'n'}, {0xC5B6, L'o'}, {0xC5BE, L'p'}, {0xC6DA, L'q'},
               {0xC8BB, L'r'}, {0xC8F6, L's'}, {0xCBFA, L't'}, {0xCDDA, L'w'}, {0xCEF4, L'x'}, {0xD1B9, L'y'}, {0xD4D1, L'z'}};
constexpr unsigned kPinyinEnd = 0xD7FA;

// The pinyin initial of a Chinese character, or 0.
wchar_t PinyinInitial(wchar_t ch)
{
    if (ch < 0x4E00 || ch > 0x9FFF)
        return 0;
    char bytes[4] = {};
    BOOL lossy = FALSE;
    if (WideCharToMultiByte(936, 0, &ch, 1, bytes, 4, nullptr, &lossy) != 2 || lossy)
        return 0;
    const unsigned code = (unsigned(BYTE(bytes[0])) << 8) | BYTE(bytes[1]);
    if (code < kPinyin[0].code || code >= kPinyinEnd)
        return 0; // level two: ordered by radical, not by sound
    wchar_t letter = 0;
    for (const auto& p : kPinyin)
        if (code >= p.code)
            letter = p.letter;
    return letter;
}

} // namespace

void StartLayout::Clamp()
{
    size = std::clamp(size, 0, kSizes - 1);
    // A folder keeps at least one app; an app appears once.
    std::vector<std::wstring> seen;
    auto fresh = [&](const std::wstring& id) {
        if (id.empty() || std::find(seen.begin(), seen.end(), id) != seen.end())
            return false;
        seen.push_back(id);
        return true;
    };
    std::vector<StartTile> kept;
    for (StartTile& t : tiles) {
        if (t.Folder()) {
            std::erase_if(t.apps, [&](const StartTile& a) { return a.Folder() || !fresh(a.id); });
            if (!t.apps.empty() && fresh(t.id))
                kept.push_back(std::move(t));
        } else if (fresh(t.id)) {
            t.apps.clear();
            kept.push_back(std::move(t));
        }
    }
    tiles = std::move(kept);
    std::vector<std::wstring> once;
    for (std::wstring& id : hidden)
        if (!id.empty() && std::find(once.begin(), once.end(), id) == once.end())
            once.push_back(std::move(id));
    hidden = std::move(once);
}

std::wstring StartLayout::NewFolderId() const
{
    unsigned long next = 1;
    for (const StartTile& t : tiles)
        if (t.Folder())
            next = std::max(next, wcstoul(t.id.c_str() + 7, nullptr, 10) + 1);
    return L"folder:" + std::to_wstring(next);
}

StartLayout LoadStartLayout()
{
    StartLayout layout;
    std::wstring text = ReadTextFile(LayoutPath());
    json::JsonObject o{nullptr};
    if (text.empty())
        return layout;
    if (!json::JsonObject::TryParse(text, o)) {
        Log(L"start: %s is not valid JSON, using the Windows pins", LayoutPath().c_str());
        return layout;
    }
    const std::wstring size = Str(o, L"size");
    layout.size = size == L"small" ? 0 : size == L"large" ? 2 : StartLayout::kMedium;
    if (o.GetNamedNumber(L"version", 1) < 2) {
        // Version 1: sizes a step smaller (5 x 3, 7 x 3, 9 x 4), or before them the tiles across and down.
        if (!size.empty())
            layout.size = size == L"large" ? 1 : 0;
        else if (o.HasKey(L"cols") && o.HasKey(L"rows"))
            layout.size = o.GetNamedNumber(L"cols", 7) >= 9 || o.GetNamedNumber(L"rows", 3) >= 4 ? 1 : 0;
    }
    auto tile = [](const json::JsonObject& e) {
        StartTile t{Str(e, L"id"), Str(e, L"name"), {}};
        if (e.HasKey(L"apps") && e.GetNamedValue(L"apps").ValueType() == json::JsonValueType::Array)
            for (const auto& v : e.GetNamedArray(L"apps"))
                if (v.ValueType() == json::JsonValueType::Object)
                    t.apps.push_back({Str(v.GetObject(), L"id"), Str(v.GetObject(), L"name"), {}});
        return t;
    };
    if (o.HasKey(L"tiles") && o.GetNamedValue(L"tiles").ValueType() == json::JsonValueType::Array) {
        layout.custom = true;
        for (const auto& v : o.GetNamedArray(L"tiles"))
            if (v.ValueType() == json::JsonValueType::Object)
                layout.tiles.push_back(tile(v.GetObject()));
    }
    if (o.HasKey(L"hidden") && o.GetNamedValue(L"hidden").ValueType() == json::JsonValueType::Array)
        for (const auto& v : o.GetNamedArray(L"hidden"))
            if (v.ValueType() == json::JsonValueType::String)
                layout.hidden.push_back(std::wstring(v.GetString()));
    layout.Clamp();
    return layout;
}

bool SaveStartLayout(const StartLayout& layout)
{
    json::JsonObject o;
    o.SetNamedValue(L"version", json::JsonValue::CreateNumberValue(2));
    o.SetNamedValue(L"size", json::JsonValue::CreateStringValue(layout.size == 0 ? L"small" : layout.size == 2 ? L"large" : L"medium"));
    auto entry = [](const StartTile& t) {
        json::JsonObject e;
        e.SetNamedValue(L"id", json::JsonValue::CreateStringValue(t.id));
        e.SetNamedValue(L"name", json::JsonValue::CreateStringValue(t.name));
        return e;
    };
    if (layout.custom) {
        json::JsonArray tiles;
        for (const StartTile& t : layout.tiles) {
            json::JsonObject e = entry(t);
            if (t.Folder()) {
                json::JsonArray apps;
                for (const StartTile& a : t.apps)
                    apps.Append(entry(a));
                e.SetNamedValue(L"apps", apps);
            }
            tiles.Append(e);
        }
        o.SetNamedValue(L"tiles", tiles);
    }
    if (!layout.hidden.empty()) {
        json::JsonArray hidden;
        for (const std::wstring& id : layout.hidden)
            hidden.Append(json::JsonValue::CreateStringValue(id));
        o.SetNamedValue(L"hidden", hidden);
    }
    if (WriteTextFile(LayoutPath(), std::wstring(o.Stringify())))
        return true;
    Log(L"start: could not save %s", LayoutPath().c_str());
    return false;
}

std::wstring NameInitials(const std::wstring& name)
{
    std::wstring out;
    bool wordStart = true;
    for (size_t i = 0; i < name.size(); ++i) {
        const wchar_t ch = name[i];
        if (wchar_t py = PinyinInitial(ch)) {
            out += py;
            wordStart = true;
        } else if (iswalnum(ch)) {
            // A word's first letter, and a capital inside one ("OneDrive" -> "od").
            if (wordStart || (iswupper(ch) && i > 0 && iswlower(name[i - 1])))
                out += wchar_t(towlower(ch));
            wordStart = false;
        } else {
            wordStart = true;
        }
    }
    return out;
}

wchar_t IndexLetter(const std::wstring& name)
{
    for (wchar_t ch : name) {
        if (iswspace(ch))
            continue;
        if (ch < 128 && iswalpha(ch))
            return wchar_t(towupper(ch));
        if (ch >= 0xFF21 && ch <= 0xFF5A && iswalpha(ch)) // full-width Latin
            return wchar_t(towupper(wchar_t(ch - 0xFEE0)));
        if (wchar_t py = PinyinInitial(ch))
            return wchar_t(towupper(py));
        return L'#';
    }
    return L'#';
}

} // namespace atrium
