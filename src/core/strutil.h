#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace pm {

// UTF-8 everywhere internally; UTF-16 only at the Win32 boundary.
std::wstring widen(std::string_view utf8);
std::string  narrow(std::wstring_view utf16);

std::string_view trim(std::string_view s);
std::vector<std::string_view> splitLines(std::string_view text);

bool iequals(std::string_view a, std::string_view b);
bool icontains(std::string_view haystack, std::string_view needle);
bool istartsWith(std::string_view haystack, std::string_view prefix);

std::string toLower(std::string_view s);
std::string toUpper(std::string_view s);

} // namespace pm
