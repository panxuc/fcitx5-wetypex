#pragma once
#if defined(__aarch64__)
// Darwin passes unnamed arguments in eight-byte stack slots. AAPCS64's va_list
// describes register save areas as well; force its cursor into the stack area.
namespace darwin_arm64 {
struct LinuxArguments {
  void *stack, *general, *vector;
  int generalOffset, vectorOffset;
};
static_assert(sizeof(va_list) == sizeof(LinuxArguments));
static void arguments(va_list &output, void *stack) {
  LinuxArguments value{stack, nullptr, nullptr, 0, 0};
  memcpy(&output, &value, sizeof(value));
}
static std::string format64(const char *format, bool scanning = false) {
  std::string result;
  bool specifier = false;
  for (size_t i = 0; format[i]; ++i) {
    const char c = format[i];
    if (specifier && scanning && c == '[') {
      result += c;
      ++i;
      if (format[i] == '^') result += format[i++];
      if (format[i] == ']') result += format[i++];
      while (format[i] && format[i] != ']') result += format[i++];
      if (!format[i]) return result;
      result += ']';
      specifier = false;
      continue;
    }
    if (specifier && c == 'L' && format[i + 1] &&
        strchr("fFeEgGaA", format[i + 1])) {
      if (scanning) result += 'l';
      continue;
    }
    result += c;
    if (!specifier && c == '%') specifier = true;
    else if (specifier && strchr("diouxXfFeEgGaAcCsSpn%m", c)) specifier = false;
  }
  return result;
}
}
extern "C" int darwin_arm_vprintf(const char *format, void *stack) {
  const auto nativeFormat = darwin_arm64::format64(format);
  va_list args;
  darwin_arm64::arguments(args, stack);
  const int result = vprintf(nativeFormat.c_str(), args);
  va_end(args);
  return result;
}
extern "C" int darwin_arm_vfprintf(FILE *file, const char *format, void *stack) {
  const auto nativeFormat = darwin_arm64::format64(format);
  va_list args;
  darwin_arm64::arguments(args, stack);
  const int result = vfprintf(file, nativeFormat.c_str(), args);
  va_end(args);
  return result;
}
extern "C" int darwin_arm_vsnprintf(char *out, size_t size, const char *format,
                                    void *stack) {
  const auto nativeFormat = darwin_arm64::format64(format);
  va_list args;
  darwin_arm64::arguments(args, stack);
  const int result = vsnprintf(out, size, nativeFormat.c_str(), args);
  va_end(args);
  return result;
}
extern "C" int darwin_arm_vsprintf(char *out, const char *format, void *stack) {
  const auto nativeFormat = darwin_arm64::format64(format);
  va_list args;
  darwin_arm64::arguments(args, stack);
  const int result = vsprintf(out, nativeFormat.c_str(), args);
  va_end(args);
  return result;
}
extern "C" int darwin_arm_vasprintf(char **out, const char *format, void *stack) {
  const auto nativeFormat = darwin_arm64::format64(format);
  va_list args;
  darwin_arm64::arguments(args, stack);
  const int result = vasprintf(out, nativeFormat.c_str(), args);
  va_end(args);
  return result;
}
extern "C" int darwin_arm_vsscanf(const char *text, const char *format, void *stack) {
  const auto nativeFormat = darwin_arm64::format64(format, true);
  va_list args;
  darwin_arm64::arguments(args, stack);
  const int result = vsscanf(text, nativeFormat.c_str(), args);
  va_end(args);
  return result;
}
extern "C" __attribute__((used, noinline)) int darwin_arm_open(const char *path, int flags, const void *stack) {
  int mode = 0;
  if (flags & 0x200)
    memcpy(&mode, stack, sizeof(mode));
  return shim_open(path, flags, mode);
}
extern "C" __attribute__((used, noinline)) int darwin_arm_fcntl(int fd, int command, const void *stack) {
  long argument = 0;
  if (command != 1 && command != 3)
    memcpy(&argument, stack, sizeof(argument));
  return shim_fcntl(fd, command, argument);
}
extern "C" __attribute__((used, noinline)) int darwin_arm_ioctl(int fd, unsigned long command, const void *stack) {
  void *argument = nullptr;
  memcpy(&argument, stack, sizeof(argument));
  return shim_ioctl(fd, command, argument);
}
extern "C" void darwin_arm_printf(), darwin_arm_fprintf(), darwin_arm_snprintf(),
    darwin_arm_sprintf(), darwin_arm_asprintf(), darwin_arm_sscanf(),
    darwin_arm_open_entry(), darwin_arm_fcntl_entry(), darwin_arm_ioctl_entry();
asm(".text\n.p2align 2\n"
    ".global darwin_arm_printf\ndarwin_arm_printf:\nmov x1, sp\nb darwin_arm_vprintf\n"
    ".global darwin_arm_fprintf\ndarwin_arm_fprintf:\nmov x2, sp\nb darwin_arm_vfprintf\n"
    ".global darwin_arm_snprintf\ndarwin_arm_snprintf:\nmov x3, sp\nb darwin_arm_vsnprintf\n"
    ".global darwin_arm_sprintf\ndarwin_arm_sprintf:\nmov x2, sp\nb darwin_arm_vsprintf\n"
    ".global darwin_arm_asprintf\ndarwin_arm_asprintf:\nmov x2, sp\nb darwin_arm_vasprintf\n"
    ".global darwin_arm_sscanf\ndarwin_arm_sscanf:\nmov x2, sp\nb darwin_arm_vsscanf\n"
    ".global darwin_arm_open_entry\ndarwin_arm_open_entry:\nmov x2, sp\nb darwin_arm_open\n"
    ".global darwin_arm_fcntl_entry\ndarwin_arm_fcntl_entry:\nmov x2, sp\nb darwin_arm_fcntl\n"
    ".global darwin_arm_ioctl_entry\ndarwin_arm_ioctl_entry:\nmov x2, sp\nb darwin_arm_ioctl\n");
static void *resolve_darwin_arm64(const std::string &name) {
  if (name == "__ZNSt3__19to_stringEe")
    return dlsym(RTLD_DEFAULT, "wetype_to_string_long_double");
  static const std::map<std::string, void *> adapters{
      {"_printf", reinterpret_cast<void *>(darwin_arm_printf)},
      {"_fprintf", reinterpret_cast<void *>(darwin_arm_fprintf)},
      {"_snprintf", reinterpret_cast<void *>(darwin_arm_snprintf)},
      {"_sprintf", reinterpret_cast<void *>(darwin_arm_sprintf)},
      {"_asprintf", reinterpret_cast<void *>(darwin_arm_asprintf)},
      {"_sscanf", reinterpret_cast<void *>(darwin_arm_sscanf)},
      {"_vprintf", reinterpret_cast<void *>(darwin_arm_vprintf)},
      {"_vfprintf", reinterpret_cast<void *>(darwin_arm_vfprintf)},
      {"_vsnprintf", reinterpret_cast<void *>(darwin_arm_vsnprintf)},
      {"_vsprintf", reinterpret_cast<void *>(darwin_arm_vsprintf)},
      {"_vasprintf", reinterpret_cast<void *>(darwin_arm_vasprintf)},
      {"_vsscanf", reinterpret_cast<void *>(darwin_arm_vsscanf)},
      {"_open", reinterpret_cast<void *>(darwin_arm_open_entry)},
      {"_fcntl", reinterpret_cast<void *>(darwin_arm_fcntl_entry)},
      {"_ioctl", reinterpret_cast<void *>(darwin_arm_ioctl_entry)}};
  const auto found = adapters.find(name);
  return found == adapters.end() ? nullptr : found->second;
}
#endif
