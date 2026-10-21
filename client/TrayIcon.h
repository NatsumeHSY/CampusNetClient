#pragma once

#include <functional>

#include <windows.h>
#include <shellapi.h>

enum class TrayCommand {
    Open,
    Login,
    Refresh,
    Exit,
};

class TrayIcon {
public:
    using Callback = std::function<void(TrayCommand)>;

    TrayIcon(HWND owner, Callback callback);
    ~TrayIcon();

    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    bool Initialize();

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message,
                                       WPARAM wParam, LPARAM lParam);
    void ShowMenu();
    void Remove();

    HWND owner_;
    HWND messageWindow_{nullptr};
    Callback callback_;
    NOTIFYICONDATAW icon_{};
};
