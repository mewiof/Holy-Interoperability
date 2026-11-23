#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace holy {

std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view utf16);

// ASCII-only case folding: PE import names and manifest tokens are ASCII, and
// locale-dependent folding would make lookups depend on the host's settings.
std::string to_lower(std::string_view text);
std::wstring to_lower(std::wstring_view text);
bool iequals(std::string_view a, std::string_view b);
bool iequals(std::wstring_view a, std::wstring_view b);
bool istarts_with(std::string_view text, std::string_view prefix);
bool istarts_with(std::wstring_view text, std::wstring_view prefix);
bool iends_with(std::string_view text, std::string_view suffix);

std::vector<std::string> split(std::string_view text, char separator);

// '*' wildcard match, ASCII case-insensitive.
bool wildcard_match(std::string_view pattern, std::string_view text);

}  // namespace holy
