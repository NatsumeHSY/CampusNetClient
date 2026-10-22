#include "PortalFormParser.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <sstream>

namespace {

std::size_t FindInsensitive(std::wstring_view source, std::wstring_view needle,
                            std::size_t start = 0) {
    if (needle.empty() || start > source.size() ||
        needle.size() > source.size() - start) {
        return std::wstring::npos;
    }

    for (std::size_t i = start; i + needle.size() <= source.size(); ++i) {
        bool equal = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (std::towlower(source[i + j]) != std::towlower(needle[j])) {
                equal = false;
                break;
            }
        }
        if (equal) {
            return i;
        }
    }
    return std::wstring::npos;
}

std::wstring Lower(std::wstring_view value) {
    std::wstring result(value);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](wchar_t c) { return std::towlower(c); });
    return result;
}

std::wstring DecodeHtml(std::wstring value) {
    struct Entity {
        std::wstring_view encoded;
        std::wstring_view decoded;
    };
    constexpr Entity entities[] = {
        {L"&amp;", L"&"},   {L"&quot;", L"\""}, {L"&#39;", L"'"},
        {L"&lt;", L"<"},     {L"&gt;", L">"},     {L"&nbsp;", L" "},
    };

    for (const auto& entity : entities) {
        std::size_t offset = 0;
        while ((offset = value.find(entity.encoded, offset)) !=
               std::wstring::npos) {
            value.replace(offset, entity.encoded.size(), entity.decoded);
            offset += entity.decoded.size();
        }
    }
    return value;
}

std::vector<std::pair<std::wstring, std::wstring>> ParseAttributes(
    std::wstring_view tag) {
    std::vector<std::pair<std::wstring, std::wstring>> attributes;
    std::size_t position = 0;
    while (position < tag.size() && tag[position] != L'<' &&
           !std::iswspace(tag[position])) {
        ++position;
    }

    while (position < tag.size()) {
        while (position < tag.size() &&
               (std::iswspace(tag[position]) || tag[position] == L'<' ||
                tag[position] == L'>')) {
            ++position;
        }
        if (position >= tag.size() || tag[position] == L'/') {
            break;
        }

        const std::size_t keyStart = position;
        while (position < tag.size() && !std::iswspace(tag[position]) &&
               tag[position] != L'=' && tag[position] != L'>') {
            ++position;
        }
        if (keyStart == position) {
            ++position;
            continue;
        }

        const std::wstring key = Lower(tag.substr(keyStart, position - keyStart));
        while (position < tag.size() && std::iswspace(tag[position])) {
            ++position;
        }

        std::wstring value;
        if (position < tag.size() && tag[position] == L'=') {
            ++position;
            while (position < tag.size() && std::iswspace(tag[position])) {
                ++position;
            }
            if (position < tag.size() &&
                (tag[position] == L'"' || tag[position] == L'\'')) {
                const wchar_t quote = tag[position++];
                const std::size_t valueStart = position;
                while (position < tag.size() && tag[position] != quote) {
                    ++position;
                }
                value = std::wstring(tag.substr(valueStart, position - valueStart));
                if (position < tag.size()) {
                    ++position;
                }
            } else {
                const std::size_t valueStart = position;
                while (position < tag.size() && !std::iswspace(tag[position]) &&
                       tag[position] != L'>') {
                    ++position;
                }
                value = std::wstring(tag.substr(valueStart, position - valueStart));
            }
        }
        attributes.emplace_back(key, DecodeHtml(std::move(value)));
    }
    return attributes;
}

std::wstring Attribute(
    const std::vector<std::pair<std::wstring, std::wstring>>& attributes,
    std::wstring_view name) {
    const std::wstring key = Lower(name);
    for (const auto& [attribute, value] : attributes) {
        if (attribute == key) {
            return value;
        }
    }
    return {};
}

bool HasAttribute(
    const std::vector<std::pair<std::wstring, std::wstring>>& attributes,
    std::wstring_view name) {
    const std::wstring key = Lower(name);
    return std::any_of(attributes.begin(), attributes.end(),
                       [&key](const auto& item) { return item.first == key; });
}

std::string ToUtf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                         static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(),
                        static_cast<int>(value.size()), result.data(), size,
                        nullptr, nullptr);
    return result;
}

std::wstring EncodeEscapeDataString(std::wstring_view value) {
    const std::string utf8 = ToUtf8(value);
    constexpr wchar_t hex[] = L"0123456789ABCDEF";
    std::wstring encoded;
    for (const unsigned char byte : utf8) {
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
            byte == '.' || byte == '~') {
            encoded.push_back(static_cast<wchar_t>(byte));
        } else {
            encoded.push_back(L'%');
            encoded.push_back(hex[(byte >> 4) & 0x0F]);
            encoded.push_back(hex[byte & 0x0F]);
        }
    }
    return encoded;
}

std::wstring DoubleEncode(std::wstring_view value) {
    const std::wstring encoded = EncodeEscapeDataString(value);
    return EncodeEscapeDataString(encoded);
}

std::wstring FindFieldValue(const PortalForm& form, std::wstring_view name,
                            std::wstring_view fallback = {}) {
    for (const auto& field : form.fields) {
        if (!field.disabled && field.name == name) {
            return field.value;
        }
    }
    return std::wstring(fallback);
}

} // namespace

std::optional<PortalForm> ParseLoginForm(std::wstring_view html) {
    std::size_t searchFrom = 0;
    while (true) {
        const std::size_t formStart =
            FindInsensitive(html, L"<form", searchFrom);
        if (formStart == std::wstring::npos) {
            return std::nullopt;
        }
        const std::size_t formTagEnd = html.find(L'>', formStart);
        if (formTagEnd == std::wstring::npos) {
            return std::nullopt;
        }

        const auto formAttributes =
            ParseAttributes(html.substr(formStart + 1,
                                        formTagEnd - formStart - 1));
        const std::wstring id = Attribute(formAttributes, L"id");
        const std::wstring name = Attribute(formAttributes, L"name");
        if (Lower(id) != L"gologinform" && Lower(name) != L"gologinform") {
            searchFrom = formTagEnd + 1;
            continue;
        }

        const std::size_t formEnd =
            FindInsensitive(html, L"</form", formTagEnd + 1);
        const std::size_t bodyEnd =
            formEnd == std::wstring::npos ? html.size() : formEnd;

        PortalForm form;
        form.action = Attribute(formAttributes, L"action");
        std::size_t inputFrom = formTagEnd + 1;
        while (inputFrom < bodyEnd) {
            const std::size_t inputStart =
                FindInsensitive(html, L"<input", inputFrom);
            if (inputStart == std::wstring::npos || inputStart >= bodyEnd) {
                break;
            }
            const std::size_t inputEnd = html.find(L'>', inputStart);
            if (inputEnd == std::wstring::npos || inputEnd > bodyEnd) {
                break;
            }

            const auto attributes =
                ParseAttributes(html.substr(inputStart + 1,
                                            inputEnd - inputStart - 1));
            const std::wstring fieldName = Attribute(attributes, L"name");
            const std::wstring type = Lower(Attribute(attributes, L"type"));
            if (!fieldName.empty() && type != L"submit" && type != L"button" &&
                type != L"image" && type != L"reset" &&
                ((type != L"checkbox" && type != L"radio") ||
                 HasAttribute(attributes, L"checked"))) {
                form.fields.push_back(
                    {fieldName, Attribute(attributes, L"value"),
                     HasAttribute(attributes, L"disabled")});
            }
            inputFrom = inputEnd + 1;
        }
        return form;
    }
}

std::wstring ResolveUrl(std::wstring_view baseUrl, std::wstring_view reference) {
    if (reference.empty()) {
        return std::wstring(baseUrl);
    }
    if (FindInsensitive(reference, L"://") != std::wstring::npos) {
        return std::wstring(reference);
    }

    const std::size_t schemeEnd = baseUrl.find(L"://");
    const std::size_t authorityEnd =
        schemeEnd == std::wstring::npos
            ? std::wstring::npos
            : baseUrl.find(L'/', schemeEnd + 3);
    const std::wstring origin =
        authorityEnd == std::wstring::npos
            ? std::wstring(baseUrl)
            : std::wstring(baseUrl.substr(0, authorityEnd));

    if (reference.rfind(L"//", 0) == 0 && schemeEnd != std::wstring::npos) {
        return std::wstring(baseUrl.substr(0, schemeEnd)) + L":" +
               std::wstring(reference);
    }
    if (reference.front() == L'/') {
        return origin + std::wstring(reference);
    }
    if (reference.front() == L'?') {
        const std::size_t queryStart = baseUrl.find(L'?');
        const std::wstring withoutQuery =
            queryStart == std::wstring::npos ? std::wstring(baseUrl)
                                              : std::wstring(baseUrl.substr(0, queryStart));
        return withoutQuery + std::wstring(reference);
    }

    const std::size_t slash = baseUrl.find_last_of(L'/');
    const std::wstring directory =
        slash == std::wstring::npos ? origin + L"/"
                                    : std::wstring(baseUrl.substr(0, slash + 1));
    return directory + std::wstring(reference);
}

std::wstring BuildLoginBody(const PortalForm& form,
                            const Credentials& credentials) {
    const std::wstring service =
        FindFieldValue(form, L"service", L"\u7535\u4FE1");
    const std::wstring queryString = FindFieldValue(form, L"queryString");
    return L"userId=" + DoubleEncode(credentials.username) +
           L"&password=" + DoubleEncode(credentials.password) +
           L"&service=" + DoubleEncode(service) +
           L"&queryString=" + DoubleEncode(queryString) +
           L"&operatorPwd=&operatorUserId=&validcode=&passwordEncrypt=" +
           DoubleEncode(L"false");
}
