#include "UpdateCheckPresentation.h"

#include <cassert>

int RunUpdateCheckPresentationTests() {
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
