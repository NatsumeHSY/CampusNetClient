#include "UpdateCheckPresentation.h"

UpdateCheckResult ClassifyUpdateCheckResult(std::string_view phase) {
    if (phase == "upToDate") return UpdateCheckResult::UpToDate;
    if (phase == "available" || phase == "downloading" || phase == "ready" || phase == "applying")
        return UpdateCheckResult::Available;
    if (phase == "failed") return UpdateCheckResult::Failed;
    return UpdateCheckResult::TimedOut;
}

UpdateCheckPresentationAction UpdateCheckPresentation::OnWindowShown() {
    if (currentlyShown_) {
        return {};
    }
    currentlyShown_ = true;
    const bool firstPresentation = !everShown_;
    everShown_ = true;
    if (firstPresentation) {
        overlayPending_ = true;
    }
    return {true, firstPresentation};
}

void UpdateCheckPresentation::OnWindowHidden() { currentlyShown_ = false; }

bool UpdateCheckPresentation::OnCheckCompleted() {
    const bool wasPending = overlayPending_;
    overlayPending_ = false;
    return wasPending;
}

bool UpdateCheckPresentation::OnTimeout() { return OnCheckCompleted(); }

bool UpdateCheckPresentation::OverlayPending() const { return overlayPending_; }
