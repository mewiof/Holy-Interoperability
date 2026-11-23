#include "core/common/strings.h"

#include <windows.h>

namespace holy {
namespace {

template <typename Char>
Char fold(Char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<Char>(c - 'A' + 'a') : c;
}

template <typename View>
bool iequals_impl(View a, View b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (fold(a[i]) != fold(b[i])) return false;
  }
  return true;
}

}  // namespace

std::wstring widen(std::string_view utf8) {
  if (utf8.empty()) return {};
  const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  if (needed <= 0) return {};
  std::wstring out(static_cast<size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed);
  return out;
}

std::string narrow(std::wstring_view utf16) {
  if (utf16.empty()) return {};
  const int needed = WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()), nullptr, 0,
                                         nullptr, nullptr);
  if (needed <= 0) return {};
  std::string out(static_cast<size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()), out.data(), needed, nullptr,
                      nullptr);
  return out;
}

std::string to_lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = fold(c);
  return out;
}

std::wstring to_lower(std::wstring_view text) {
  std::wstring out(text);
  for (wchar_t& c : out) c = fold(c);
  return out;
}

bool iequals(std::string_view a, std::string_view b) {
  return iequals_impl(a, b);
}
bool iequals(std::wstring_view a, std::wstring_view b) {
  return iequals_impl(a, b);
}

bool istarts_with(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && iequals_impl(text.substr(0, prefix.size()), prefix);
}

bool istarts_with(std::wstring_view text, std::wstring_view prefix) {
  return text.size() >= prefix.size() && iequals_impl(text.substr(0, prefix.size()), prefix);
}

bool iends_with(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() && iequals_impl(text.substr(text.size() - suffix.size()), suffix);
}

std::vector<std::string> split(std::string_view text, char separator) {
  std::vector<std::string> parts;
  size_t start = 0;
  while (start <= text.size()) {
    const size_t end = text.find(separator, start);
    const std::string_view piece = text.substr(start, end == std::string_view::npos ? end : end - start);
    if (!piece.empty()) parts.emplace_back(piece);
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return parts;
}

bool wildcard_match(std::string_view pattern, std::string_view text) {
  size_t p = 0, t = 0, star = std::string_view::npos, mark = 0;
  while (t < text.size()) {
    if (p < pattern.size() && pattern[p] != '*' && fold(pattern[p]) == fold(text[t])) {
      ++p;
      ++t;
    } else if (p < pattern.size() && pattern[p] == '*') {
      star = p++;
      mark = t;
    } else if (star != std::string_view::npos) {
      p = star + 1;
      t = ++mark;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '*') ++p;
  return p == pattern.size();
}

}  // namespace holy
