# LiveDesktop

A proof-of-concept **interactive, animated wallpaper** for Windows 10/11. It renders with the Windows
Visual Layer (`Windows.UI.Composition`) into a window that sits **behind the desktop icons**. Icons,
selection, the desktop right-click menu and drag/drop keep working, because LiveDesktop never sits in
the input path.

| Effect | What it does |
| --- | --- |
| **Aurora** | Rotating gradient sky, waving aurora curtains, and soft orbs that drift and **move away from the cursor**. Clicking empty desktop space sends out ripples. |
| **Fireflies** | Night sky with nebula glows and hundreds of twinkling sparks that **gather toward the cursor**. Clicking empty desktop space gives a spark burst and a ripple. |
| **Ember** | The Aurora scene with a warm palette. |

All motion is driven by composition key-frame and expression animations that run in DWM. The app
only does work when the mouse moves, so its own CPU use stays near zero. We measured about 0.3% while
idle on the desktop.

## Build

Requirements:

- Visual Studio 2022+ or Build Tools with the **x64 C++ workload**. Its bundled CMake and Ninja are used.
- Windows SDK **10.0.22621+**. Its `cppwinrt` headers are used, so no NuGet package is needed.

```cmd
build.cmd            :: Release build -> build\LiveDesktop.exe
build.cmd Debug
```

`build.cmd` uses `vswhere` to find `vcvars64.bat`, then runs `cmake -G Ninja` and builds. You can also
open the folder in Visual Studio (CMake project) or run CMake yourself from a *x64 Native Tools*
prompt.

## Run

```cmd
build\LiveDesktop.exe                 :: start (console shows the log)
build\LiveDesktop.exe --effect 1      :: start with Fireflies
build\LiveDesktop.exe --next          :: switch effect in the running instance
build\LiveDesktop.exe --stop          :: exit the running instance cleanly
build\LiveDesktop.exe --exit-after 30 :: auto-exit after 30 s (testing)
```

- **Tray icon:** left-click switches to the next effect. Right-click opens a menu with *Next effect*,
  *Pause animations* and *Exit*.
- **Ctrl+C**, the tray **Exit** item, or `WM_CLOSE` all exit cleanly. LiveDesktop removes its window
  and re-applies the current wallpaper so the desktop repaints.
- **Log:** `LiveDesktop.log` is written next to the exe (or `%TEMP%` if that folder isn't writable). It
  also goes to the console and `OutputDebugString`. The log records the detected desktop layout, all
  HWNDs, and the child z-order of the parent window.
- **Other flags:** `--no-wallpaper-refresh` skips the wallpaper re-apply on exit. `--layered`
  experimentally makes the host a `WS_EX_LAYERED` child, which some 24H2 builds/drivers may need.

Only one instance runs at a time. Nothing is written to the registry or any persistent setting.

## How it works

### 1. Getting behind the icons (the "WorkerW" trick)

```
SendMessageTimeout(Progman, 0x052C, 0xD, 0x1, SMTO_NORMAL, 1000)   // then (0, 0) as a fallback
```

`0x052C` is an undocumented message. Explorer uses it to prepare the desktop for the animated
wallpaper cross-fade. It makes Explorer split the desktop so a separate **WorkerW** draws the
wallpaper behind the icon layer (`SHELLDLL_DefView` → `SysListView32`). Where that WorkerW ends up
depends on the Windows version. `DesktopHost.cpp` detects both layouts and logs which one it found:

| | Classic (Windows 10, Windows 11 ≤ 23H2) | Windows 11 24H2+ |
| --- | --- | --- |
| `SHELLDLL_DefView` lives in | a **top-level** `WorkerW` (moved out of Progman) | **Progman** (child) |
| Wallpaper `WorkerW` | the **next top-level sibling** after that window | a **child of Progman** |
| How we find it | `EnumWindows` → window containing `SHELLDLL_DefView` → `FindWindowEx(NULL, thatWindow, "WorkerW", NULL)` | `FindWindowEx(progman, NULL, "WorkerW", NULL)` |
| Our host's parent | that WorkerW | **Progman** |
| Z-order | top of the WorkerW | **just below `SHELLDLL_DefView`, above the WorkerW** |

The host window is created with `WS_EX_NOREDIRECTIONBITMAP`, then switched to
`WS_CHILD | WS_VISIBLE` (no caption) and parented with `SetParent`. It is then sized to the
**virtual screen** (`SM_XVIRTUALSCREEN`… mapped into the parent's client coordinates), so it spans
every monitor. It returns `HTTRANSPARENT` and is `WS_DISABLED`, so even when it is hit-tested it never
takes input. The verified 24H2 z-order looks like this in the log:

```
Detected desktop layout: 24H2+ (WorkerW + SHELLDLL_DefView are children of Progman)
  Children of 0x...1035E (Progman), top to bottom:
    [0] 0x...1036A SHELLDLL_DefView
    [1] 0x...F7018A LiveDesktop.Host  <-- LiveDesktop host
    [2] 0x...B30940 WorkerW
```

### 2. Rendering: Windows.UI.Composition via C++/WinRT

`Scene.cpp` sets up composition on the UI thread:

1. `CreateDispatcherQueueController` (`DQTYPE_THREAD_CURRENT`, STA).
2. A `Compositor`.
3. `ICompositorDesktopInterop::CreateDesktopWindowTarget(hwnd, FALSE)` on the host HWND.

Each effect (`Effects.cpp`) builds a visual tree under one container. It uses:

- `CompositionLinearGradientBrush` / `CompositionRadialGradientBrush` with animated stop colors,
  offsets and endpoints.
- `SpriteVisual`s moved by `Vector3KeyFrameAnimation` loops.
- `ExpressionAnimation`s that read a shared **pointer `CompositionPropertySet`** (`Position`,
  `Strength`, `Radius`). This is how orbs move away from the cursor and sparks gather toward it, with
  no per-frame CPU work.

Switching effects cross-fades the old and new trees. Pausing sets `Compositor.GlobalPlaybackRate` to
its minimum.

No Win2D or other dependency is used. Blur is faked with radial gradients that fade to transparent,
which is cheaper than a `GaussianBlurEffect` anyway.

### 3. Interactivity without touching input

The icon layer (`SysListView32`) gets all desktop mouse input, and it has to keep getting it.
LiveDesktop installs a **`WH_MOUSE_LL` hook on a dedicated thread**. The hook:

- only records the cursor position and posts a coalesced message to the UI thread;
- **always calls `CallNextHookEx`** and never swallows input;
- runs on its own thread, so slow UI-thread work (COM calls into Explorer, composition) can never hit
  the low-level hook timeout or make the system mouse lag.

The UI thread then checks `GetAncestor(WindowFromPoint(pt), GA_ROOT)`. It reacts only when that is
`Progman`/`WorkerW`, which means the cursor is over the desktop and not over an app window.

On a click, ripples fire only over **empty** desktop space. The icon hit test uses the documented
shell automation chain: `IShellWindows::FindWindowSW(SWC_DESKTOP)` → `IShellBrowser` →
`IFolderView::GetItemPosition`.

### 4. Robustness

- **Explorer restart:** LiveDesktop handles the `RegisterWindowMessage(L"TaskbarCreated")` broadcast by
  re-adding the tray icon and re-attaching. A 2 s health timer also checks that the parent, DefView
  and host still exist and that the z-order is still correct, and re-attaches if not (it retries
  every second until the desktop is back).
- **Display changes:** `WM_DISPLAYCHANGE`, `WM_DPICHANGED` and `WM_SETTINGCHANGE` are debounced. A
  display signature (virtual screen + monitor rects + DPI) decides whether to resize the host and
  rebuild the scene per monitor. The manifest declares **PerMonitorV2** DPI awareness.
- **Fullscreen apps:** `SHQueryUserNotificationState` (busy / D3D fullscreen / presentation mode) or a
  foreground window that covers its monitor pauses all animations. The log line says which one
  triggered it.
- **Clean exit:** LiveDesktop:
  1. unhooks the mouse;
  2. closes the composition target;
  3. destroys the host window;
  4. removes the tray icon;
  5. re-applies the current wallpaper with `SPI_GETDESKWALLPAPER` → `SPI_SETDESKWALLPAPER(path,
     SPIF_SENDCHANGE)`.

  Step 5 does not use `SPIF_UPDATEINIFILE`, so nothing is persisted.

## Limitations

- **Undocumented.** Both `0x052C` and the WorkerW/Progman window structure are Explorer internals.
  They changed in 24H2 and may change again. Both known layouts are handled and the log says which
  one was used.
- **Input is observed, not owned.** The desktop never sends mouse messages to our window, which is the
  whole point. Effects can only react to the *global* cursor position and button presses that we
  observe through the hook. Hover and drag feedback is approximate: for example, a click that starts
  an icon drag is filtered out only by the icon hit test.
- **Cross-process parenting.** Making our window a child of an Explorer window attaches the two
  threads' input queues. The UI thread must keep pumping messages, and this code never blocks it for
  long. If Explorer crashes, the child window is destroyed with it, and the health timer re-attaches.
- **Low-level hook.** Some security tools flag global hooks. A Raw Input (`RIDEV_INPUTSINK`) variant
  would also work.
- **Wallpaper restore.** Re-applying the wallpaper on exit assumes a single image. With a slideshow,
  Spotlight or a solid color, `SPI_GETDESKWALLPAPER` may return a different or empty path. In that
  case LiveDesktop skips the re-apply and relies on the desktop repainting by itself.
- **GPU.** The animations are cheap, but they keep DWM composing a full-screen surface on every
  frame.
- **Not a product.** This is a proof of concept: there is no installer, no autostart and no settings
  UI.

## Layout

```
CMakeLists.txt          C++20, static CRT, links windowsapp + CoreMessaging
build.cmd               vswhere -> vcvars64 -> cmake/ninja
src/main.cpp            controller window, tray, hook thread, timers, lifecycle
src/DesktopHost.*       WorkerW / 24H2 discovery, parenting, z-order, bounds
src/Scene.*             compositor, DesktopWindowTarget, pointer property set, pause, effect switching
src/Effects.cpp         Aurora / Fireflies / Ember
src/DesktopIcons.*      IFolderView-based icon hit test
src/AppIcon.cpp         tray icon generated at runtime
src/Log.*               file + console logging
src/LiveDesktop.manifest  PerMonitorV2, Windows 10/11 supportedOS
```
