#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <unknwn.h>

// Disable the Win32 macro before any C++/WinRT XAML headers are parsed.
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Windowing.h>

// Win32 exposes a GetCurrentTime macro that collides with the WinUI XAML
// animation API's Storyboard::GetCurrentTime method.
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

// The WinUI XAML compiler emits XamlTypeInfo.g.cpp, which requires the
// implementation header for local XAML types to be visible through the PCH.
#include "MainWindow.xaml.h"

// Keep the macro disabled after generated WinUI headers are included too.
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif
