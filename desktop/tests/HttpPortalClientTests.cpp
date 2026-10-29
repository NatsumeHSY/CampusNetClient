#include "HttpPortalClient.h"
#include "PortalNetworkIdentity.h"

#include <cassert>

int RunHttpPortalClientTests() {
    assert(ExtractBossSessionId(L"HTTP/1.1 200 OK\r\nSet-Cookie: JSESSIONID-BOSS-0=Abc-123.node; Path=/; HttpOnly\r\n") == L"Abc-123.node");
    assert(ExtractBossSessionId(L"Cookie: other=x; JSESSIONID-BOSS-0=session_2") == L"session_2");
    assert(ExtractBossSessionId(L"Set-Cookie: unrelated=abc").empty());
    assert(ExtractBossSessionId(L"Set-Cookie: JSESSIONID-BOSS-0=bad/value; Path=/").empty());
    assert(ExtractBossSessionId(L"Set-Cookie: OTHERJSESSIONID-BOSS-0=abc").empty());

    const auto accepted = ClassifyLoginResponse(
        200, {}, L"{\"result\": \"success\", \"message\": \"ok\"}");
    assert(accepted.outcome == HttpLoginOutcome::Success);

    const auto rejected = ClassifyLoginResponse(
        200, {}, L"{\"result\": \"fail\", \"message\": \"bad credentials\"}");
    assert(rejected.outcome == HttpLoginOutcome::LoginRejected);

    const auto htmlIsNotSuccess =
        ClassifyLoginResponse(200, {}, L"<html>login accepted</html>");
    assert(htmlIsNotSuccess.outcome == HttpLoginOutcome::LoginRejected);

    assert(IsPortalAlreadyOnline(L"<html>\u767B\u5F55\u6210\u529F</html>"));
    assert(IsPortalAlreadyOnline(L"<a href=\"/eportal/success.jsp\">done</a>"));
    assert(IsPortalAlreadyOnline(L"<div>userIndex</div>"));
    assert(!IsPortalAlreadyOnline(L"<form id=\"goLoginForm\"></form>"));

    const std::vector<PortalAdapterInfo> adapters{
        {L"vEthernet (WSL)", L"Hyper-V Virtual Ethernet Adapter", true,
         L"172.23.128.1", L"00155d000001"},
        {L"Wi-Fi", L"Intel(R) Wi-Fi 6 AX201", true, L"192.168.1.8",
         L"aabbccddeeff"},
        {L"\u4EE5\u592A\u7F51", L"Intel(R) Ethernet Controller", true,
         L"10.20.30.40", L"001122aabbcc"},
    };
    const auto identity = SelectPortalNetworkIdentity(adapters);
    assert(identity.ipv4 == L"10.20.30.40");
    assert(identity.mac == L"001122aabbcc");
    assert(BuildPortalQueryString(identity) ==
           L"wlanuserip=10.20.30.40&wlanacname=HHXY_XXL2F_HUIJU"
           L"&t=wireless-v2-plain&nasip=2.2.2.230&mac=001122aabbcc");

    const std::vector<PortalAdapterInfo> fallbackAdapters{
        {L"vEthernet (WSL)", L"Hyper-V Virtual Ethernet Adapter", true,
         L"172.23.128.1", L"00155d000001"},
        {L"Ethernet 2", L"USB Ethernet", false, L"169.254.10.2",
         L"112233445566"},
        {L"Loopback Pseudo-Interface 1", L"Loopback", false, L"127.0.0.1",
         L"000000000000"},
        {L"Wi-Fi", L"Intel(R) Wi-Fi 6 AX201", true, L"192.168.1.8",
         L"aabbccddeeff"},
    };
    const auto fallback = SelectPortalNetworkIdentity(fallbackAdapters);
    assert(fallback.ipv4 == L"192.168.1.8");
    assert(fallback.mac == L"aabbccddeeff");

    const auto redirected =
        ClassifyLoginResponse(302, L"http://1.1.1.1/next", {});
    assert(redirected.outcome == HttpLoginOutcome::Redirected);

    assert(ClassifyInternetResponse(204, 204).reachable);
    assert(!ClassifyInternetResponse(200, 204).reachable);
    assert(ClassifyInternetResponse(200, 200).reachable);
    assert(!ClassifyInternetResponse(0, 200).reachable);

    assert(ParsePublicIpResponse(L"198.51.100.42\r\n") ==
           L"198.51.100.42");
    assert(ParsePublicIpResponse(L"2001:db8::5\n") == L"2001:db8::5");
    assert(ParsePublicIpResponse(L"999.1.1.1").empty());
    assert(ParsePublicIpResponse(L"<html>captive portal</html>").empty());
    return 0;
}
