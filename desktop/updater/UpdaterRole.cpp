#include "UpdaterRole.h"

UpdaterRole ResolveUpdaterRole(std::wstring_view inheritedRole) {
    if (inheritedRole.empty()) {
        return UpdaterRole::Gui;
    }
    if (inheritedRole == L"agent") {
        return UpdaterRole::Agent;
    }
    if (inheritedRole == L"apply") {
        return UpdaterRole::Apply;
    }
    return UpdaterRole::Invalid;
}
