#include "UpdateCheckPresentation.h"

#include <cassert>

int RunUpdateCheckPresentationTests() {
    assert(ClassifyUpdateCheckResult("upToDate") == UpdateCheckResult::UpToDate);
    assert(ClassifyUpdateCheckResult("") == UpdateCheckResult::TimedOut);
    assert(ClassifyUpdateCheckResult("checking") == UpdateCheckResult::TimedOut);
    assert(ClassifyUpdateCheckResult("failed") == UpdateCheckResult::Failed);
    assert(ClassifyUpdateCheckResult("available") == UpdateCheckResult::Available);
    assert(ClassifyUpdateCheckResult("downloading") == UpdateCheckResult::Available);
    assert(ClassifyUpdateCheckResult("ready") == UpdateCheckResult::Available);
    assert(ClassifyUpdateCheckResult("applying") == UpdateCheckResult::Available);
    UpdateCheckPresentation presentation;
    auto action = presentation.OnWindowShown();
    assert(action.requestCheck);
    assert(action.showOverlay);
    assert(presentation.OverlayPending());

    action = presentation.OnWindowShown();
    assert(!action.requestCheck);
    assert(!action.showOverlay);

    assert(presentation.OnCheckCompleted());
    assert(!presentation.OverlayPending());
    assert(!presentation.OnTimeout());

    presentation.OnWindowHidden();
    action = presentation.OnWindowShown();
    assert(action.requestCheck);
    assert(!action.showOverlay);
    assert(!presentation.OnCheckCompleted());

    presentation.OnWindowHidden();
    action = presentation.OnWindowShown();
    assert(action.requestCheck);
    assert(!action.showOverlay);
    presentation.OnWindowHidden();
    return 0;
}
