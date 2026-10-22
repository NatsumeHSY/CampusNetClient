#include "UpdaterRole.h"

#include <cassert>

int RunUpdaterRoleTests() {
    assert(ResolveUpdaterRole(L"") == UpdaterRole::Gui);
    assert(ResolveUpdaterRole(L"agent") == UpdaterRole::Agent);
    assert(ResolveUpdaterRole(L"apply") == UpdaterRole::Apply);
    assert(ResolveUpdaterRole(L"AGENT") == UpdaterRole::Invalid);
    assert(ResolveUpdaterRole(L"arbitrary") == UpdaterRole::Invalid);
    return 0;
}
