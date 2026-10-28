#pragma once

#include <chrono>

std::chrono::milliseconds NormalUpdateCheckInterval() noexcept;
bool ShouldBeginUpdateCheck(bool hasLoggedOnInteractiveUser) noexcept;
