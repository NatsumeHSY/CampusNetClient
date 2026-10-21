#include "pch.h"
#include "TrayIcon.h"
#include "resource.h"

#include <shellapi.h>

#include <string>

#pragma comment(lib, "Shell32.lib")

namespace {

constexpr UINT kTrayMessage = WM_APP + 42;
constexpr UINT kOpenCommand = 1001;
constexpr UINT kLoginCommand = 1002;
constexpr UINT kRefreshCommand = 1003;
constexpr UINT kExitCommand = 1004;

const wchar_t kWindowClass[] = L"CampusNetClient.TrayWindow";
const wchar_t kApplicationName[] = L"广航海校园网自动登录";

HICON LoadCampusNetIcon(int width, int height) {
    auto* module = GetModuleHandleW(nullptr);
    auto icon = reinterpret_cast<HICON>(LoadImageW(
        module, MAKEINTRESOURCEW(IDI_CAMPUSNETCLIENT), IMAGE_ICON, width,
        height, LR_DEFAULTCOLOR | LR_SHARED));
    return icon != nullptr ? icon : LoadIconW(nullptr, IDI_APPLICATION);
}

} // namespace

TrayIcon::TrayIcon(HWND owner, Callback callback)
    : owner_(owner), callback_(std::move(callback)) {}

TrayIcon::~TrayIcon() { Remove(); }

bool TrayIcon::Initialize() {
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = &TrayIcon::WindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = kWindowClass;
    RegisterClassW(&windowClass);

    messageWindow_ = CreateWindowExW(
        0, kWindowClass, kApplicationName, 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), this);
    if (messageWindow_ == nullptr) {
        return false;
    }

    icon_.cbSize = sizeof(icon_);
    icon_.hWnd = messageWindow_;
    icon_.uID = 1;
    icon_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    icon_.uCallbackMessage = kTrayMessage;
    icon_.hIcon = LoadCampusNetIcon(GetSystemMetrics(SM_CXSMICON),
                                    GetSystemMetrics(SM_CYSMICON));
    wcscpy_s(icon_.szTip, kApplicationName);
    return Shell_NotifyIconW(NIM_ADD, &icon_) == TRUE;
}

LRESULT CALLBACK TrayIcon::WindowProc(HWND window, UINT message, WPARAM wParam,
                                      LPARAM lParam) {
    auto* tray = reinterpret_cast<TrayIcon*>(GetWindowLongPtrW(
        window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        tray = static_cast<TrayIcon*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(tray));
    }
    if (tray != nullptr && message == kTrayMessage) {
        if (lParam == WM_LBUTTONUP) {
            tray->callback_(TrayCommand::Open);
        } else if (lParam == WM_RBUTTONUP) {
            tray->ShowMenu();
        }
        return 0;
    }
    if (tray != nullptr && message == WM_COMMAND) {
        switch (LOWORD(wParam)) {
        case kOpenCommand:
            tray->callback_(TrayCommand::Open);
            break;
        case kLoginCommand:
            tray->callback_(TrayCommand::Login);
            break;
        case kRefreshCommand:
            tray->callback_(TrayCommand::Refresh);
            break;
        case kExitCommand:
            tray->callback_(TrayCommand::Exit);
            break;
        default:
            break;
        }
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void TrayIcon::ShowMenu() {
    POINT point{};
    GetCursorPos(&point);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kOpenCommand, L"\u6253\u5F00\u8BBE\u7F6E");
    AppendMenuW(menu, MF_STRING, kLoginCommand, L"\u7ACB\u5373\u767B\u5F55");
    AppendMenuW(menu, MF_STRING, kRefreshCommand, L"\u5237\u65B0\u72B6\u6001");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kExitCommand, L"\u9000\u51FA");
    SetForegroundWindow(messageWindow_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, point.x, point.y, 0, messageWindow_,
                   nullptr);
    DestroyMenu(menu);
}

void TrayIcon::Remove() {
    if (messageWindow_ != nullptr) {
        Shell_NotifyIconW(NIM_DELETE, &icon_);
        DestroyWindow(messageWindow_);
        messageWindow_ = nullptr;
    }
}
