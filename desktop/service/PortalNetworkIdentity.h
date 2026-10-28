#pragma once

#include <string>
#include <string_view>
#include <vector>

struct PortalAdapterInfo {
    std::wstring name;
    std::wstring description;
    bool up{false};
    std::wstring ipv4;
    std::wstring mac;
};

struct PortalNetworkIdentity {
    std::wstring ipv4;
    std::wstring mac;
};

PortalNetworkIdentity SelectPortalNetworkIdentity(
    const std::vector<PortalAdapterInfo>& adapters);
std::wstring BuildPortalQueryString(const PortalNetworkIdentity& identity);
