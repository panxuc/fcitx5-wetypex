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
#ifndef JSON_TOKENER_VALIDATE_UTF8
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
#endif
inline Json parse(const std::string &s) {
  if (s.size() >= INT_MAX)
    return {};
  json_tokener *t = json_tokener_new();
  if (!t)
    return {};
#ifdef JSON_TOKENER_VALIDATE_UTF8
  json_tokener_set_flags(t, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
#else
  if (!validUtf8(s)) {
    json_tokener_free(t);
    return {};
  }
  json_tokener_set_flags(t, JSON_TOKENER_STRICT);
#endif
  auto *p = json_tokener_parse_ex(t, s.c_str(), s.size() + 1);
  bool ok = json_tokener_get_error(t) == json_tokener_success;
#ifdef JSON_TOKENER_VALIDATE_UTF8
  const size_t consumed = json_tokener_get_parse_end(t);
#else
  const size_t consumed = t->char_offset;
#endif
  for (size_t end = consumed; end < s.size(); ++end)
    if (!std::isspace(static_cast<unsigned char>(s[end])))
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
