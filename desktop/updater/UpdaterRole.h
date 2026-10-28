#pragma once

#include <string_view>

enum class UpdaterRole {
    Gui,
    Agent,
    Apply,
    Invalid,
};

UpdaterRole ResolveUpdaterRole(std::wstring_view inheritedRole);
