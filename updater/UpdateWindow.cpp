#include "UpdateWindow.h"

#include "resource.h"

#include <CommCtrl.h>

#include <algorithm>
#include <string>
#include <utility>

namespace {

constexpr wchar_t kWindowClass[] = L"CampusNetUpdaterWindow";
constexpr UINT kStateChangedMessage = WM_APP + 1;
constexpr int kMargin = 22;
constexpr int kLabelHeight = 24;
constexpr int kButtonWidth = 108;
constexpr int kButtonHeight = 32;

std::wstring FormatByteCount(std::uint64_t value) {
    return std::to_wstring(value) + L" bytes";
}

void SetControlFont(HWND control) {
    if (control != nullptr) {
        SendMessageW(control, WM_SETFONT,
                     reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),
                     TRUE);
    }
}

HWND CreateChild(HWND parent, const wchar_t* className,
                 const wchar_t* text, DWORD style, int controlId) {
    HWND control = CreateWindowExW(
        0, className, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0,
        parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)),
        GetModuleHandleW(nullptr), nullptr);
    SetControlFont(control);
    return control;
}

} // namespace

UpdaterWindow::UpdaterWindow(UpdateWindowState initialState)
    : state_(std::move(initialState)) {}

int UpdaterWindow::Run(HINSTANCE instance) {
    INITCOMMONCONTROLSEX commonControls{sizeof(commonControls), ICC_PROGRESS_CLASS};
    if (!InitCommonControlsEx(&commonControls)) {
        return static_cast<int>(GetLastError());
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = &UpdaterWindow::WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hIconSm = windowClass.hIcon;
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = kWindowClass;
    if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return static_cast<int>(GetLastError());
    }

    HWND created = CreateWindowExW(
        0, kWindowClass, L"CampusNet 自动更新", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 760, 560, nullptr, nullptr, instance,
        this);
    if (created == nullptr) {
        return static_cast<int>(GetLastError());
    }

    ShowWindow(created, SW_SHOWNORMAL);
    UpdateWindow(created);
    MSG message{};
    BOOL result = 0;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (result < 0) {
        return static_cast<int>(GetLastError());
    }
    return static_cast<int>(message.wParam);
}

void UpdaterWindow::SetState(UpdateWindowState state) {
    HWND target = nullptr;
    {
        std::lock_guard lock(stateMutex_);
        state_ = std::move(state);
        target = window_;
    }
    if (target != nullptr) {
        PostMessageW(target, kStateChangedMessage, 0, 0);
    }
}

LRESULT CALLBACK UpdaterWindow::WindowProc(HWND window, UINT message,
                                          WPARAM wParam, LPARAM lParam) {
    UpdaterWindow* self = reinterpret_cast<UpdaterWindow*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<UpdaterWindow*>(create->lpCreateParams);
        if (self == nullptr) {
            return FALSE;
        }
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(self));
        {
            std::lock_guard lock(self->stateMutex_);
            self->window_ = window;
        }
    }
    if (self == nullptr) {
        return DefWindowProcW(window, message, wParam, lParam);
    }

    switch (message) {
    case WM_CREATE:
        if (!self->CreateControls()) {
            return -1;
        }
        self->Render();
        return 0;
    case WM_SIZE:
        self->Layout(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case kStateChangedMessage:
        self->Render();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_UPDATER_CLOSE) {
            DestroyWindow(window);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_NCDESTROY:
        {
            std::lock_guard lock(self->stateMutex_);
            if (self->window_ == window) {
                self->window_ = nullptr;
            }
        }
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool UpdaterWindow::CreateControls() {
    localVersion_ = CreateChild(window_, L"STATIC", L"Local version:", SS_LEFT, 0);
    targetVersion_ = CreateChild(window_, L"STATIC", L"Target version:", SS_LEFT, 0);
    channel_ = CreateChild(window_, L"STATIC", L"Channel:", SS_LEFT, 0);
    status_ = CreateChild(window_, L"STATIC", L"Status:", SS_LEFT, 0);
    releaseNotes_ = CreateChild(
        window_, L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY |
                               WS_VSCROLL | WS_BORDER | ES_LEFT, 0);
    progress_ = CreateChild(window_, PROGRESS_CLASSW, L"",
                            PBS_SMOOTH | WS_BORDER, IDC_UPDATER_PROGRESS);
    progressText_ = CreateChild(window_, L"STATIC", L"0 bytes / 0 bytes",
                                SS_LEFT, 0);
    speedText_ = CreateChild(window_, L"STATIC", L"0 bytes/s", SS_LEFT, 0);
    errorText_ = CreateChild(window_, L"STATIC", L"", SS_LEFT, 0);
    closeButton_ = CreateChild(window_, L"BUTTON", L"关闭",
                               BS_DEFPUSHBUTTON, IDC_UPDATER_CLOSE);
    return localVersion_ != nullptr && targetVersion_ != nullptr &&
           channel_ != nullptr && status_ != nullptr && releaseNotes_ != nullptr &&
           progress_ != nullptr && progressText_ != nullptr && speedText_ != nullptr &&
           errorText_ != nullptr && closeButton_ != nullptr;
}

void UpdaterWindow::Layout(int width, int height) {
    const int contentWidth = std::max(0, width - kMargin * 2);
    int y = kMargin;
    MoveWindow(localVersion_, kMargin, y, contentWidth, kLabelHeight, TRUE);
    y += kLabelHeight + 5;
    MoveWindow(targetVersion_, kMargin, y, contentWidth, kLabelHeight, TRUE);
    y += kLabelHeight + 5;
    MoveWindow(channel_, kMargin, y, contentWidth, kLabelHeight, TRUE);
    y += kLabelHeight + 5;
    MoveWindow(status_, kMargin, y, contentWidth, kLabelHeight, TRUE);
    y += kLabelHeight + 8;

    const int bottomArea = 4 * kLabelHeight + kButtonHeight + 5 * kMargin;
    const int notesHeight = std::max(80, height - y - bottomArea);
    MoveWindow(releaseNotes_, kMargin, y, contentWidth, notesHeight, TRUE);
    y += notesHeight + kMargin;
    MoveWindow(progress_, kMargin, y, contentWidth, 22, TRUE);
    y += 28;
    MoveWindow(progressText_, kMargin, y, contentWidth, kLabelHeight, TRUE);
    y += kLabelHeight;
    MoveWindow(speedText_, kMargin, y, contentWidth, kLabelHeight, TRUE);
    y += kLabelHeight;
    MoveWindow(errorText_, kMargin, y, contentWidth, kLabelHeight, TRUE);
    MoveWindow(closeButton_, width - kMargin - kButtonWidth,
               height - kMargin - kButtonHeight, kButtonWidth, kButtonHeight,
               TRUE);
}

void UpdaterWindow::Render() {
    UpdateWindowState snapshot;
    {
        std::lock_guard lock(stateMutex_);
        snapshot = state_;
    }

    std::wstring local = L"Current version: " + snapshot.localVersion;
    std::wstring target = L"Target version: " + snapshot.targetVersion;
    std::wstring channel = L"Channel: " + snapshot.channel;
    if (snapshot.requiresUacRestart) {
        snapshot.status += L" (relaunch may require UAC approval)";
    }
    const std::wstring progress = L"Downloaded " +
                                  FormatByteCount(snapshot.receivedBytes) + L" / " +
                                  FormatByteCount(snapshot.totalBytes);
    const std::wstring speed = L"Speed: " +
                               FormatByteCount(snapshot.bytesPerSecond) + L"/s";

    SetWindowTextW(localVersion_, local.c_str());
    SetWindowTextW(targetVersion_, target.c_str());
    SetWindowTextW(channel_, channel.c_str());
    SetWindowTextW(status_, snapshot.status.c_str());
    SetWindowTextW(releaseNotes_, snapshot.releaseNotes.c_str());
    SetWindowTextW(progressText_, progress.c_str());
    SetWindowTextW(speedText_, speed.c_str());
    SetWindowTextW(errorText_, snapshot.error.c_str());

    int position = 0;
    if (snapshot.totalBytes != 0) {
        const auto cappedReceived = std::min(snapshot.receivedBytes,
                                             snapshot.totalBytes);
        position = static_cast<int>((cappedReceived * 1000) /
                                    snapshot.totalBytes);
    }
    SendMessageW(progress_, PBM_SETRANGE32, 0, 1000);
    SendMessageW(progress_, PBM_SETPOS, position, 0);
}
