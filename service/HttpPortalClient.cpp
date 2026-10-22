#include "HttpPortalClient.h"

#include "Logger.h"
#include "PortalFormParser.h"
#include "PortalNetworkIdentity.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <ipexport.h>
#include <icmpapi.h>
#include <iphlpapi.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <memory>
#include <regex>
#include <thread>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

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

class WinsockRuntime {
public:
    WinsockRuntime() {
        WSADATA data{};
        started_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~WinsockRuntime() {
        if (started_) {
            WSACleanup();
        }
    }
    bool Started() const { return started_; }

private:
    bool started_{false};
};

WinsockRuntime& Winsock() {
    static WinsockRuntime runtime;
    return runtime;
}

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
                 std::wstring_view body, std::wstring_view extraHeaders,
                 int timeoutMs) {
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
    DWORD maxRedirects = 5;
    WinHttpSetOption(request.get(), WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS,
                     &maxRedirects, sizeof(maxRedirects));

    std::wstring headers(extraHeaders);
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
    response.body = ReadBody(request.get());
    return response;
}

bool ContainsInsensitive(std::wstring_view value, std::wstring_view marker) {
    if (marker.empty()) {
        return true;
    }
    return std::search(value.begin(), value.end(), marker.begin(), marker.end(),
                       [](wchar_t left, wchar_t right) {
                           return std::towlower(left) == std::towlower(right);
                       }) != value.end();
}

std::wstring FormatMacAddress(const BYTE* address, ULONG length) {
    if (address == nullptr || length == 0) {
        return {};
    }
    std::wstring result;
    result.reserve(length * 2);
    for (ULONG index = 0; index < length; ++index) {
        wchar_t byteText[3]{};
        swprintf_s(byteText, ARRAYSIZE(byteText), L"%02x", address[index]);
        result.append(byteText);
    }
    return result;
}

std::vector<PortalAdapterInfo> ReadPortalAdapters() {
    if (!Winsock().Started()) {
        return {};
    }
    ULONG bufferLength = 15 * 1024;
    std::vector<BYTE> buffer(bufferLength);
    auto* adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
    ULONG status = GetAdaptersAddresses(
        AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                     GAA_FLAG_SKIP_DNS_SERVER,
        nullptr, adapters, &bufferLength);
    if (status == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(bufferLength);
        adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
        status = GetAdaptersAddresses(
            AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                         GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, adapters, &bufferLength);
    }
    if (status != NO_ERROR) {
        return {};
    }

    std::vector<PortalAdapterInfo> result;
    for (auto* adapter = adapters; adapter != nullptr; adapter = adapter->Next) {
        PortalAdapterInfo base;
        base.name = adapter->FriendlyName == nullptr ? L"" : adapter->FriendlyName;
        base.description =
            adapter->Description == nullptr ? L"" : adapter->Description;
        base.up = adapter->OperStatus == IfOperStatusUp;
        base.mac = FormatMacAddress(adapter->PhysicalAddress,
                                    adapter->PhysicalAddressLength);

        bool addedAddress = false;
        for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr;
             unicast = unicast->Next) {
            if (unicast->Address.lpSockaddr == nullptr ||
                unicast->Address.lpSockaddr->sa_family != AF_INET) {
                continue;
            }
            const auto* address = reinterpret_cast<const sockaddr_in*>(
                unicast->Address.lpSockaddr);
            wchar_t ipv4[INET_ADDRSTRLEN]{};
            if (InetNtopW(AF_INET,
                          const_cast<IN_ADDR*>(&address->sin_addr), ipv4,
                          ARRAYSIZE(ipv4)) == nullptr) {
                continue;
            }
            PortalAdapterInfo candidate = base;
            candidate.ipv4 = ipv4;
            result.push_back(std::move(candidate));
            addedAddress = true;
        }
        if (!addedAddress) {
            result.push_back(std::move(base));
        }
    }
    return result;
}

PortalNetworkIdentity ReadPortalNetworkIdentity() {
    return SelectPortalNetworkIdentity(ReadPortalAdapters());
}

bool PingPortal(std::wstring_view host) {
    if (!Winsock().Started()) {
        return false;
    }
    const std::wstring ownedHost(host);
    IN_ADDR destination{};
    if (InetPtonW(AF_INET, ownedHost.c_str(), &destination) != 1) {
        return false;
    }

    HANDLE icmp = IcmpCreateFile();
    if (icmp == INVALID_HANDLE_VALUE) {
        return false;
    }
    const char payload[] = "CampusNet";
    std::vector<char> replyBuffer(sizeof(ICMP_ECHO_REPLY) + sizeof(payload));
    const DWORD replyCount = IcmpSendEcho(
        icmp, destination.S_un.S_addr, const_cast<char*>(payload),
        sizeof(payload), nullptr, replyBuffer.data(),
        static_cast<DWORD>(replyBuffer.size()), 1000);
    const auto* reply = reinterpret_cast<const ICMP_ECHO_REPLY*>(
        replyBuffer.data());
    const bool reachable = replyCount != 0 && reply->Status == IP_SUCCESS;
    IcmpCloseHandle(icmp);
    return reachable;
}

bool WaitForPortalNetwork(const ParsedUrl& portal,
                          const std::atomic<bool>& cancelled) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(90);
    while (std::chrono::steady_clock::now() < deadline) {
        if (cancelled.load()) {
            return false;
        }
        if (!ReadPortalNetworkIdentity().ipv4.empty() &&
            PingPortal(portal.host)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }
    return false;
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
    } else if (std::regex_search(
                   body.begin(), body.end(),
                   std::wregex(LR"(\"result\"\s*:\s*\"success\")",
                               std::regex_constants::icase))) {
        result.outcome = HttpLoginOutcome::Success;
        result.message = L"Portal accepted the login request";
    } else {
        result.outcome = HttpLoginOutcome::LoginRejected;
        result.message = L"Portal did not report result=success";
    }
    return result;
}

bool IsPortalAlreadyOnline(std::wstring_view body) {
    return ContainsInsensitive(body, L"\u767B\u5F55\u6210\u529F") ||
           ContainsInsensitive(body, L"success.jsp") ||
           ContainsInsensitive(body, L"userIndex");
}

InternetStatus ClassifyInternetResponse(unsigned statusCode,
                                         unsigned expectedStatus) {
    InternetStatus result;
    result.statusCode = statusCode;
    result.reachable = statusCode != 0 && statusCode == expectedStatus;
    return result;
}

std::wstring ParsePublicIpResponse(std::wstring_view response) {
    while (!response.empty() && std::iswspace(response.front())) {
        response.remove_prefix(1);
    }
    while (!response.empty() && std::iswspace(response.back())) {
        response.remove_suffix(1);
    }
    if (response.empty() || response.size() > INET6_ADDRSTRLEN) {
        return {};
    }

    const std::wstring address(response);
    if (!Winsock().Started()) {
        return {};
    }
    IN_ADDR ipv4{};
    IN6_ADDR ipv6{};
    if (InetPtonW(AF_INET, address.c_str(), &ipv4) == 1 ||
        InetPtonW(AF_INET6, address.c_str(), &ipv6) == 1) {
        return address;
    }
    return {};
}

HttpPortalClient::HttpPortalClient(Logger* logger) : logger_(logger) {}

void HttpPortalClient::Cancel() noexcept { cancelled_.store(true); }

HttpLoginResult HttpPortalClient::Login(const AppConfig& config,
                                        const Credentials& credentials) const {
    if (cancelled_.load()) {
        return {HttpLoginOutcome::Cancelled, 0, {}, L"Login cancelled"};
    }

    ParsedUrl portal;
    if (!ParseUrl(config.portalUrl, portal)) {
        return {HttpLoginOutcome::InvalidResponse, 0, {},
                L"Invalid campus portal URL"};
    }
    if (!WaitForPortalNetwork(portal, cancelled_)) {
        if (cancelled_.load()) {
            return {HttpLoginOutcome::Cancelled, 0, {}, L"Login cancelled"};
        }
        return {HttpLoginOutcome::TransportError, 0, {},
                L"Campus portal is not reachable"};
    }

    const std::wstring rootUrl = ResolveUrl(config.portalUrl, L"/");
    const Response page = Request(rootUrl, L"GET", {}, {}, 10000);
    if (cancelled_.load()) {
        return {HttpLoginOutcome::Cancelled, 0, {}, L"Login cancelled"};
    }
    if (page.transportOk && IsPortalAlreadyOnline(page.body)) {
        return {HttpLoginOutcome::Success, page.statusCode, {},
                L"Already authenticated at the campus portal"};
    }

    const PortalNetworkIdentity identity = ReadPortalNetworkIdentity();
    if (identity.ipv4.empty() || identity.mac.empty()) {
        return {HttpLoginOutcome::TransportError, 0, {},
                L"Unable to identify the campus network adapter"};
    }
    if (cancelled_.load()) {
        return {HttpLoginOutcome::Cancelled, 0, {}, L"Login cancelled"};
    }

    PortalForm fields;
    fields.fields.push_back({L"service", L"\u7535\u4FE1", false});
    fields.fields.push_back(
        {L"queryString", BuildPortalQueryString(identity), false});
    const std::wstring body = BuildLoginBody(fields, credentials);
    const std::wstring action = ResolveUrl(
        config.portalUrl, L"/eportal/InterFace.do?method=login");
    const std::wstring headers =
        L"Content-Type: application/x-www-form-urlencoded; charset=UTF-8\r\n";
    const Response login = Request(action, L"POST", body, headers, 20000);
    if (!login.transportOk) {
        if (cancelled_.load()) {
            return {HttpLoginOutcome::Cancelled, 0, {}, L"Login cancelled"};
        }
        return {HttpLoginOutcome::TransportError, 0, {},
                L"Portal POST failed"};
    }

    const HttpLoginResult result =
        ClassifyLoginResponse(login.statusCode, login.location, login.body);
    if (cancelled_.load() && result.outcome != HttpLoginOutcome::Success) {
        return {HttpLoginOutcome::Cancelled, 0, {}, L"Login cancelled"};
    }
    if (logger_ != nullptr) {
        logger_->Info(L"HTTP portal login completed with status " +
                      std::to_wstring(result.statusCode));
    }

    if (result.outcome == HttpLoginOutcome::Success && !cancelled_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        if (!cancelled_.load()) {
            const Response confirmation =
                Request(rootUrl, L"GET", {}, {}, 10000);
            if (logger_ != nullptr) {
                if (confirmation.transportOk &&
                    IsPortalAlreadyOnline(confirmation.body)) {
                    logger_->Info(L"Campus portal online status confirmed");
                } else {
                    logger_->Warn(
                        L"Portal API accepted login; online page not confirmed yet");
                }
            }
        }
    }
    return result;
}

InternetStatus HttpPortalClient::CheckInternet(const AppConfig& config) const {
    InternetStatus last;
    for (const auto& endpoint : config.internetChecks) {
        last = ClassifyInternetResponse(0, endpoint.expectedStatus);
        last.endpoint = endpoint.url;
        const Response response = Request(endpoint.url, L"GET", {}, {},
                                          config.verifyTimeoutMs);
        if (!response.transportOk) {
            continue;
        }
        last = ClassifyInternetResponse(response.statusCode,
                                        endpoint.expectedStatus);
        last.endpoint = endpoint.url;
        if (last.reachable) {
            break;
        }
    }

    constexpr std::wstring_view publicIpEndpoint =
        L"https://api64.ipify.org/";
    const Response publicIpResponse =
        Request(publicIpEndpoint, L"GET", {}, {}, config.verifyTimeoutMs);
    if (publicIpResponse.transportOk && publicIpResponse.statusCode == 200) {
        last.publicIp = ParsePublicIpResponse(publicIpResponse.body);
        if (!last.publicIp.empty() && !last.reachable) {
            last.reachable = true;
            last.statusCode = publicIpResponse.statusCode;
            last.endpoint = publicIpEndpoint;
        }
    }
    return last;
}
