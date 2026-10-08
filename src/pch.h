#pragma once

// Win32 / Shell
#include <windows.h>
#include <windowsx.h>
#include <unknwn.h>   // must precede winrt/base.h so classic COM interfaces work with winrt::com_ptr/as<>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlguid.h>
#include <exdisp.h>
#include <dwmapi.h>

// Composition interop (ABI)
#include <DispatcherQueue.h>
#include <windows.ui.composition.interop.h>

// C++/WinRT
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>

// STL
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <vector>
