#pragma once

#include <map>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <utility>

namespace wetype {
inline std::string trimAppearance(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos)
    return {};
  return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

// Classic UI's top-level options share a file with section-specific options.
// Keep comments, quoting and the other sections intact when changing a field.
inline std::optional<std::string> classicUiValue(const std::string &text,
                                                 const std::string &key) {
  std::optional<std::string> value;
  for (size_t begin = 0; begin < text.size();) {
    const auto end = text.find('\n', begin);
    const auto line = trimAppearance(text.substr(begin, end - begin));
    if (!line.empty() && line.front() == '[')
      break;
    const auto equals = line.find('=');
    if (equals != std::string::npos &&
        trimAppearance(line.substr(0, equals)) == key)
      value = trimAppearance(line.substr(equals + 1));
    if (end == std::string::npos)
      break;
    begin = end + 1;
  }
  return value;
}

inline std::string
updateClassicUi(const std::string &text,
                const std::map<std::string, std::string> &values) {
  std::string output;
  std::set<std::string> written;
  const std::string newline =
      text.find("\r\n") == std::string::npos ? "\n" : "\r\n";
  auto appendMissing = [&] {
    for (const auto &[key, value] : values) {
      if (written.insert(key).second) {
        if (!output.empty() && output.back() != '\n')
          output += newline;
        output += key + "=" + value + newline;
      }
    }
  };
  bool root = true;
  for (size_t begin = 0; begin < text.size();) {
    const auto end = text.find('\n', begin);
    const auto raw = text.substr(begin, end - begin);
    const auto line = trimAppearance(raw);
    if (root && !line.empty() && line.front() == '[') {
      appendMissing();
      root = false;
    }
    const auto equals = line.find('=');
    const auto key = equals == std::string::npos
                         ? std::string()
                         : trimAppearance(line.substr(0, equals));
    const auto setting = values.find(key);
    if (root && setting != values.end()) {
      output += key + "=" + setting->second;
      if (!raw.empty() && raw.back() == '\r')
        output += '\r';
      written.insert(key);
    } else {
      output += raw;
    }
    if (end == std::string::npos)
      break;
    output += '\n';
    begin = end + 1;
  }
  appendMissing();
  return output;
}

inline std::string candidateFontWithSize(std::string font, int size) {
  font = trimAppearance(std::move(font));
  const bool quoted =
      font.size() >= 2 && font.front() == '"' && font.back() == '"';
  if (quoted)
    font = font.substr(1, font.size() - 2);
  // Pango places optional variation/feature settings after the font size.
  // Work on the original value so escaped family names and style tokens
  // survive.
  static const std::regex suffixExpression("[ \t]+(?=[@#])");
  static const std::regex sizeExpression(
      "(^|[ \t]+)([0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)(px)?$");
  std::smatch match;
  std::string suffix, unit;
  if (std::regex_search(font, match, suffixExpression)) {
    suffix = font.substr(match.position());
    font.resize(match.position());
  }
  font = trimAppearance(std::move(font));
  if (std::regex_search(font, match, sizeExpression)) {
    unit = match[3];
    font.resize(match.position());
  }
  font = trimAppearance(std::move(font));
  if (font.empty())
    font = "Sans";
  font += " " + std::to_string(size) + unit + suffix;
  return quoted ? '"' + font + '"' : font;
}
} // namespace wetype
