#pragma once
#include "json.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <string_view>

namespace wetype {
// Functional mappings verified against the native Linux client. The user's
// original artwork is installed separately, never shipped in WeTypeX packages.
class WeChatEmoticons {
public:
  static constexpr const char *FontFamily = "WeTypeX WeChat Emoticons v1";
  struct Entry {
    std::string commit;
    uint32_t codepoint;
  };
  explicit WeChatEmoticons(const std::filesystem::path &path) {
    std::ifstream input(path);
    std::string bytes(1048577, '\0');
    input.read(bytes.data(), bytes.size());
    bytes.resize(input.gcount());
    if (bytes.size() > 1048576)
      return;
    auto json = wire::parse(bytes);
    if (wire::number(json.get(), "version") != 1 ||
        wire::str(json.get(), "font_family") != FontFamily)
      return;
    auto *entries = wire::get(json.get(), "entries");
    if (!entries || !json_object_is_type(entries, json_type_array) ||
        json_object_array_length(entries) > 256)
      return;
    for (size_t i = 0; i < json_object_array_length(entries); ++i) {
      auto *row = json_object_array_get_idx(entries, i);
      auto commit = wire::str(row, "commit");
      const auto codepoint = wire::number(row, "codepoint");
      auto *aliases = wire::get(row, "aliases");
      if (commit.size() < 3 || commit.size() > 256 || commit.front() != '[' ||
          commit.back() != ']' || codepoint < 0x10f000 ||
          codepoint > 0x10f0ff || !aliases ||
          !json_object_is_type(aliases, json_type_array))
        continue;
      for (size_t a = 0;
           a < std::min<size_t>(32, json_object_array_length(aliases)); ++a) {
        auto *alias = json_object_array_get_idx(aliases, a);
        if (json_object_is_type(alias, json_type_string)) {
          std::string name = json_object_get_string(alias);
          if (!name.empty() && name.size() <= 256) {
            entryInitials_[static_cast<unsigned char>(name.front())] = true;
            entries_.emplace(std::move(name),
                             Entry{commit, uint32_t(codepoint)});
          }
        }
      }
    }
    auto *unsupported = wire::get(json.get(), "unsupported_aliases");
    if (unsupported && json_object_is_type(unsupported, json_type_array))
      for (size_t i = 0;
           i < std::min<size_t>(256, json_object_array_length(unsupported));
           ++i) {
        auto *value = json_object_array_get_idx(unsupported, i);
        if (json_object_is_type(value, json_type_string)) {
          const std::string text = json_object_get_string(value);
          if (!text.empty())
            unsupportedInitials_[static_cast<unsigned char>(text.front())] = true;
          unsupported_.emplace(text);
        }
      }
  }
  bool ready() const { return !entries_.empty(); }
  const Entry *find(std::string_view text) const {
    if (text.empty() || !entryInitials_[static_cast<unsigned char>(text.front())])
      return nullptr;
    const auto it = entries_.find(text);
    return it == entries_.end() ? nullptr : &it->second;
  }
  bool hidden(std::string_view text, bool inWeChat) const {
    if (!text.empty()) {
      const auto first = static_cast<unsigned char>(text.front());
      if (!unsupportedInitials_[first] && (inWeChat || !entryInitials_[first]))
        return false;
    }
    return unsupported_.find(text) != unsupported_.end() ||
           (!inWeChat && find(text));
  }
  std::string commit(const std::string &text, bool inWeChat) const {
    const auto *entry = inWeChat ? find(text) : nullptr;
    return entry ? entry->commit : text;
  }
  static bool isWeChat(std::string program) {
    const auto slash = program.find_last_of('/');
    if (slash != std::string::npos)
      program.erase(0, slash + 1);
    std::transform(program.begin(), program.end(), program.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    constexpr std::string_view desktop = ".desktop";
    if (program.size() > desktop.size() &&
        program.compare(program.size() - desktop.size(), desktop.size(),
                        desktop) == 0)
      program.resize(program.size() - desktop.size());
    // WeChatAppEx is the browser. Titles and unknown Wayland apps do not count.
    return program == "wechat" || program == "weixin" || program == "xwechat" ||
           program == "com.tencent.wechat" || program == "com.tencent.weixin";
  }

private:
  std::array<bool, 256> entryInitials_{};
  std::array<bool, 256> unsupportedInitials_{};
  std::map<std::string, Entry, std::less<>> entries_;
  std::set<std::string, std::less<>> unsupported_;
};
} // namespace wetype
