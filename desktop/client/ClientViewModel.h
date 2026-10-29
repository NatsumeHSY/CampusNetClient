#pragma once

#include "Config.h"

#include <optional>
#include <string>

class ClientViewModel {
public:
    explicit ClientViewModel(AppConfig config);

    bool CanRequestLogin(std::optional<bool> internetReachable,
                         bool loginSucceeded,
                         bool loginInProgress = false) const;

    const AppConfig& Config() const;

private:
    AppConfig config_;
};
