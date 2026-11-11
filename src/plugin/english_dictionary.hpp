#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fcitx-utils/standardpath.h>
#include <fcitx-utils/utf8.h>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wetype {

class EnglishDictionary {
public:
  EnglishDictionary()
      : entries_(readDictionary(fcitx::StandardPath::global().locate(
            fcitx::StandardPath::Type::PkgData, "spell/en_dict.fscd"))) {}

  bool available() const { return !entries_.empty(); }

  std::vector<std::string> complete(std::string_view prefix, size_t limit) const {
    std::string lowerPrefix(prefix);
    std::transform(lowerPrefix.begin(), lowerPrefix.end(), lowerPrefix.begin(),
                   lowerAscii);
    auto first = std::lower_bound(entries_.begin(), entries_.end(), lowerPrefix,
                                  [](const Entry &entry, const std::string &key) {
                                    return entry.word < key;
                                  });
    std::vector<const Entry *> matches;
    for (auto it = first;
         it != entries_.end() && !it->word.compare(0, prefix.size(), lowerPrefix);
         ++it)
      matches.push_back(&*it);

    const auto count = std::min(limit, matches.size());
    std::partial_sort(matches.begin(), matches.begin() + count, matches.end(),
                      [length = prefix.size()](const Entry *a, const Entry *b) {
                        const auto aScore = score(*a, length);
                        const auto bScore = score(*b, length);
                        return aScore != bScore ? aScore > bScore : a->word < b->word;
                      });
    const bool uppercase = std::count_if(prefix.begin(), prefix.end(), isUpper) > 1 &&
                           std::none_of(prefix.begin(), prefix.end(), isLower);
    std::vector<std::string> result;
    result.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      auto word = matches[i]->word;
      if (uppercase)
        std::transform(word.begin() + prefix.size(), word.end(),
                       word.begin() + prefix.size(), upperAscii);
      word.replace(0, prefix.size(), prefix);
      result.push_back(std::move(word));
    }
    return result;
  }

private:
  struct Entry {
    std::string word;
    int frequency;
  };

  static bool isUpper(char c) { return c >= 'A' && c <= 'Z'; }
  static bool isLower(char c) { return c >= 'a' && c <= 'z'; }
  static char lowerAscii(char c) { return isUpper(c) ? c + ('a' - 'A') : c; }
  static char upperAscii(char c) { return isLower(c) ? c - ('a' - 'A') : c; }

  static int64_t score(const Entry &entry, size_t prefixLength) {
    const auto extra = int64_t(entry.word.size() - prefixLength);
    // Prefer a complete common word and penalize longer completions slightly.
    const int exactBonus = extra == 0 ? std::min(2048, entry.frequency / 4) : 0;
    return entry.frequency + exactBonus - extra * 384;
  }

  static uint32_t littleEndian(std::string_view bytes) {
    uint32_t value = 0;
    for (size_t i = 0; i < bytes.size(); ++i)
      value |= uint32_t(static_cast<unsigned char>(bytes[i])) << (i * 8);
    return value;
  }

  static std::vector<Entry> readDictionary(const std::string &path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    constexpr size_t headerSize = 12;
    const std::streamoff size = input.tellg();
    if (size < std::streamoff(headerSize) || size > 16 * 1024 * 1024)
      return {};
    std::string data(size_t(size), '\0');
    input.seekg(0);
    if (!input.read(data.data(), size) || data.compare(0, 8, "FSCD0000"))
      return {};

    // FSCD0000 stores a little-endian count followed by frequency and word.
    const std::string_view bytes(data);
    const auto count = littleEndian(bytes.substr(8, 4));
    if (count > (data.size() - headerSize) / 4)
      return {};
    std::vector<Entry> entries;
    entries.reserve(count);
    for (size_t pos = headerSize; pos < data.size();) {
      if (data.size() - pos < 4 || entries.size() == count)
        return {};
      const auto end = data.find('\0', pos + 2);
      if (end == std::string::npos || end == pos + 2)
        return {};
      auto word = data.substr(pos + 2, end - pos - 2);
      if (!fcitx::utf8::validate(word) ||
          std::any_of(word.begin(), word.end(),
                      [](unsigned char c) { return c <= ' ' || c == 0x7f; }))
        return {};
      std::transform(word.begin(), word.end(), word.begin(), lowerAscii);
      entries.push_back({std::move(word), int(littleEndian(bytes.substr(pos, 2)))});
      pos = end + 1;
    }
    if (entries.size() != count)
      return {};
    const auto byWord = [](const Entry &a, const Entry &b) {
      return a.word != b.word ? a.word < b.word : a.frequency > b.frequency;
    };
    if (!std::is_sorted(entries.begin(), entries.end(), byWord))
      std::sort(entries.begin(), entries.end(), byWord);
    entries.erase(std::unique(entries.begin(), entries.end(),
                              [](const Entry &a, const Entry &b) {
                                return a.word == b.word;
                              }),
                  entries.end());
    return entries;
  }

  std::vector<Entry> entries_;
};

} // namespace wetype
