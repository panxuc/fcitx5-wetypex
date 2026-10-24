#pragma once
#include <cctype>
#include <climits>
#include <cstdint>
#include <json-c/json.h>
#include <memory>
#include <string>
#include <type_traits>
namespace wire {
struct Deleter {
  void operator()(json_object *p) const {
    if (p)
      json_object_put(p);
  }
};
using Json = std::unique_ptr<json_object, Deleter>;
inline Json object() { return Json(json_object_new_object()); }
inline bool validUtf8(const std::string &text) {
  for (size_t i = 0; i < text.size(); ++i) {
    const auto first = static_cast<unsigned char>(text[i]);
    if (first < 0x80)
      continue;
    if (first < 0xc2 || first > 0xf4)
      return false;
    const size_t tails = first < 0xe0 ? 1 : first < 0xf0 ? 2 : 3;
    if (tails >= text.size() - i)
      return false;
    const auto second = static_cast<unsigned char>(text[i + 1]);
    if ((first == 0xe0 && second < 0xa0) ||
        (first == 0xed && second >= 0xa0) ||
        (first == 0xf0 && second < 0x90) ||
        (first == 0xf4 && second >= 0x90))
      return false;
    for (size_t j = 1; j <= tails; ++j)
      if ((static_cast<unsigned char>(text[i + j]) & 0xc0) != 0x80)
        return false;
    i += tails;
  }
  return true;
}
// json-c 0.18 compares signed UTF-8 bytes with 0x1f in strict mode
// (upstream #875). Keep strict JSON syntax by escaping raw string scalars
// before passing them to that version, including supplementary-plane emoji.
inline std::string asciiJson(const std::string &text) {
  std::string output;
  output.reserve(text.size());
  bool quoted = false, escaped = false;
  const auto append = [&](uint32_t value) {
    constexpr char hex[] = "0123456789abcdef";
    output += "\\u";
    for (int shift : {12, 8, 4, 0})
      output += hex[(value >> shift) & 15];
  };
  for (size_t i = 0; i < text.size(); ++i) {
    const auto c = static_cast<unsigned char>(text[i]);
    if (c < 0x80) {
      output += char(c);
      if (quoted) {
        if (escaped)
          escaped = false;
        else if (c == '\\')
          escaped = true;
        else if (c == '"')
          quoted = false;
      } else if (c == '"')
        quoted = true;
      continue;
    }
    if (!quoted || escaped)
      return {};
    const size_t tails = c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
    uint32_t scalar = c & (tails == 1 ? 0x1f : tails == 2 ? 0xf : 7);
    for (size_t j = 0; j < tails; ++j)
      scalar = (scalar << 6) | (static_cast<unsigned char>(text[++i]) & 0x3f);
    if (scalar > 0xffff) {
      scalar -= 0x10000;
      append(0xd800 + (scalar >> 10));
      append(0xdc00 + (scalar & 0x3ff));
    } else
      append(scalar);
  }
  return output;
}
inline Json parse(const std::string &s) {
  if (s.size() >= INT_MAX || !validUtf8(s))
    return {};
  const bool affected = (json_c_version_num() & 0xffff00) == 0x1200;
  const std::string escaped = affected ? asciiJson(s) : std::string();
  const auto &input = affected ? escaped : s;
  if ((!s.empty() && input.empty()) || input.size() >= INT_MAX)
    return {};
  json_tokener *t = json_tokener_new();
  if (!t)
    return {};
#ifdef JSON_TOKENER_VALIDATE_UTF8
  json_tokener_set_flags(t, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
#else
  json_tokener_set_flags(t, JSON_TOKENER_STRICT);
#endif
  auto *p = json_tokener_parse_ex(t, input.c_str(), input.size() + 1);
  bool ok = json_tokener_get_error(t) == json_tokener_success;
#ifdef JSON_TOKENER_VALIDATE_UTF8
  const size_t consumed = json_tokener_get_parse_end(t);
#else
  const size_t consumed = t->char_offset;
#endif
  for (size_t end = consumed; end < input.size(); ++end)
    if (!std::isspace(static_cast<unsigned char>(input[end])))
      ok = false;
  json_tokener_free(t);
  if (!ok && p)
    json_object_put(p);
  return Json(ok ? p : nullptr);
}
inline json_object *get(json_object *p, const char *k) {
  json_object *v = nullptr;
  if (p)
    json_object_object_get_ex(p, k, &v);
  return v;
}
inline std::string str(json_object *p, const char *k,
                       const char *fallback = "") {
  auto *v = get(p, k);
  return v && json_object_is_type(v, json_type_string)
             ? std::string(json_object_get_string(v),
                           json_object_get_string_len(v))
             : fallback;
}
inline int64_t number(json_object *p, const char *k, int64_t fallback = 0) {
  auto *v = get(p, k);
  // Older callers encode engine flags as 0/1; native configuration stores
  // them as JSON booleans. Accept both representations at the IPC boundary.
  return v && (json_object_is_type(v, json_type_int) ||
               json_object_is_type(v, json_type_boolean))
             ? json_object_get_int64(v)
             : fallback;
}
inline void put(json_object *p, const char *k, const std::string &v) {
  json_object_object_add(p, k, json_object_new_string_len(v.data(), v.size()));
}
inline void put(json_object *p, const char *k, int64_t v) {
  json_object_object_add(p, k, json_object_new_int64(v));
}
template <typename T>
inline std::enable_if_t<std::is_same_v<T, bool>> put(json_object *p,
                                                     const char *k, T v) {
  json_object_object_add(p, k, json_object_new_boolean(v));
}
inline void boolean(json_object *p, const char *k, bool v) {
  json_object_object_add(p, k, json_object_new_boolean(v));
}
inline std::string dump(json_object *p) {
  return json_object_to_json_string_ext(p, JSON_C_TO_STRING_PLAIN);
}
} // namespace wire
