#include "PortalNetworkIdentity.h"

#include <algorithm>
#include <cwctype>

namespace {

constexpr std::wstring_view kEthernetAlias = L"\u4EE5\u592A\u7F51";

bool IsApipa(std::wstring_view ipv4) {
    return ipv4.starts_with(L"169.254.");
}

bool StartsWithInsensitive(std::wstring_view value, std::wstring_view prefix) {
    if (value.size() < prefix.size()) {
        return false;
    }
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        if (std::towlower(value[index]) != std::towlower(prefix[index])) {
            return false;
        }
    }
    return true;
}

bool ContainsInsensitive(std::wstring_view value, std::wstring_view part) {
    if (part.empty()) {
        return true;
    }
    return std::search(value.begin(), value.end(), part.begin(), part.end(),
                       [](wchar_t left, wchar_t right) {
                           return std::towlower(left) == std::towlower(right);
                       }) != value.end();
}

bool IsUsableFallbackIpv4(const PortalAdapterInfo& adapter) {
    return !adapter.ipv4.empty() && !IsApipa(adapter.ipv4) &&
           adapter.ipv4 != L"127.0.0.1" &&
           !StartsWithInsensitive(adapter.name, L"vEthernet");
}

} // namespace

PortalNetworkIdentity SelectPortalNetworkIdentity(
    const std::vector<PortalAdapterInfo>& adapters) {
    PortalNetworkIdentity identity;

    for (const auto& adapter : adapters) {
        if (adapter.name == kEthernetAlias && !adapter.ipv4.empty() &&
            !IsApipa(adapter.ipv4)) {
            identity.ipv4 = adapter.ipv4;
            break;
        }
    }
    if (identity.ipv4.empty()) {
        for (const auto& adapter : adapters) {
            if (IsUsableFallbackIpv4(adapter)) {
                identity.ipv4 = adapter.ipv4;
                break;
            }
        }
    }

    for (const auto& adapter : adapters) {
        if (adapter.name == kEthernetAlias && adapter.up &&
            !adapter.mac.empty()) {
            identity.mac = adapter.mac;
            break;
        }
    }
    if (identity.mac.empty()) {
        for (const auto& adapter : adapters) {
            if (adapter.up && !adapter.mac.empty() &&
                !ContainsInsensitive(adapter.description, L"Hyper-V")) {
                identity.mac = adapter.mac;
                break;
            }
        }
    }

    return identity;
}

std::wstring BuildPortalQueryString(const PortalNetworkIdentity& identity) {
    return L"wlanuserip=" + identity.ipv4 +
           L"&wlanacname=HHXY_XXL2F_HUIJU&t=wireless-v2-plain"
           L"&nasip=2.2.2.230&mac=" + identity.mac;
}
