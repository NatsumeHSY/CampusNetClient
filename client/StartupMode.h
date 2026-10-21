#pragma once

#include <string_view>

enum class ClientStartupMode {
    Interactive,
    Background,
};

ClientStartupMode ParseStartupMode(std::wstring_view value);

