#include "ClientViewModel.h"

#include <utility>

ClientViewModel::ClientViewModel(AppConfig config)
    : config_(std::move(config)) {}



bool ClientViewModel::CanRequestLogin(std::optional<bool> internetReachable,
                                      bool loginSucceeded,
                                      bool loginInProgress) const {
    if (loginSucceeded || loginInProgress) {
        return false;
    }
    if (internetReachable.has_value()) {
        return !*internetReachable;
    }
    return true;
}

const AppConfig& ClientViewModel::Config() const { return config_; }
