#include "../updater/UpdateSchedule.h"

#include <cassert>
#include <chrono>

int RunUpdateScheduleTests() {
    assert(NormalUpdateCheckInterval() == std::chrono::minutes(10));
    assert(!ShouldBeginUpdateCheck(false));
    assert(ShouldBeginUpdateCheck(true));
    return 0;
}
