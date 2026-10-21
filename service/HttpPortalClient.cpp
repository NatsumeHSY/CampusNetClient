#include "HttpPortalClient.h"

#include "Logger.h"
#include "PortalFormParser.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <memory>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace {

struct ParsedUrl {
    bool secure{false};
    std::wstring host;
    INTERNET_PORT port{INTERNET_DEFAULT_HTTP_PORT};
    std::wstring target{L"/"};
};

struct Response {
    bool transportOk{false};
    unsigned statusCode{0};
    std::wstring location;
    std::wstring cookies;
    std::wstring body;
};

class WinHttpHandle {
public:
    explicit WinHttpHandle(HINTERNET handle = nullptr) : handle_(handle) {}
    ~WinHttpHandle() {
        if (handle_ != nullptr) {
            WinHttpCloseHandle(handle_);
        }
    }
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    HINTERNET get() const { return handle_; }
    explicit operator bool() const { return handle_ != nullptr; }

private:
    HINTERNET handle_;
};

bool ParseUrl(std::wstring_view url, ParsedUrl& result) {
    std::wstring owned(url);
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    wchar_t host[2048]{};
    wchar_t path[8192]{};
    wchar_t extra[8192]{};
    components.lpszHostName = host;
    components.dwHostNameLength = static_cast<DWORD>(std::size(host));
    components.lpszUrlPath = path;
    components.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    components.lpszExtraInfo = extra;
    components.dwExtraInfoLength = static_cast<DWORD>(std::size(extra));
    if (!WinHttpCrackUrl(owned.c_str(), static_cast<DWORD>(owned.size()), 0,
                         &components)) {
        return false;
    }

    result.secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    result.host.assign(host, components.dwHostNameLength);
    result.port = components.nPort;
    result.target.assign(path, components.dwUrlPathLength);
    if (result.target.empty()) {
        result.target = L"/";
    }
    result.target.append(extra, components.dwExtraInfoLength);
    return !result.host.empty();
}

std::wstring QueryHeader(HINTERNET request, DWORD query, LPCWSTR name = nullptr) {
    DWORD size = 0;
    DWORD index = WINHTTP_NO_HEADER_INDEX;
    if (WinHttpQueryHeaders(request, query, name, nullptr, &size, &index) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return {};
    }

    std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1, L'\0');
    index = WINHTTP_NO_HEADER_INDEX;
    if (!WinHttpQueryHeaders(request, query, name, buffer.data(), &size,
                             &index)) {
        return {};
    }
    return std::wstring(buffer.data(), size / sizeof(wchar_t));
}

std::wstring ReadBody(HINTERNET request) {
    std::string bytes;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            return {};
        }
        if (available == 0) {
            break;
        }
        const std::size_t oldSize = bytes.size();
        bytes.resize(oldSize + available);
        DWORD read = 0;
        if (!WinHttpReadData(request, bytes.data() + oldSize, available,
                             &read)) {
            return {};
        }
        bytes.resize(oldSize + read);
        if (read == 0) {
            break;
        }
    }

    if (bytes.empty()) {
        return {};
    }
    const int wideSize = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
        static_cast<int>(bytes.size()), nullptr, 0);
    if (wideSize > 0) {
        std::wstring result(static_cast<std::size_t>(wideSize), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                            static_cast<int>(bytes.size()), result.data(),
                            wideSize);
        return result;
    }

    const int fallbackSize = MultiByteToWideChar(
        CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (fallbackSize <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(fallbackSize), L'\0');
    MultiByteToWideChar(CP_ACP, 0, bytes.data(),
                        static_cast<int>(bytes.size()), result.data(),
                        fallbackSize);
    return result;
}

std::wstring CookieHeader(std::wstring_view setCookies) {
    std::wstring result;
    std::size_t offset = 0;
    while (offset < setCookies.size()) {
        const std::size_t lineEnd = setCookies.find(L'\n', offset);
        const std::size_t end = lineEnd == std::wstring::npos
                                    ? setCookies.size()
                                    : lineEnd;
        std::wstring line(setCookies.substr(offset, end - offset));
        while (!line.empty() &&
               (line.back() == L'\r' || line.back() == L' ' ||
                line.back() == L'\t')) {
            line.pop_back();
        }
        const std::size_t separator = line.find(L';');
        if (separator != std::wstring::npos) {
            line.resize(separator);
        }
        if (!line.empty()) {
            if (!result.empty()) {
                result += L"; ";
            }
            result += line;
        }
        offset = lineEnd == std::wstring::npos ? setCookies.size() : lineEnd + 1;
    }
    return result;
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

Response Request(std::wstring_view url, std::wstring_view method,
                 std::wstring_view body, std::wstring_view cookie,
                 std::wstring_view extraHeaders, int timeoutMs) {
    ParsedUrl parsed;
    if (!ParseUrl(url, parsed)) {
        return {};
    }

    WinHttpHandle session(WinHttpOpen(
        L"CampusNetClient/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        return {};
    }
    WinHttpSetTimeouts(session.get(), timeoutMs, timeoutMs, timeoutMs,
                       timeoutMs);

    WinHttpHandle connection(
        WinHttpConnect(session.get(), parsed.host.c_str(), parsed.port, 0));
    if (!connection) {
        return {};
    }

    const DWORD flags = parsed.secure ? WINHTTP_FLAG_SECURE : 0;
    WinHttpHandle request(WinHttpOpenRequest(
        connection.get(), std::wstring(method).c_str(), parsed.target.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!request) {
        return {};
    }
    WinHttpSetTimeouts(request.get(), timeoutMs, timeoutMs, timeoutMs,
                       timeoutMs);
    const DWORD disableRedirects = WINHTTP_DISABLE_REDIRECTS;
    WinHttpSetOption(request.get(), WINHTTP_OPTION_DISABLE_FEATURE,
                     const_cast<DWORD*>(&disableRedirects),
                     sizeof(disableRedirects));

    std::wstring headers(extraHeaders);
    if (!cookie.empty()) {
        headers += L"Cookie: ";
        headers += cookie;
        headers += L"\r\n";
    }
    const std::string bodyBytes = ToUtf8(body);
    const void* data = bodyBytes.empty() ? WINHTTP_NO_REQUEST_DATA
                                         : bodyBytes.data();
    const DWORD dataBytes = static_cast<DWORD>(bodyBytes.size());
    if (!WinHttpSendRequest(request.get(),
                            headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS
                                             : headers.c_str(),
                            headers.empty() ? 0 : static_cast<DWORD>(headers.size()),
                            const_cast<void*>(data),
                            body.empty() ? 0 : dataBytes, dataBytes, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) {
        return {};
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(),
                             WINHTTP_QUERY_STATUS_CODE |
                                 WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status,
                             &statusSize, WINHTTP_NO_HEADER_INDEX)) {
        return {};
    }

    Response response;
    response.transportOk = true;
    response.statusCode = status;
    response.location = QueryHeader(request.get(), WINHTTP_QUERY_LOCATION);
    response.cookies = QueryHeader(request.get(), WINHTTP_QUERY_SET_COOKIE);
    response.body = ReadBody(request.get());
    return response;
}

std::wstring QueryStringField(const PortalForm& form, std::wstring_view name) {
    for (const auto& field : form.fields) {
        if (!field.disabled && field.name == name) {
            return field.value;
        }
    }
    return {};
}

} // namespace

HttpLoginResult ClassifyLoginResponse(unsigned statusCode,
                                      std::wstring_view location,
                                      std::wstring_view body) {
    HttpLoginResult result;
    result.statusCode = statusCode;
    result.location = std::wstring(location);
    if (statusCode == 0) {
        result.outcome = HttpLoginOutcome::TransportError;
        result.message = L"HTTP transport failed";
    } else if (statusCode >= 300 && statusCode < 400) {
        result.outcome = HttpLoginOutcome::Redirected;
        result.message = L"Portal returned a redirect";
    } else if (statusCode < 200 || statusCode >= 300) {
        result.outcome = HttpLoginOutcome::InvalidResponse;
        result.message = L"Portal returned an unexpected status";
    } else if (body.find(L"goLoginForm") != std::wstring::npos) {
        result.outcome = HttpLoginOutcome::LoginRejected;
        result.message = L"Portal still presents the login form";
    } else {
        result.outcome = HttpLoginOutcome::Success;
        result.message = L"Portal accepted the login request";
    }
    return result;
}

InternetStatus ClassifyInternetResponse(unsigned statusCode,
                                         unsigned expectedStatus) {
    InternetStatus result;
    result.statusCode = statusCode;
    result.reachable = statusCode != 0 && statusCode == expectedStatus;
    return result;
}

HttpPortalClient::HttpPortalClient(Logger* logger) : logger_(logger) {}

HttpLoginResult HttpPortalClient::Login(const AppConfig& config,
                                        const Credentials& credentials) const {
    const Response page = Request(config.portalUrl, L"GET", {}, {}, {},
                                  config.httpTimeoutMs);
    if (!page.transportOk) {
        return {HttpLoginOutcome::TransportError, 0, {},
                L"Portal GET failed"};
    }
    if (page.statusCode >= 300 && page.statusCode < 400) {
        return ClassifyLoginResponse(page.statusCode, page.location, {});
    }

    const auto form = ParseLoginForm(page.body);
    if (!form.has_value()) {
        return {HttpLoginOutcome::InvalidResponse, page.statusCode, {},
                L"Login form not found"};
    }

    std::wstring action = ResolveUrl(config.portalUrl, form->action);
    if (form->action.empty()) {
        const std::wstring parameter = QueryStringField(*form, L"urlParameter");
        action = ResolveUrl(config.portalUrl, L"/webauth.do");
        if (!parameter.empty()) {
            action += L"?" + parameter;
        }
    }

    const std::wstring body = BuildLoginBody(*form, credentials);
    const std::wstring headers =
        L"Content-Type: application/x-www-form-urlencoded\r\n";
    const Response login = Request(action, L"POST", body,
                                   CookieHeader(page.cookies), headers,
                                   config.httpTimeoutMs);
    if (!login.transportOk) {
        return {HttpLoginOutcome::TransportError, 0, {},
                L"Portal POST failed"};
    }

    const HttpLoginResult result =
        ClassifyLoginResponse(login.statusCode, login.location, login.body);
    if (logger_ != nullptr) {
        logger_->Info(L"HTTP portal login completed with status " +
                      std::to_wstring(result.statusCode));
    }
    return result;
}

InternetStatus HttpPortalClient::CheckInternet(const AppConfig& config) const {
    InternetStatus last;
    for (const auto& endpoint : config.internetChecks) {
        last = ClassifyInternetResponse(0, endpoint.expectedStatus);
        last.endpoint = endpoint.url;
        const Response response = Request(endpoint.url, L"GET", {}, {}, {},
                                          config.verifyTimeoutMs);
        if (!response.transportOk) {
            continue;
        }
        last = ClassifyInternetResponse(response.statusCode,
                                        endpoint.expectedStatus);
        last.endpoint = endpoint.url;
        if (last.reachable) {
            return last;
        }
    }
    return last;
}
