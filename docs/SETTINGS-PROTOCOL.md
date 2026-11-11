# SETTINGS-PROTOCOL · 设置协议

`Atrium.Settings.exe`（C# / WinUI 3，按需启动）通过命名管道调整 `Atrium.exe` 的外观（D18）。两边共享同一份设置模型：`native/core/settings.h` 与 `settings/Atrium.Settings/BarSettings.cs`，键名、范围、预设必须同步修改。

## 设置文件

`%LOCALAPPDATA%\Atrium\settings.json`，UTF-8，一个扁平 JSON 对象，原子替换写入。缺少的键取默认值，超出范围的值被截断，格式错误的文件整体回退到默认值（Core 日志会记录）。

| 键 | 类型 | 范围 | 默认（液态，参照 macOS Liquid Glass） | 说明 |
| --- | --- | --- | --- | --- |
| `version` | number | 1–2 | 2 | 架构版本。2（v0.6）：读取版本 1 的文件时忽略其中的 `notifyListener`（版本 1 保存的是旧默认值“关”），即迁移为开启；没有 `version` 的输入（管道的部分更新）不迁移 |
| `preset` | string | `liquid` / `frosted` / `custom` | `liquid` | 仅用于界面显示 |
| `transparency` | number | 0–1 | 0.95 | 1 = 完全不染色；着色混合 = 1 − transparency |
| `blur` | number | 0–1 | 0.50 | 模糊强度：主要决定 mip 级别（1 + 4.5 × blur），低值时与清晰采样混合 |
| `refraction` | number | 0–60 DIP | 30 | 边缘最大采样偏移 |
| `edge` | number | 1–80 DIP | 24 | 发生折射的边缘宽度 |
| `dispersion` | number | 0–0.5 | 0.30 | 通道分离比例 |
| `highlight` | number | 0–1 | 0.80 | 边缘高光与玻璃下内容的鲜艳度 |
| `shadow` | number | 0–1 | 0.30 | 投影浓度 |
| `tint` | string | `#RRGGBB` | `#FFFFFF` | 染色 |
| `cornerRadius` | number | 0–200 DIP | 30 | 绘制时不超过高度的一半 |
| `width` | number | 200–4000 DIP | 1200 | 最大宽度，不超过工作区；`autoWidth` 关闭时为固定宽度 |
| `height` | number | 32–160 DIP | 60 | |
| `margin` | number | 0–400 DIP | 10 | 与工作区底边的距离 |
| `showClock` | bool | | true | 时钟与日期 |
| `showTray` | bool | | true | 系统托盘入口 |
| `startTray` | bool | | true | 开始菜单底部胶囊里的托盘按钮 |
| `startStatus` | bool | | true | 开始菜单底部胶囊里的网络、音量和电池按钮 |
| `startSize` | string | `small` / `medium` / `large` | 无 | 开始菜单的大小（设置中心 › 任务栏 › 开始菜单，或在菜单里拖握把、右键选择）。没有这一项时沿用 `start.json` 里的大小，Core 启动时把它写进来 |
| `showStatus` | bool | | true | 状态环与控制中心入口 |
| `showNotifications` | bool | | true | 通知入口；关闭入口不关闭通知来源 |
| `textColor` | string | `auto` / `light` / `dark` | `auto` | 文字颜色；`auto` 按玻璃下方的实际亮度选择 |
| `autoHide` | string | `never` / `fullscreen` / `always` | `fullscreen` | 栏的显示方式：常驻；全屏时隐藏（最大化或全屏的应用在前面时滑出屏幕）；一直隐藏。隐藏时，指针移到栏下方的屏幕底边栏就出现，离开后收回 |
| `statusStyle` | string | `ring` / `icons` | `ring` | 状态栏的样子：Wi-Fi 外面一圈电量或音量的圆环；或者像 Windows 任务栏那样并排的网络、音量、电池三个图标（自绘，跟随字色）。点按都打开控制中心 |
| `iconSize` | number | 16–64 DIP | 30 | 任务栏图标边长 |
| `magnify` | number | 1–2 | 1.7 | 悬停放大：指针下图标的缩放倍数，相邻图标按波形放大；1 为关闭 |
| `hoverRise` | bool | | false | 形状页的“跟随鼠标拱起”：背景随鼠标局部向上延伸，幅度复用 `magnify`；关闭后保持图标放大。默认关闭，旧配置没有这一项时也是关闭 |
| `autoWidth` | bool | | true | 宽度随内容伸缩，最大为 `width` |
| `showRunning` | bool | | true | 显示未固定但正在运行的应用 |
| `dockDisplayMode` | string | `primary` / `follow` / `fixed` / `all` | `primary` | 仅主屏；单个 Dock 在其他屏幕底边停留后唤出（只有接了两块以上屏幕时才查看指针）；固定屏幕；或每个扩展显示器常驻一条任务栏 |
| `dockDisplayDelay` | number | 100–2000 ms | 450 | 跨屏唤出的连续停留时间；拖动图标、文件或使用弹出面板时不切屏 |
| `dockDisplay` | string | 显示器设备接口 ID，或空字符串 | `""` | `fixed` 模式的目标；空值跟随系统主屏；断开后临时使用主屏，保留 ID 供重连恢复 |
| `desktopDisplay` | string | 显示器设备接口 ID，或空字符串 | `""` | 小组件与台前调度的屏幕，独立于 Dock；空值跟随系统主屏；切换前恢复原屏幕中台前调度收起的窗口 |
| `taskbarApps` | string | `all` / `primaryAndWindow` / `window` | `all` | 运行中的应用分别显示在所有任务栏、系统主屏和窗口所在屏、或仅窗口所在屏；固定应用保留，运行标记和预览只使用符合规则的窗口 |
| `taskbarRightPrimaryOnly` | bool | | false | 仅在系统主屏显示任务栏右侧元素；副屏隐藏托盘、状态圆环、时钟日期、通知入口及分隔线，不保留占位；各元素原有的显示开关保持不变，也适用于跨屏唤出和固定显示器模式 |
| `notifyTimeout` | number | 3–30 s | 6 | 通知弹出后停留的时间（指针停在卡片上时不计时） |
| `notifyPlacement` | string | `dock` / `topRight` | `dock` | 默认在任务栏上方堆叠；右上角模式下，弹窗和铃铛打开的通知中心均固定在系统主屏工作区右上角，新通知从右侧淡入，已有通知平滑下移；通知中心的操作区在列表上方，超出屏幕高度可滚动 |
| `notifyHideBody` | bool | | false | 隐藏通知正文；同时把通知窗口排除在截图和屏幕共享之外 |
| `notifyListener` | bool | | true | 通过 UserNotificationListener 把 Windows 通知显示为 Atrium 卡片（系统横幅仍会出现，除非用户在 Windows 里关闭该应用的横幅） |
| `notifyWeChat` | bool | | true | 实验：读取微信窗口的会话列表（UI 自动化，只读），非免打扰会话有新的未读消息时显示卡片 |
| `notifyFeishu` | bool | | true | 读取飞书自己的消息弹窗里的文字（UI 自动化，只读），同时显示为 Atrium 卡片 |
| `bar` | bool | | true | 玻璃任务栏模块：关闭时栏的窗口隐藏，开始菜单、托盘、控制中心、日历与通知中心随之关闭，通知来源停止接收（Windows 自己的横幅照常出现），`hideTaskbar` 与 `hideBanners` 不再生效；渲染线程继续为小组件与菜单绘制玻璃。与 `widgets`、`stageManager` 互不依赖 |
| `widgets` | bool | | true | 桌面小组件；布局由 Core 存于 `widgets.json`（D35）。中号时钟和中号数字时钟的世界时钟城市可在小组件右键菜单“更改城市…”里逐个换，存为该小组件的 `cities`（城市名数组，按显示顺序；没有这一项时用北京、东京、伦敦、纽约，数字时钟用后三个） |
| `widgetsOnRight` | bool | | false | 小组件逻辑网格从右侧排布；切换时镜像现有列，不改写 `widgets.json` |
| `widgetBackdrop` | string | `desktop` / `wallpaper` | `desktop` | 小组件玻璃的背景：实时抓取桌面窗口，或直接读取壁纸图片（省电）；都不含上层窗口（D38） |
| `widgetMaterial` | string | `glass` / `dusk` / `color` | `glass` | 小组件的材质：任务栏的玻璃（墨色随玻璃变化）；暮色（同一块玻璃压上 22% 的黑，文字与图标全白，单色）；原色（不渲染玻璃，不透明的白底与浅色模式强调色，天气小组件为随天气变化的天空渐变） |
| `stageManager` | bool | | false | 台前调度：前台应用留在屏幕中间，其他应用的窗口先在后台拍下画面，再最小化（不激活），画面斜立在屏幕一侧；打开时桌面小组件排到台前调度的另一侧（Core 从栏上打开时同时写入 `widgetsOnRight`）；关闭或 Core 退出时全部恢复。Core 把收起的窗口记在 `%LOCALAPPDATA%\Atrium\stage.json`（窗口句柄、应用键与分组），异常退出后再次启动时接回仍处于最小化的窗口 |
| `stageOnRight` | bool | | false | 台前调度的窗口放在屏幕右侧；默认左侧 |
| `stageAutoHide` | bool | | false | 悬停弹出：台前调度平时隐藏，指针碰到那一侧的屏幕边缘时才滑出；默认常驻（窗口挡住时让开） |
| `stageMinimize` | bool | | false | 窗口最小化到台前调度：前面的应用的窗口全部最小化后，窗口的画面直接飞进一侧的卡片（不播放系统的最小化动画），点卡片换回前面；默认关闭（最小化的窗口不进台前调度） |
| `hideTaskbar` | bool | | false | Atrium 运行时隐藏系统任务栏（并设为自动隐藏），关闭或退出时恢复（D37）。隐藏期间另有一个看护进程（`Atrium.exe --guard <pid>`）：Atrium 无论怎样结束（崩溃、被结束进程），它都立刻还原任务栏、自动隐藏设置和请勿打扰；Atrium 约 15 秒无响应时先显示任务栏 |
| `hideBanners` | bool | | false | 打开“请勿打扰”（仅闹钟）去掉系统横幅，通知仍显示为 Atrium 卡片；关闭或退出时恢复（D37） |
| `winKeyStart` | bool | | false | 单独按下 Win 键时打开 Atrium 的开始菜单，Windows 的开始菜单不出现。低级键盘钩子在单独的线程上先扣下 Win 键：后面跟了别的键（Win + E 等）就把两个键一起交给 Windows，单独松开则 Windows 始终没有收到它。前台是管理员权限的窗口时（补发的按键送不进去）不拦截。Win + L 由系统在钩子之前处理，不受影响。需要打开 `bar` |
| `winNumbers` | bool | | false | Win + 1…9、0 打开栏上第 1…10 个应用，和点按它一样（没开就启动，开着就切换过去）。与 `winKeyStart` 共用同一个键盘钩子；只开这一项时，单独按 Win 仍交给 Windows。带 Shift、Ctrl、Alt 的组合照常交给 Windows。需要打开 `bar` |
| `startOnLogin` | bool | | true | 当前用户开机自启：任务计划程序里当前用户的登录任务 `Atrium Startup-<SID>`（登录时随会话启动，不在 Explorer 的 Run 项队列里排队）；注册失败时退回 HKCU 的 Run 项。正式安装每次启动时，若此值为开且 Run 项没有在任务管理器里被关闭，就把旧版本的 Run 项换成登录任务；开发构建不修改开机项 |
| `pins` | array | | 见下 | 固定的应用，按顺序显示 |

`pins` 的每一项：`{"name": "显示名", "path": ".lnk / .exe 路径，可空", "args": "附加参数，可空", "aumid": "AppUserModelID，可空", "multipleWindows": false}`。`multipleWindows` 可省略，浏览器与 Windows 文件管理器默认允许新窗口，聊天、音乐及其他应用默认打开已有页面；中键与“打开新窗口”遵循此设置。浏览器仅按常用程序文件名识别（Chrome、Edge、Firefox、Brave、Opera、Vivaldi、Chromium），不扫描协议注册或按显示名推断。主窗口关闭到后台时，通过应用自己的托盘入口恢复，不强制显示隐藏窗口。识别同一应用时 `aumid` 优先，其次匹配启动器与实际程序；多个固定项共用的程序路径不作模糊归组。设置文件里**没有** `pins` 键时（首次运行），Core 导入用户当前 Windows 任务栏的固定项（`%APPDATA%\Microsoft\Internet Explorer\Quick Launch\User Pinned\TaskBar\*.lnk`），`get` 返回的 `current` / `saved` 中包含导入结果；保存后写入文件，之后不再导入。

开始菜单的排列不在设置文件里，而是 Core 自己保存在同一目录的 `start.json`：`{"version":2,"size":"medium","tiles":[{"id":"应用 id","name":"名称"},{"id":"folder:1","name":"文件夹名","apps":[{"id":…,"name":…}]}]}`。`hidden` 是用户隐藏的应用 id（右键“隐藏”）：不出现在网格、文件夹和搜索里，只在“全部应用”末尾的“已隐藏”下面（右键开始菜单空白处“显示隐藏的应用”直接到那里），右键“取消隐藏”或“固定到开始菜单”恢复。`size` 是 `small`（7 × 3 格）、`medium`（9 × 4，默认）或 `large`（11 × 5；版本 1 的三档小一级，读入时换算），由菜单右上角的握把或右键空白处选择；屏幕放不下时用放得下的最大一档。应用 id 是 AppsFolder 里的解析名（AUMID、已知文件夹路径）或快捷方式的完整路径。没有 `tiles` 键时，菜单显示 Windows 开始菜单的固定项；用户第一次拖动、建文件夹、固定或移除时，把当时的固定项复制进来，之后与 Windows 的固定项互不影响；右键空白处“恢复为 Windows 开始菜单的固定项”删去 `tiles`。已卸载的应用不显示。

预设只改材质键（transparency、blur、refraction、edge、dispersion、highlight、shadow、tint），不改布局与内容键。预设由 `BarSettings.cs` 的 `Liquid()` / `Frosted()` 定义，液态默认值与 `native/core/settings.h` 保持一致。

## 管道

- 名称：`\\.\pipe\Atrium.Core.<会话 ID>`；消息模式；拒绝远程客户端；Core 以 `FILE_FLAG_FIRST_PIPE_INSTANCE` 创建，其他进程无法预先占用名称。
- Settings 连接后用 `GetNamedPipeServerProcessId` 核对对端的完整可执行路径，必须对应本分发目录的 Atrium.exe。不同安装位置不共用设置连接；另一实例已运行时明确提示，仍不支持两套任务栏并行。管道名称沿用内部标识 Atrium.Core。
- 同一时间只服务一个客户端。每条请求是一个 UTF-8 JSON 消息，对应一条回复。

| 请求 | 回复 | 行为 |
| --- | --- | --- |
| `{"op":"get"}` | `{"op":"state","protocol":1,"saved":{…},"current":{…},"path":"…"}` | 已保存值与当前生效值（可能含未保存预览） |
| `{"op":"preview","settings":{…}}` | `{"op":"ok"}` | 只改内存，立即重绘；可只发部分键 |
| `{"op":"commit","settings":{…}}` | `{"op":"ok","warning":…}`（warning 可省）或 `{"op":"error","message":…}` | 写文件成功才更新 saved/current；文件已保存但自启修改失败时仍返回 ok，另给 warning，界面接受保存快照并显示警告 |
| `{"op":"cancel"}` | `{"op":"ok"}` | 恢复到已保存值 |
| `{"op":"add-widget","kind":"clock","size":"small"}` | `{"op":"ok"}` 或 `{"op":"error","message":…}` | 添加小组件并保存布局；不改变设置预览租约 |

客户端断开时若当前值与已保存值不同，Core 自动撤销预览（预览租约）。Settings 端拖动滑块时同一时间只有一个预览请求在途，返回后再发送最新状态，不积压。

短请求自进入客户端起有 5 秒总期限，包含等待信号量、写入、刷新和读取。超时会取消本连接、释放排队请求；窗口关闭也取消在途操作。连接中断后的提交不会自动退回离线写入或当作成功；重新连接并读取 state 后核对保存结果。回复上限为 1 MiB。

向 UI 发送 add-widget 内部消息前，把名称解析成 WidgetKind / WidgetSize 两个值类型；SendMessageTimeout 超时后不留下局部字符串指针。

Core 自己也会改设置（栏的右键菜单、控制中心里的开关，例如台前调度），改后同样写入设置文件。Settings 监视这个文件，变化后用 `get` 重新读取：已保存值里变了的键同步到界面，用户在 Settings 里尚未保存的其他修改保留，并重新发送预览。

Core 未运行时，Settings 直接读写设置文件，Core 下次启动生效。

## Core 命令行

| 参数 | 作用 |
| --- | --- |
| （无） | 启动；若已在运行，则打开设置窗口 |
| `--exit` | 请正在运行的实例退出，最多等 10 秒（安装、升级用） |
| `--development` / development.txt | 显式开发身份：不注册正式卸载项、快捷方式或 Windows 自启。本地 CMake / dotnet 构建默认也是开发身份；tools/build.ps1 -Development 输出至 dist/Development/Atrium，正式打包显式关闭开发编译标记 |
| `--ready-event <name>` | 更新启动握手；核心在 UI 消息循环和管道就绪后确认，设置中心在窗口加载后确认；更新保持核心运行时，设置中心还必须成功读取对应核心的 state |
| `--debug-capturable` | 仅开发：不排除捕获并冻结背景，用于截取真实画面 |
