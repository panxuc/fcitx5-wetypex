// Translate libc++ ctype masks at the Darwin/Linux ABI boundary.
// The pinned original core uses the C locale at this ABI boundary.
#include <array>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <locale>
#include <map>
#include <mutex>
#include <string>
#if defined(__aarch64__)
// Darwin ARM64 long double is binary64; Linux ARM64 long double is binary128.
// Preserve the original argument width and the private libc++ string ABI.
std::string wetype_to_string_long_double(double value)
    asm("wetype_to_string_long_double");
std::string wetype_to_string_long_double(double value) {
  return std::to_string(value);
}
#endif
static uint32_t darwin_mask(std::ctype_base::mask m) {
  uint32_t r = 0;
  const std::pair<std::ctype_base::mask, uint32_t> pairs[] = {
      {std::ctype_base::alpha, 0x100},   {std::ctype_base::cntrl, 0x200},
      {std::ctype_base::digit, 0x400},   {std::ctype_base::lower, 0x1000},
      {std::ctype_base::punct, 0x2000},  {std::ctype_base::space, 0x4000},
      {std::ctype_base::upper, 0x8000},  {std::ctype_base::xdigit, 0x10000},
      {std::ctype_base::blank, 0x20000}, {std::ctype_base::print, 0x40000}};
  for (auto [bit, value] : pairs)
    if ((m & bit) == bit)
      r |= value;
  if (m & 0x80)
    r |= 0x80; // libc++'s regex-only word mask in these builds.
  return r;
}
extern "C" void *wetype_locale_use_facet(void *locale, void *id) {
  static void *h = dlopen("libc++.so.1", RTLD_NOW);
  static auto fn = (void *(*)(void *, void *))dlsym(
      h, "_ZNKSt3__16locale9use_facetERNS0_2idE");
  void *facet = fn(locale, id);
  if (id != (void *)&std::ctype<char>::id)
    return facet;
  struct Clone {
    alignas(16) unsigned char object[64];
    std::array<uint32_t, 256> table;
  };
  static std::mutex mutex;
  static std::map<void *, Clone *> clones;
  std::lock_guard<std::mutex> lock(mutex);
  auto &c = clones[facet];
  if (!c) {
    c = new Clone;
    static_assert(sizeof(std::ctype<char>) <= 64);
    memcpy(c->object, facet, sizeof(std::ctype<char>));
    const auto *table =
        std::use_facet<std::ctype<char>>(*(std::locale *)locale).table();
    for (unsigned i = 0; i < 256; i++)
      c->table[i] = darwin_mask(table[i]);
    void *p = c->table.data();
    memcpy(c->object + 16, &p, 8);
  }
  return c->object;
}
extern "C" uint32_t wetype_get_classname(const char *s, bool ignorecase) {
  static void *h = dlopen("libc++.so.1", RTLD_NOW);
  static auto fn = (std::ctype_base::mask (*)(const char *, bool))dlsym(
      h, "_ZNSt3__115__get_classnameEPKcb");
  return darwin_mask(fn(s, ignorecase));
}
