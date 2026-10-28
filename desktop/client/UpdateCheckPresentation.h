#pragma once
#include <string_view>

enum class UpdateCheckResult { TimedOut, UpToDate, Available, Failed };
UpdateCheckResult ClassifyUpdateCheckResult(std::string_view phase);

struct UpdateCheckPresentationAction {
    bool requestCheck = false;
    bool showOverlay = false;
};

class UpdateCheckPresentation final {
public:
    UpdateCheckPresentationAction OnWindowShown();
    void OnWindowHidden();
    bool OnCheckCompleted();
    bool OnTimeout();
    bool OverlayPending() const;

private:
    bool everShown_ = false;
    bool currentlyShown_ = false;
    bool overlayPending_ = false;
};
