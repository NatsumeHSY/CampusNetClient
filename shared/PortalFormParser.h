#pragma once

#include "CredentialStore.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct PortalField {
    std::wstring name;
    std::wstring value;
    bool disabled{false};
};

struct PortalForm {
    std::wstring action;
    std::vector<PortalField> fields;
};

std::optional<PortalForm> ParseLoginForm(std::wstring_view html);
std::wstring ResolveUrl(std::wstring_view baseUrl, std::wstring_view reference);
std::wstring BuildLoginBody(const PortalForm& form,
                            const Credentials& credentials);
std::wstring BuildMobileLoginBody(const PortalForm& form,
                                 const Credentials& credentials);
std::wstring MobileLoginAction(const PortalForm& form, std::wstring_view pageUrl);
