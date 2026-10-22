#include "WinHttpUpdateTransport.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <WinHttp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr std::wstring_view kRawHost = L"raw.githubusercontent.com";
constexpr std::wstring_view kGitHubHost = L"github.com";
constexpr std::wstring_view kGitHubReleasePath =
    L"/NatsumeHSY/CampusNetClient/releases/download/";
constexpr std::array<std::wstring_view, 3> kAssetCdnHosts{
    L"release-assets.githubusercontent.com",
    L"objects.githubusercontent.com",
    L"github-releases.githubusercontent.com",
};

struct ParsedUrl {
    std::wstring host;
    std::wstring path;
    std::wstring extra;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
};

class WinHttpHandle final {
public:
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET handle) : handle_(handle) {}
    ~WinHttpHandle() { Reset(); }
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    WinHttpHandle(WinHttpHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)) {}
    WinHttpHandle& operator=(WinHttpHandle&& other) noexcept {
        if (this != &other) {
            Reset();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }
    HINTERNET Get() const { return handle_; }
    explicit operator bool() const { return handle_ != nullptr; }

private:
    void Reset() {
        if (handle_ != nullptr) {
            WinHttpCloseHandle(handle_);
            handle_ = nullptr;
        }
    }
    HINTERNET handle_ = nullptr;
};

std::wstring WinHttpError(std::wstring_view operation, DWORD code) {
    return std::wstring(operation) + L" failed (WinHTTP error " +
           std::to_wstring(code) + L")";
}

bool IsRedirectStatus(DWORD status) {
    return status == HTTP_STATUS_MOVED || status == HTTP_STATUS_REDIRECT ||
           status == HTTP_STATUS_REDIRECT_METHOD ||
           status == HTTP_STATUS_REDIRECT_KEEP_VERB ||
           status == HTTP_STATUS_PERMANENT_REDIRECT;
}

bool ParseHttpsUrl(std::wstring_view url, ParsedUrl& parsed) {
    if (url.empty() || url.size() > 8192 ||
        url.find_first_of(L"\\ \t\r\n#") != std::wstring_view::npos ||
        std::any_of(url.begin(), url.end(), [](wchar_t ch) { return ch < 0x20; })) {
        return false;
    }
    const std::wstring owned(url);
    std::vector<wchar_t> scheme(owned.size() + 1);
    std::vector<wchar_t> host(owned.size() + 1);
    std::vector<wchar_t> user(owned.size() + 1);
    std::vector<wchar_t> password(owned.size() + 1);
    std::vector<wchar_t> path(owned.size() + 1);
    std::vector<wchar_t> extra(owned.size() + 1);

    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.lpszScheme = scheme.data();
    components.dwSchemeLength = static_cast<DWORD>(scheme.size());
    components.lpszHostName = host.data();
    components.dwHostNameLength = static_cast<DWORD>(host.size());
    components.lpszUserName = user.data();
    components.dwUserNameLength = static_cast<DWORD>(user.size());
    components.lpszPassword = password.data();
    components.dwPasswordLength = static_cast<DWORD>(password.size());
    components.lpszUrlPath = path.data();
    components.dwUrlPathLength = static_cast<DWORD>(path.size());
    components.lpszExtraInfo = extra.data();
    components.dwExtraInfoLength = static_cast<DWORD>(extra.size());

    if (!WinHttpCrackUrl(owned.c_str(), static_cast<DWORD>(owned.size()), 0,
                         &components) ||
        components.nScheme != INTERNET_SCHEME_HTTPS ||
        components.nPort != INTERNET_DEFAULT_HTTPS_PORT ||
        components.dwUserNameLength != 0 || components.dwPasswordLength != 0) {
        return false;
    }
    parsed.host.assign(host.data(), components.dwHostNameLength);
    parsed.path.assign(path.data(), components.dwUrlPathLength);
    parsed.extra.assign(extra.data(), components.dwExtraInfoLength);
    parsed.port = components.nPort;
    return !parsed.host.empty();
}

bool EqualsOrdinalIgnoreCase(std::wstring_view left, std::wstring_view right) {
    return CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                                right.data(), static_cast<int>(right.size()),
                                TRUE) == CSTR_EQUAL;
}

bool IsGitHubReleasePath(std::wstring_view path) {
    return path.starts_with(kGitHubReleasePath) &&
           path.size() > kGitHubReleasePath.size();
}

bool IsAllowedRedirectForPurpose(std::wstring_view url,
                                 UpdateUrlPurpose purpose) {
    ParsedUrl parsed;
    if (!ParseHttpsUrl(url, parsed)) {
        return false;
    }
    if (purpose == UpdateUrlPurpose::Manifest) {
        return EqualsOrdinalIgnoreCase(parsed.host, kRawHost) &&
               parsed.path == L"/NatsumeHSY/CampusNetClient/main/version.json" &&
               parsed.extra.empty();
    }
    if (EqualsOrdinalIgnoreCase(parsed.host, kGitHubHost)) {
        return IsGitHubReleasePath(parsed.path) &&
               (purpose == UpdateUrlPurpose::ArtifactRedirect ||
                parsed.extra.empty());
    }
    if (purpose == UpdateUrlPurpose::ArtifactRedirect) {
        return std::any_of(kAssetCdnHosts.begin(), kAssetCdnHosts.end(),
                           [&](std::wstring_view host) {
                               return EqualsOrdinalIgnoreCase(parsed.host, host);
                           });
    }
    return false;
}

bool QueryHeader(HINTERNET request, DWORD query, std::wstring& value,
                 bool& present, std::wstring& error) {
    present = false;
    DWORD bytes = 0;
    if (WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX,
                            nullptr, &bytes, WINHTTP_NO_HEADER_INDEX)) {
        present = true;
        value.clear();
        return true;
    }
    DWORD lastError = GetLastError();
    if (lastError == ERROR_WINHTTP_HEADER_NOT_FOUND) {
        value.clear();
        return true;
    }
    if (lastError != ERROR_INSUFFICIENT_BUFFER || bytes == 0 ||
        bytes % sizeof(wchar_t) != 0) {
        error = WinHttpError(L"WinHttpQueryHeaders", lastError);
        return false;
    }
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    if (!WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX,
                             buffer.data(), &bytes,
                             WINHTTP_NO_HEADER_INDEX)) {
        error = WinHttpError(L"WinHttpQueryHeaders", GetLastError());
        return false;
    }
    const std::size_t length = wcsnlen(buffer.data(), buffer.size());
    value.assign(buffer.data(), length);
    present = true;
    return true;
}

bool ParseContentLength(std::wstring_view text, std::uint64_t& value) {
    if (text.empty()) {
        return false;
    }
    value = 0;
    for (wchar_t ch : text) {
        if (ch < L'0' || ch > L'9') {
            return false;
        }
        const unsigned digit = static_cast<unsigned>(ch - L'0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    return true;
}

bool QueryContentLength(HINTERNET request, std::uint64_t& length,
                        std::wstring& error) {
    std::wstring text;
    bool present = false;
    if (!QueryHeader(request, WINHTTP_QUERY_CONTENT_LENGTH, text, present,
                     error)) {
        return false;
    }
    if (!present) {
        length = 0;
        return true;
    }
    if (!ParseContentLength(text, length)) {
        error = L"HTTP Content-Length is invalid";
        return false;
    }
    return true;
}

bool ResolveRedirect(std::wstring_view currentUrl, std::wstring_view location,
                     std::wstring& resolved) {
    while (!location.empty() &&
           (location.front() == L' ' || location.front() == L'\t')) {
        location.remove_prefix(1);
    }
    while (!location.empty() &&
           (location.back() == L' ' || location.back() == L'\t')) {
        location.remove_suffix(1);
    }
    if (location.empty()) {
        return false;
    }
    if (location.starts_with(L"https://") ||
        location.starts_with(L"HTTPS://")) {
        resolved.assign(location);
        return true;
    }
    if (location.starts_with(L"//")) {
        resolved = L"https:";
        resolved.append(location);
        return true;
    }
    if (!location.starts_with(L"/")) {
        return false;
    }
    ParsedUrl current;
    if (!ParseHttpsUrl(currentUrl, current)) {
        return false;
    }
    resolved = L"https://" + current.host;
    resolved.append(location);
    return true;
}

struct OpenResponse {
    WinHttpHandle connection;
    WinHttpHandle request;
    std::wstring finalUrl;
    DWORD statusCode = 0;
    std::uint64_t contentLength = 0;
};

bool OpenFinalResponse(HINTERNET session, std::wstring_view initialUrl,
                       UpdateUrlPurpose initialPurpose,
                       const HttpLimits& limits, OpenResponse& result,
                       std::wstring& error,
                       std::chrono::steady_clock::time_point deadline) {
    if (limits.maxBytes == 0 || limits.totalTimeoutMs == 0 ||
        limits.readTimeoutMs == 0 || limits.maxRedirects > 10) {
        error = L"HTTP request limits are invalid";
        return false;
    }
    std::wstring currentUrl(initialUrl);
    std::uint32_t redirects = 0;
    for (;;) {
        if (std::chrono::steady_clock::now() >= deadline) {
            error = L"HTTP request exceeded its total timeout";
            return false;
        }
        const auto currentPurpose = redirects == 0
                                        ? initialPurpose
                                        : (initialPurpose == UpdateUrlPurpose::Manifest
                                               ? UpdateUrlPurpose::Manifest
                                               : UpdateUrlPurpose::ArtifactRedirect);
        if (!IsAllowedRedirectForPurpose(currentUrl, currentPurpose)) {
            error = L"HTTP request URL or redirect host is not allowed";
            return false;
        }
        ParsedUrl parsed;
        if (!ParseHttpsUrl(currentUrl, parsed)) {
            error = L"HTTP request URL is invalid";
            return false;
        }

        WinHttpHandle connection(WinHttpConnect(
            session, parsed.host.c_str(), parsed.port, 0));
        if (!connection) {
            error = WinHttpError(L"WinHttpConnect", GetLastError());
            return false;
        }
        const std::wstring requestTarget = parsed.path.empty()
                                               ? L"/"
                                               : parsed.path + parsed.extra;
        LPCWSTR acceptTypes[] = {L"*/*", nullptr};
        WinHttpHandle request(WinHttpOpenRequest(
            connection.Get(), L"GET", requestTarget.c_str(), nullptr,
            WINHTTP_NO_REFERER, acceptTypes, WINHTTP_FLAG_SECURE));
        if (!request) {
            error = WinHttpError(L"WinHttpOpenRequest", GetLastError());
            return false;
        }
        DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        if (!WinHttpSetOption(request.Get(), WINHTTP_OPTION_REDIRECT_POLICY,
                              &redirectPolicy, sizeof(redirectPolicy))) {
            error = WinHttpError(L"WinHttpSetOption(redirect policy)",
                                 GetLastError());
            return false;
        }
        if (!WinHttpSendRequest(request.Get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            error = WinHttpError(L"WinHttpSendRequest", GetLastError());
            return false;
        }
        if (!WinHttpReceiveResponse(request.Get(), nullptr)) {
            error = WinHttpError(L"WinHttpReceiveResponse", GetLastError());
            return false;
        }

        DWORD status = 0;
        DWORD statusBytes = sizeof(status);
        if (!WinHttpQueryHeaders(
                request.Get(), WINHTTP_QUERY_STATUS_CODE |
                                   WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusBytes,
                WINHTTP_NO_HEADER_INDEX)) {
            error = WinHttpError(L"WinHttpQueryHeaders(status)", GetLastError());
            return false;
        }
        if (IsRedirectStatus(status)) {
            if (redirects >= limits.maxRedirects) {
                error = L"HTTP redirect limit exceeded";
                return false;
            }
            std::wstring location;
            bool present = false;
            if (!QueryHeader(request.Get(), WINHTTP_QUERY_LOCATION, location,
                             present, error) ||
                !present ||
                !ResolveRedirect(currentUrl, location, currentUrl)) {
                if (error.empty()) {
                    error = L"HTTP redirect did not contain a valid Location";
                }
                return false;
            }
            ++redirects;
            continue;
        }

        std::uint64_t contentLength = 0;
        if (!QueryContentLength(request.Get(), contentLength, error)) {
            return false;
        }
        if (contentLength > limits.maxBytes) {
            error = L"HTTP response exceeds its configured size limit";
            return false;
        }
        result.connection = std::move(connection);
        result.request = std::move(request);
        result.finalUrl = std::move(currentUrl);
        result.statusCode = status;
        result.contentLength = contentLength;
        return true;
    }
}

bool ReadBody(HINTERNET request, std::uint64_t declaredLength,
              const HttpLimits& limits, const ChunkSink& sink,
              const ProgressCallback& progress, std::uint64_t& received,
              std::wstring& error,
              std::chrono::steady_clock::time_point deadline) {
    std::vector<std::uint8_t> buffer(64 * 1024);
    const auto start = std::chrono::steady_clock::now();
    received = 0;
    for (;;) {
        if (std::chrono::steady_clock::now() >= deadline) {
            error = L"HTTP response exceeded its total timeout";
            return false;
        }
        DWORD bytesRead = 0;
        if (!WinHttpReadData(request, buffer.data(),
                             static_cast<DWORD>(buffer.size()), &bytesRead)) {
            error = WinHttpError(L"WinHttpReadData", GetLastError());
            return false;
        }
        if (bytesRead == 0) {
            break;
        }
        if (bytesRead > limits.maxBytes - received) {
            error = L"HTTP response exceeds its configured size limit";
            return false;
        }
        const std::span<const std::uint8_t> chunk(buffer.data(), bytesRead);
        if (!sink(chunk)) {
            if (error.empty()) {
                error = L"HTTP response consumer stopped reading";
            }
            return false;
        }
        received += bytesRead;
        if (progress) {
            const auto elapsed = std::chrono::duration_cast<
                std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                           start)
                                     .count();
            const auto speed = elapsed > 0 ? (received * 1000) /
                                                 static_cast<std::uint64_t>(elapsed)
                                           : 0;
            progress(received, declaredLength, speed);
        }
    }
    if (declaredLength != 0 && received != declaredLength) {
        error = L"HTTP response ended before its declared Content-Length";
        return false;
    }
    return true;
}

bool CreateSession(const HttpLimits& limits, WinHttpHandle& session,
                   std::wstring& error) {
    session = WinHttpHandle(WinHttpOpen(
        L"CampusNetUpdater/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        error = WinHttpError(L"WinHttpOpen", GetLastError());
        return false;
    }
    const int timeout = static_cast<int>(std::min<std::uint32_t>(
        limits.readTimeoutMs, static_cast<std::uint32_t>(INT_MAX)));
    if (!WinHttpSetTimeouts(session.Get(), timeout, timeout, timeout, timeout)) {
        error = WinHttpError(L"WinHttpSetTimeouts", GetLastError());
        return false;
    }
    return true;
}

} // namespace

bool IsAllowedUpdateUrl(std::wstring_view url, UpdateUrlPurpose purpose) {
    if (purpose == UpdateUrlPurpose::Artifact) {
        ParsedUrl parsed;
        return ParseHttpsUrl(url, parsed) &&
               EqualsOrdinalIgnoreCase(parsed.host, kGitHubHost) &&
               IsGitHubReleasePath(parsed.path) && parsed.extra.empty();
    }
    return IsAllowedRedirectForPurpose(url, purpose);
}

bool WinHttpUpdateTransport::Get(std::wstring_view url,
                                 const HttpLimits& limits,
                                 HttpResponse& response,
                                 std::wstring& error) {
    error.clear();
    response = {};
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(limits.totalTimeoutMs);
    WinHttpHandle session;
    if (!CreateSession(limits, session, error)) {
        return false;
    }
    OpenResponse opened;
    if (!OpenFinalResponse(session.Get(), url, UpdateUrlPurpose::Manifest,
                           limits, opened, error, deadline)) {
        return false;
    }
    response.statusCode = opened.statusCode;
    response.contentLength = opened.contentLength;
    response.finalUrl = opened.finalUrl;
    if (response.statusCode != 200) {
        return true;
    }

    const ChunkSink appendBody = [&](std::span<const std::uint8_t> chunk) {
        response.body.append(reinterpret_cast<const char*>(chunk.data()),
                             chunk.size());
        return true;
    };
    std::uint64_t received = 0;
    return ReadBody(opened.request.Get(), opened.contentLength, limits,
                    appendBody, {}, received, error, deadline);
}

bool WinHttpUpdateTransport::Download(std::wstring_view url,
                                     const HttpLimits& limits,
                                     const ChunkSink& sink,
                                     const ProgressCallback& progress,
                                     std::wstring& error) {
    error.clear();
    if (!sink) {
        error = L"HTTP download requires a data sink";
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(limits.totalTimeoutMs);
    WinHttpHandle session;
    if (!CreateSession(limits, session, error)) {
        return false;
    }
    OpenResponse opened;
    if (!OpenFinalResponse(session.Get(), url, UpdateUrlPurpose::Artifact,
                           limits, opened, error, deadline)) {
        return false;
    }
    if (opened.statusCode != 200) {
        error = L"Update asset request returned a non-success HTTP status";
        return false;
    }
    std::uint64_t received = 0;
    return ReadBody(opened.request.Get(), opened.contentLength, limits, sink,
                    progress, received, error, deadline);
}
