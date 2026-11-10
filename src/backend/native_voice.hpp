#pragma once
// C ABI verified against the pinned voice_engine_c_api.cpp wrappers. The
// original engine owns Opus, VAD, request sequencing, retries and smoothing.
namespace native_voice {
static std::mutex outputMutex;
static std::string token;
static FILE *output = nullptr;
static void emit(wire::Json event) {
  std::lock_guard lock(outputMutex);
  wire::put(event.get(), "token", token);
  fprintf(output, "%s\n", wire::dump(event.get()).c_str());
  fflush(output);
}
static wire::Json event(const char *name) {
  auto value = wire::object();
  wire::put(value.get(), "event", std::string(name));
  return value;
}
// The first string is the voice ID; the second is the displayed recognition
// text. Treating the voice ID as text would expose an identifier as preedit.
static void result(void *, const char *, uint32_t, const char *text,
                   uint32_t size, int, int, int, const char *, uint32_t, int,
                   int) {
  if (!text || size > 65536)
    return;
  auto value = event("partial");
  wire::put(value.get(), "text", std::string(text, size));
  emit(std::move(value));
}
static void finished(void *, const char *text, uint32_t size, int reason) {
  auto value = event("final");
  if (text && size <= 65536)
    wire::put(value.get(), "text", std::string(text, size));
  wire::put(value.get(), "reason", int64_t(reason));
  emit(std::move(value));
}
static void state(void *, int state) {
  auto value = event("state");
  wire::put(value.get(), "state", int64_t(state));
  emit(std::move(value));
}
static void error(void *, int code, const char *message, uint32_t size,
                  int source) {
  auto value = event("error");
  wire::put(value.get(), "code", int64_t(code));
  wire::put(value.get(), "source", int64_t(source));
  if (message && size <= 4096)
    wire::put(value.get(), "message", std::string(message, size));
  emit(std::move(value));
}
static void vad(void *, int speaking) {
  auto value = event("vad");
  wire::boolean(value.get(), "speaking", speaking != 0);
  emit(std::move(value));
}
static void polish(void *) { emit(event("polishing")); }
struct Callbacks {
  void *context;
  decltype(&result) recognition;
  decltype(&finished) finish;
  decltype(&state) changed;
  decltype(&error) failure;
  decltype(&vad) speech;
  decltype(&polish) polishing;
  // 3.x appends optional recap, network and performance callbacks. Older
  // wrappers copy the original prefix; both ABIs pass these aggregates in
  // memory. Supply the complete zero-initialized tail so a newer wrapper never
  // reads unrelated stack bytes as callback addresses.
  void (*recap)(void *, const char *, uint32_t) = nullptr;
  void (*network)(void *, bool) = nullptr;
  void (*performance)(void *, int, uint64_t) = nullptr;
};
static_assert(offsetof(Callbacks, recap) == 56);
static_assert(sizeof(Callbacks) == 80);
static int nibble(char c) {
  return c >= '0' && c <= '9'   ? c - '0'
         : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                : -1;
}
static void run() {
  output = fdopen(dup(STDOUT_FILENO), "w");
  if (!output || dup2(STDERR_FILENO, STDOUT_FILENO) < 0)
    _exit(78);
  for (const auto *name :
       {"_voice_engine_create", "_voice_engine_destroy",
        "_voice_engine_set_callbacks", "_voice_engine_start_session",
        "_voice_engine_feed_pcm_data", "_voice_engine_stop_recording",
        "_voice_engine_stop_immediately", "_voice_engine_reset",
        "_voice_engine_init_vad"})
    if (!syms.count(name)) {
      fputs("Original native voice interface is unavailable.\n", stderr);
      _exit(78);
    }
  auto *engine = ((void *(*)())syms.at("_voice_engine_create"))();
  if (!engine)
    _exit(78);
  ((void (*)(void *, Callbacks))syms.at("_voice_engine_set_callbacks"))(
      engine, {nullptr, result, finished, state, error, vad, polish});
  bool vadReady = false;
  if (const char *directory = getenv("WETYPE_VAD_DIR")) {
    const auto root = std::filesystem::path(directory);
    const auto config = root / "configure.16k.20221116.offline";
    if (access(config.c_str(), R_OK) == 0 &&
        access((root / "model_16k_20210121_iter07.bin").c_str(), R_OK) == 0 &&
        access((root / "CMS_40dim_chi.bin").c_str(), R_OK) == 0)
      vadReady = ((bool (*)(void *, const char *))syms.at(
          "_voice_engine_init_vad"))(engine, config.c_str());
  }
  auto ready = event("ready");
  wire::boolean(ready.get(), "vad", vadReady);
  emit(std::move(ready));
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.size() > 131072)
      break;
    auto request = wire::parse(line);
    const auto op = wire::str(request.get(), "op");
    if (op == "start") {
      ((void (*)(void *))syms.at("_voice_engine_reset"))(engine);
      {
        std::lock_guard lock(outputMutex);
        token = wire::str(request.get(), "token");
      }
      // The native desktop's fallback phase-A timeout is 1500 ms. Keep the
      // engine's own end polling and final/aggressive-result selection.
      ((void (*)(void *, int))syms.at("_voice_engine_set_phase_a_timeout"))(
          engine, 1500);
      const bool polish = wire::number(request.get(), "polish", 1);
      const auto uin = std::to_string(business_init_uin.load());
      ((void (*)(void *, bool, const char *, bool, bool, bool))syms.at(
          "_voice_engine_start_session"))(
          engine, wire::number(request.get(), "traditional", 0), uin.c_str(),
          polish, polish, polish);
      auto started = event("started");
      emit(std::move(started));
    } else if (wire::str(request.get(), "token") != token) {
      continue;
    } else if (op == "pcm") {
      const auto hex = wire::str(request.get(), "data");
      if (hex.size() % 4 || hex.size() > 1280)
        break;
      std::vector<int16_t> samples(hex.size() / 4);
      for (size_t i = 0; i < hex.size(); i += 4) {
        int a = nibble(hex[i]), b = nibble(hex[i + 1]), c = nibble(hex[i + 2]),
            d = nibble(hex[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0)
          _exit(90);
        samples[i / 4] = int16_t((a << 4) | b | (c << 12) | (d << 8));
      }
      ((void (*)(void *, const int16_t *, int))syms.at(
          "_voice_engine_feed_pcm_data"))(engine, samples.data(),
                                          samples.size() * sizeof(int16_t));
    } else if (op == "stop" || op == "finish") {
      ((void (*)(void *))syms.at(op == "finish"
                                     ? "_voice_engine_stop_immediately"
                                     : "_voice_engine_stop_recording"))(engine);
    } else if (op == "cancel") {
      ((void (*)(void *))syms.at("_voice_engine_reset"))(engine);
      emit(event("cancelled"));
    } else if (op == "quit")
      break;
  }
  ((void (*)(void *))syms.at("_voice_engine_reset"))(engine);
  ((void (*)(void *))syms.at("_voice_engine_destroy"))(engine);
}
} // namespace native_voice
