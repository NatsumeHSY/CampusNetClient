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

    PortalForm scriptForm;
    scriptForm.fields = {
        {L"service", L"\u7535\u4FE1"},
        {L"queryString",
         L"wlanuserip=10.20.30.40&wlanacname=HHXY_XXL2F_HUIJU&t=wireless-v2-plain&nasip=2.2.2.230&mac=001122aabbcc"},
    };
    const std::wstring body =
        BuildLoginBody(scriptForm, {L"student@school", L"p+ss word"});
    assert(body ==
           L"userId=student%2540school&password=p%252Bss%2520word&"
           L"service=%25E7%2594%25B5%25E4%25BF%25A1&"
           L"queryString=wlanuserip%253D10.20.30.40%2526"
           L"wlanacname%253DHHXY_XXL2F_HUIJU%2526"
           L"t%253Dwireless-v2-plain%2526nasip%253D2.2.2.230%2526"
           L"mac%253D001122aabbcc&operatorPwd=&operatorUserId=&"
           L"validcode=&passwordEncrypt=false");

    assert(!ParseLoginForm(L"<html><body>no login form</body></html>")
                .has_value());
    return 0;
}
