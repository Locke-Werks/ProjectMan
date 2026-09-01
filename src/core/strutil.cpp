#include "strutil.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>

namespace pm {

std::wstring widen(std::string_view utf8)
{
    if (utf8.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                      static_cast<int>(utf8.size()), nullptr, 0);
    if (n <= 0)
        return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                        out.data(), n);
    return out;
}

std::string narrow(std::wstring_view utf16)
{
    if (utf16.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, utf16.data(),
                                      static_cast<int>(utf16.size()),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0)
        return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

std::string_view trim(std::string_view s)
{
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    while (!s.empty() && !notSpace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && !notSpace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return s;
}

std::vector<std::string_view> splitLines(std::string_view text)
{
    std::vector<std::string_view> out;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t nl = text.find('\n', start);
        const size_t end = (nl == std::string_view::npos) ? text.size() : nl;
        std::string_view line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        out.push_back(line);
        if (nl == std::string_view::npos)
            break;
        start = nl + 1;
    }
    return out;
}

namespace {
inline char lower(char c)
{
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}
} // namespace

bool iequals(std::string_view a, std::string_view b)
{
    return a.size() == b.size()
        && std::equal(a.begin(), a.end(), b.begin(),
                      [](char x, char y) { return lower(x) == lower(y); });
}

bool istartsWith(std::string_view haystack, std::string_view prefix)
{
    return haystack.size() >= prefix.size()
        && iequals(haystack.substr(0, prefix.size()), prefix);
}

bool icontains(std::string_view haystack, std::string_view needle)
{
    if (needle.empty())
        return true;
    if (needle.size() > haystack.size())
        return false;
    const auto it = std::search(haystack.begin(), haystack.end(),
                                needle.begin(), needle.end(),
                                [](char x, char y) { return lower(x) == lower(y); });
    return it != haystack.end();
}

std::string toLower(std::string_view s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), lower);
    return out;
}

std::string toUpper(std::string_view s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](char c) {
        return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    });
    return out;
}

std::string relativeAge(std::int64_t unixSeconds)
{
    if (unixSeconds <= 0)
        return "-";

    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();

    // Clock skew and a commit dated in the future both land here. Clamping
    // beats printing a negative age.
    std::int64_t d = static_cast<std::int64_t>(now) - unixSeconds;
    if (d < 0)
        d = 0;

    if (d < 60)          return std::to_string(d) + "s";
    if (d < 3600)        return std::to_string(d / 60) + "m";
    if (d < 86400)       return std::to_string(d / 3600) + "h";
    if (d < 86400 * 30)  return std::to_string(d / 86400) + "d";
    if (d < 86400 * 365) return std::to_string(d / (86400 * 30)) + "mo";
    return std::to_string(d / (86400 * 365)) + "y";
}

} // namespace pm
