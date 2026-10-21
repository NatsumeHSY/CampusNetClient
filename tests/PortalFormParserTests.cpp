#include "PortalFormParser.h"

#include <cassert>

namespace {

bool HasField(const PortalForm& form, std::wstring_view name,
             std::wstring_view value) {
    for (const auto& field : form.fields) {
        if (field.name == name && field.value == value) {
            return true;
        }
    }
    return false;
}

} // namespace

int RunPortalFormParserTests() {
    const std::wstring html = LR"(
        <html><body>
        <form id="goLoginForm" action="/webauth.do?urlParameter=abc" method="post">
          <input type="hidden" name="userId" value="old-user">
          <input type="hidden" name="urlParameter" value="abc&amp;act=LOGINSUCC">
          <input type="hidden" name="queryString" value="a=1&amp;b=2">
          <input type="hidden" name="other1" value="presentation">
          <input type="password" name="passwd" value="">
          <input type="submit" name="submit" value="登录">
        </form>
        </body></html>)";

    const auto form = ParseLoginForm(html);
    assert(form.has_value());
    assert(form->action == L"/webauth.do?urlParameter=abc");
    assert(HasField(*form, L"urlParameter", L"abc&act=LOGINSUCC"));
    assert(HasField(*form, L"queryString", L"a=1&b=2"));

    const auto action = ResolveUrl(L"http://1.1.1.1:8888/login/index.html",
                                   form->action);
    assert(action == L"http://1.1.1.1:8888/webauth.do?urlParameter=abc");

    const std::wstring body =
        BuildLoginBody(*form, {L"student@school", L"p&word"});
    assert(body.find(L"userId=student%40school") != std::wstring::npos);
    assert(body.find(L"passwd=p%26word") != std::wstring::npos);
    assert(body.find(L"presentation") == std::wstring::npos);
    assert(body.find(L"urlParameter=") == std::wstring::npos);

    assert(!ParseLoginForm(L"<html><body>no login form</body></html>")
                .has_value());
    return 0;
}
