<p align="center">
  <img src="assets/atrium-256.png" alt="Atrium Desktop" width="112">
</p>

<h1 align="center">Atrium Desktop</h1>

<p align="center">
  <b>More than a desktop theme.</b><br>
  Windows as you know it, with a pane of real-time refracting liquid glass,<br>
  and one design language joining the taskbar, start menu, control centre, notifications and widgets.
</p>

<p align="center">
  <a href="https://github.com/wynter-yaxley/atrium-desktop/releases/latest"><b>Download</b></a>&nbsp;&nbsp;·&nbsp;&nbsp;<a href="#features"><b>Features</b></a>&nbsp;&nbsp;·&nbsp;&nbsp;<a href="#getting-started"><b>Getting started</b></a>&nbsp;&nbsp;·&nbsp;&nbsp;<a href="README.md"><b>简体中文</b></a>
</p>

<p align="center">
  <a href="https://github.com/wynter-yaxley/atrium-desktop/releases/latest"><img src="https://img.shields.io/github/v/release/wynter-yaxley/atrium-desktop?style=flat-square&label=version&labelColor=1d1d1f&color=3a3a3c" alt="version"></a>
  <a href="https://github.com/wynter-yaxley/atrium-desktop/actions/workflows/build.yml"><img src="https://img.shields.io/github/actions/workflow/status/wynter-yaxley/atrium-desktop/build.yml?branch=main&style=flat-square&label=build&labelColor=1d1d1f" alt="build"></a>
  <img src="https://img.shields.io/badge/Windows-11-3a3a3c?style=flat-square&labelColor=1d1d1f" alt="Windows 11">
  <a href="LICENSE"><img src="https://img.shields.io/github/license/wynter-yaxley/atrium-desktop?style=flat-square&labelColor=1d1d1f&color=3a3a3c" alt="license"></a>
</p>

<p align="center">
  <img src="docs/images/readme/hero.jpg" alt="Atrium Desktop" width="100%">
</p>

## More than a desktop theme

The Windows desktop is several generations of design stacked on top of each other: the taskbar, the tray, the start menu, notifications and quick settings each have their own corners, type sizes and animations. Atrium redoes the layer you look at every day without changing how you use Windows.

- **Same habits**: pinning apps, switching windows, right-click menus and dropping a file on an app's icon to open it all work as before.
- **One glass**: the bar, panels, menus, notifications and widgets share one material that shows the desktop behind it in real time.
- **One design language**: corners, spacing, type and motion follow one set of rules, and the same control feels the same wherever it appears.
- **Three independent modules**: the taskbar, desktop widgets and Stage Manager turn on and off on their own. Use just one if you like.
- **Clean**: no code injected into system processes, no system files changed, no services, no administrator rights. When you quit, Windows' own taskbar comes back as usual.

## Features

### Liquid and frosted

<p align="center">
  <img src="docs/images/readme/material.jpg" alt="Liquid on the left, frosted on the right" width="100%">
</p>

The glass bends what is behind it, with a touch of highlight and dispersion at the edge. Liquid (left) is the clearer one, frosted (right) the calmer one; transparency, blur and refraction can be tuned in the settings. Text on the glass switches between dark and light with the brightness behind it.

### Taskbar

<p align="center">
  <img src="docs/images/readme/bar.jpg" alt="The icon under the pointer magnified, with the app's name above it" width="100%">
</p>

- A floating capsule of glass at the bottom of the screen. Icons swell like a wave under the pointer; rest for a moment and the app's name appears.
- Rest on an open app to see a live preview of each of its windows, and switch to or close one from there.
- Drag pinned apps to reorder them. When the app in front goes full screen or maximised, the bar steps aside (or keep it up, or always hidden). Optionally, Win + 1…9 open the bar's apps.

### Start menu

<table>
  <tr>
    <td width="50%"><img src="docs/images/readme/start-pins.jpg" alt="Pinned apps in the start menu"></td>
    <td width="50%"><img src="docs/images/readme/start-search.jpg" alt="Typing straight into the start menu's search"></td>
  </tr>
</table>

Opened from the Windows logo at the left of the bar, or optionally the Windows key. The apps pinned in the Windows start menu are here; drag them around, drop one onto another to make a folder and name it. Three sizes: small, medium and large. Just start typing to search apps and folders, no need to click the search box first; pinyin initials work too (wx finds 微信). All apps lists them A to Z with a letter index to jump through, and apps you do not want to see can be hidden. The strip below holds your account, network, volume, battery and power.

### Status ring and control centre

<p align="center">
  <img src="docs/images/readme/ring.jpg" alt="The status ring: charging, adjusting the volume, adjusting the brightness" width="100%">
</p>

Instead of a row of battery, network and volume icons, the right of the bar has a single ring. The outer ring is the battery, with a bolt in its gap while charging; the centre is Wi‑Fi. When you change the volume or brightness, the ring turns into that level and goes back to battery and Wi‑Fi 1.5 seconds later. Scroll over the ring to change the volume. If you prefer the Windows look, switch to network, volume and battery icons side by side.

<table>
  <tr>
    <td width="50%"><img src="docs/images/readme/control-center.jpg" alt="The control centre"></td>
    <td width="50%"><img src="docs/images/readme/notifications.jpg" alt="The notification list"></td>
  </tr>
</table>

Click the ring for the control centre: Wi‑Fi, Bluetooth, battery, casting, Stage Manager, display brightness, sound and output device, and a volume for each app. Click the bell for notifications: new ones stack up, and a click lays the stack out as a list. Click the clock for a calendar with the Chinese lunar calendar and holidays.

### Desktop widgets

<p align="center">
  <img src="docs/images/readme/widgets.jpg" alt="Widgets on the desktop" width="100%">
</p>

Ten kinds — clock, digital clock, calendar (with the lunar calendar), weather, battery, Bluetooth devices, system monitor, notes, reminders and photos — in three sizes. They snap to a grid wherever you drop them; hover one and an arc appears at its lower corner, drag it to resize. Widgets sit below all windows and come to the top when you press Win + D.

<p align="center">
  <img src="docs/images/readme/widget-materials.jpg" alt="The widgets' three materials: like the taskbar, dusk and colour" width="100%">
</p>

Three materials: the clear glass **like the taskbar**; **dusk**, the same glass dimmed with all-white text; and **colour**, opaque faces in their own colours.

### Stage Manager

<table>
  <tr>
    <td width="50%"><img src="docs/images/readme/stage.jpg" alt="Stage Manager: the current app in the middle, the others at the side"></td>
    <td width="50%"><img src="docs/images/readme/stage-hover.jpg" alt="A window at the side turns to face you and grows under the pointer"></td>
  </tr>
</table>

The app you are using stays in the middle of the screen; the others move to one side, their windows standing at an angle in a column, several windows of one app stacked together. Hover one and it turns to face you; click to bring it to the front, or Shift-click to group apps together. The side can always show, or slide out only when the pointer reaches the screen's edge. Turning Stage Manager off or quitting Atrium puts every window back.

## Getting started

1. Download `Atrium-<version>-win-x64.zip` from [Releases](https://github.com/wynter-yaxley/atrium-desktop/releases/latest) and unzip it anywhere.
2. Run `Atrium.exe`. The glass taskbar appears at the bottom of the screen; on first run it imports the apps pinned to the Windows taskbar.
3. Double-click Atrium's tray icon, or right-click it and choose 外观设置… (Appearance settings…), to open the settings: material, size, pinned apps, notifications, widgets and the three modules' switches. Check for updates on the 关于 (About) page.
4. Right-click the tray icon and choose 退出 Atrium (Quit Atrium) to exit completely. Settings are kept in `%LOCALAPPDATA%\Atrium`.

Requirements: Windows 10 19041 or later; Windows 11 recommended. The interface is in Simplified Chinese.

## Status

| | |
| :--- | :--- |
| Ready for daily use | Taskbar, start menu, control centre, calendar, tray, notifications, desktop widgets, Stage Manager, settings |
| Experimental | Windows notifications shown as Atrium cards; WeChat and Feishu messages |
| Not yet verified | Multiple displays, HDR, high refresh rates, sleep and wake, stable releases of Windows 11 |
| Known limits | Recorders that use DXGI desktop duplication do not capture Atrium; on laptops with two GPUs, Atrium needs one restart after its first run |

## Building

Needs the .NET 10 SDK and Visual Studio or the Build Tools with the C++ workload and the Windows SDK (CMake 3.25+).

```powershell
./tools/build.ps1          # writes dist/Atrium; add -Zip for a package too
```

The core is C++20 (Win32, Direct3D 11, DirectComposition); the settings app is WinUI 3. The design specification is in [docs/DESIGN.md](docs/DESIGN.md).

## Contributing

Design feedback, and reports from other hardware and Windows versions, help the most. See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

[MIT](LICENSE). Visually inspired by Apple's Liquid Glass; this project is not affiliated with Apple or Microsoft.

<sub>The pictures on this page are rendered offline at Atrium's real sizes and material settings; the app icons are original illustrations and the notifications are made up.</sub>
