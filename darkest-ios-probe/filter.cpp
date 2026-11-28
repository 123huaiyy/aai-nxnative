
#include <regex>
#include <optional>
#include <cwchar>
#include <string>
#include <cstdio>
[[gnu::used]] static auto* const compare_wide = &::wmemcmp;
int main() {
    constexpr char16_t pattern[] = u"assets/[^/]+\\.(json|xml)";
    const auto wide = std::u16string(pattern);
    std::optional<std::wregex> regex;
    regex.emplace(wide.begin(), wide.end(), std::regex::ECMAScript);
    if (!std::regex_match(std::wstring(L"assets/party.json"), *regex)) return 1;
    if (std::regex_match(std::wstring(L"assets/party.png"), *regex)) return 2;
    if (std::regex_match(std::wstring(L"assets/x/party.json"), *regex)) return 3;
    std::puts("Host wide file-filter matches passed.");
}
