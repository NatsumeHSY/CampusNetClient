#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstdint>
#include <mutex>
#include <string>

struct UpdateWindowState {
    std::wstring localVersion = L"Unknown";
    std::wstring targetVersion = L"Unknown";
    std::wstring channel = L"portable";
    std::wstring releaseNotes;
    std::wstring status = L"Checking for updates...";
    std::wstring error;
    std::uint64_t receivedBytes = 0;
    std::uint64_t totalBytes = 0;
    std::uint64_t bytesPerSecond = 0;
    bool requiresUacRestart = false;
};

class UpdaterWindow final {
public:
    explicit UpdaterWindow(UpdateWindowState initialState = {});

    int Run(HINSTANCE instance);
    void SetState(UpdateWindowState state);

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message,
                                       WPARAM wParam, LPARAM lParam);
    bool CreateControls();
    void Layout(int width, int height);
    void Render();

    HWND window_ = nullptr;
    HWND localVersion_ = nullptr;
    HWND targetVersion_ = nullptr;
    HWND channel_ = nullptr;
    HWND status_ = nullptr;
    HWND releaseNotes_ = nullptr;
    HWND progress_ = nullptr;
    HWND progressText_ = nullptr;
    HWND speedText_ = nullptr;
    HWND errorText_ = nullptr;
    HWND closeButton_ = nullptr;
    mutable std::mutex stateMutex_;
    UpdateWindowState state_;
};
