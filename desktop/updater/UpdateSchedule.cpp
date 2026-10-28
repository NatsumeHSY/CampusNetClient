#include "UpdateSchedule.h"

std::chrono::milliseconds NormalUpdateCheckInterval() noexcept {
    return std::chrono::minutes(10);
}

bool ShouldBeginUpdateCheck(bool hasLoggedOnInteractiveUser) noexcept {
    return hasLoggedOnInteractiveUser;
}
