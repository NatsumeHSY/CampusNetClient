#include "UpdateCheckPresentation.h"

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
