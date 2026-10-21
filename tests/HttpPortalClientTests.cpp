#include "HttpPortalClient.h"

#include <cassert>

int RunHttpPortalClientTests() {
    const auto accepted =
        ClassifyLoginResponse(200, {}, L"<html>login accepted</html>");
    assert(accepted.outcome == HttpLoginOutcome::Success);

    const auto rejected = ClassifyLoginResponse(
        200, {}, L"<form id=\"goLoginForm\"><input name=\"passwd\"></form>");
    assert(rejected.outcome == HttpLoginOutcome::LoginRejected);

    const auto redirected =
        ClassifyLoginResponse(302, L"http://1.1.1.1/next", {});
    assert(redirected.outcome == HttpLoginOutcome::Redirected);

    assert(ClassifyInternetResponse(204, 204).reachable);
    assert(!ClassifyInternetResponse(200, 204).reachable);
    assert(ClassifyInternetResponse(200, 200).reachable);
    assert(!ClassifyInternetResponse(0, 200).reachable);
    return 0;
}
