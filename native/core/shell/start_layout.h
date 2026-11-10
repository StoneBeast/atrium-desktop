// The start menu's own arrangement, %LOCALAPPDATA%\Atrium\start.json: its
// size (small, medium or large), the apps the user hid, and its tiles in the user's order, apps and folders of apps.
// Until the user arranges something the menu shows the apps pinned to the
// Windows start menu (`custom` false); the first change copies them in, and
// from then on the two are separate. Also the initials the menu searches and
// indexes names by (the pinyin of Chinese names).
#pragma once

#include <string>
#include <vector>

namespace atrium {

struct StartTile {
    std::wstring id;   // an app: its catalog id (CatalogApp::id); a folder: "folder:<n>"
    std::wstring name; // an app: its name when saved (shown when it is not found); a folder: its name
    std::vector<StartTile> apps; // a folder's apps, in order
    bool Folder() const { return id.rfind(L"folder:", 0) == 0; }
};

struct StartLayout {
    // The three sizes, in tiles across and down: small, medium (the default), large.
    static constexpr int kSizes = 3, kMedium = 1;
    static constexpr int kCols[kSizes] = {7, 9, 11}, kRows[kSizes] = {3, 4, 5};
    int size = kMedium;
    bool custom = false;           // tiles arranged by the user (else the Windows pins)
    std::vector<StartTile> tiles;
    std::vector<std::wstring> hidden; // apps kept out of the menu (but for the end of the A-Z list)
    void Clamp();
    std::wstring NewFolderId() const;
};

StartLayout LoadStartLayout();
bool SaveStartLayout(const StartLayout& layout);

// The first letter of each word, and for each Chinese character the first
// letter of its pinyin (the 3755 common characters of GB2312), lower case:
// "Visual Studio Code" -> "vsc", "微信" -> "wx".
std::wstring NameInitials(const std::wstring& name);
// The A-Z index's letter of a name: 'A'..'Z', or '#' (digits, symbols, rare characters).
wchar_t IndexLetter(const std::wstring& name);

} // namespace atrium
