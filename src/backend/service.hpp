// Native engine IPC. All engine operations are serialized on the service
// thread.
#include "control.hpp"
static void configure_group_sync(uint64_t groupId, uint64_t functions,
                                 uint64_t phraseVersion,
                                 uint64_t dictionaryVersion) {
  // macOS 2.2.3.657 / Windows 2.1.3.18: the 40-byte aggregate is passed by
  // value.
  struct GroupSyncInfo {
    bool unknown, debug;
    unsigned char padding[6];
    uint64_t groupId, functions, phraseVersion, dictionaryVersion;
  } info{
      false, false, {}, groupId, functions, phraseVersion, dictionaryVersion};
  static_assert(sizeof(GroupSyncInfo) == 40);
  ((void (*)(GroupSyncInfo))syms.at("_wxime_group_sync_info_changed"))(info);
}
// Verified against +[WXIMEUtil addHotWord:value:] and removeHotWord:.
// The 80-byte aggregate is passed BY VALUE, including in the enumerator
// callback.
struct HotWord {
  const char *id;
  uint32_t idlen, pad0;
  const char *key;
  uint32_t keylen, pad1;
  const char *words;
  uint32_t wordslen, flag;
  const char *file;
  uint32_t filelen, pad2;
  const char *thumb;
  uint32_t thumblen, kind;
};
static_assert(sizeof(HotWord) == 80);
static bool hotword_collect(void *opaque, HotWord item) {
  auto *array = static_cast<json_object *>(opaque);
  if (json_object_array_length(array) >= 10000)
    return false;
  auto row = wire::object();
  for (auto field : {std::tuple{"id", item.id, item.idlen},
                     std::tuple{"key", item.key, item.keylen},
                     std::tuple{"words", item.words, item.wordslen}}) {
    auto [name, data, size] = field;
    if (size > 65536 || (!data && size))
      _exit(89);
    wire::put(row.get(), name, std::string(data ? data : "", size));
  }
  wire::put(row.get(), "flag", int64_t(item.flag));
  wire::put(row.get(), "kind", int64_t(item.kind));
  json_object_array_add(array, row.release());
  return true;
}
struct ServiceCandidate {
  std::string text;
  std::vector<unsigned char> id;
  uint32_t cover = 0;
};
struct ServiceSession {
  uint64_t engine = 0, revision = 0;
  uint32_t keyboard = 0;
  std::string preedit, selected;
  size_t cursor = 0;
  std::string pendingRaw, displayPreedit;
  size_t displayCursor = 0;
  std::vector<size_t> cursorStops;
  unsigned pending = 0;
  bool vMode = false;
  std::mutex mutex;
  std::vector<ServiceCandidate> candidates;
  struct PublishedCandidates {
    uint64_t revision;
    std::vector<ServiceCandidate> candidates;
  };
  std::deque<PublishedCandidates> published;
  std::vector<std::string> commits;
};
// Caller holds the session mutex. Retain the exact candidate IDs sent to the
// client so a cloud refresh cannot change the meaning of a displayed rank.
static void publish_service_candidates(ServiceSession &s) {
  if (s.published.empty() || s.published.back().revision != s.revision)
    s.published.push_back({s.revision, s.candidates});
  else
    s.published.back().candidates = s.candidates;
  while (s.published.size() > 4)
    s.published.pop_front();
}
static void append_pending_scalar(std::string &output, unsigned char value) {
  if (value)
    output.push_back(char(value));
}
static void append_utf8_stops(std::vector<size_t> &stops,
                              const std::string &text, size_t begin) {
  size_t offset = 0;
  while (offset < text.size()) {
    const auto first = static_cast<unsigned char>(text[offset]);
    size_t length = first < 0x80 ? 1 : first < 0xe0 ? 2 : first < 0xf0 ? 3 : 4;
    if (offset + length > text.size())
      break;
    offset += length;
    stops.push_back(begin + offset);
  }
}
static void configure_cloud_session(unsigned char *config) {
  // Windows 2.1.3.18 desktop defaults recovered from wetype_server.exe's
  // SessionConfigRW fallback object and its field serializer.  In particular,
  // the desktop enables cloud candidates while typing, but keeps the mobile-
  // style post-commit "most likely" list disabled.
  config[0x41] = 1;                 // cloud.enable_cloud_search_by_input
  *(uint32_t *)(config + 0x50) = 3; // cloud.process_input.return_cnt
  *(uint32_t *)(config + 0x54) = 1; // cloud.process_input.predict_next_n
}
static void read_service_candidates(void *iterator,
                                    std::vector<ServiceCandidate> &output) {
  output.clear();
  for (unsigned rank = 0; iterator && rank < 50; rank++) {
    alignas(16) unsigned char c[512]{};
    if (!((bool (*)(void *, void *))syms.at("_wxime_candidate_next"))(iterator,
                                                                      c))
      break;
    const char *text;
    const unsigned char *id;
    uint32_t len, n;
    memcpy(&text, c, 8);
    memcpy(&len, c + 8, 4);
    memcpy(&id, c + 0x20, 8);
    memcpy(&n, c + 0x28, 4);
    if (!text || len > 65536 || n > 65536)
      _exit(89);
    ServiceCandidate candidate;
    candidate.text.assign(text, len);
    memcpy(&candidate.cover, c + 0x64, 4);
    if (id && n)
      candidate.id.assign(id, id + n);
    output.push_back(std::move(candidate));
  }
}
static void service_listener(uint64_t, int32_t *types, void **payloads,
                             uint32_t count, void *opaque) {
  auto &s = *(ServiceSession *)opaque;
  if (count > 64 || (!types && count) || (!payloads && count))
    _exit(89);
  std::lock_guard<std::mutex> guard(s.mutex);
  for (unsigned i = 0; i < count; i++) {
    // An empty PendingInput list is represented by a null head pointer.
    if (!payloads[i] && types[i] != 5)
      continue;
    if (types[i] == 0) {
      // CCallbackHolder::ParseEvent: iterator +0, kind +8,
      // cloud iterator +16, version +24 (macOS 2.2.3.657).
      auto it = *(void **)payloads[i];
      auto cloud = *((void **)payloads[i] + 2);
      read_service_candidates(it, s.candidates);
      ++s.revision;
      if (it)
        ((void (*)(void *))syms.at("_wxime_delete_candidate_iterator"))(it);
      if (cloud && cloud != it)
        ((void (*)(void *))syms.at("_wxime_delete_candidate_iterator"))(cloud);
    } else if (types[i] == 6) {
      // _wxime_cloud_result_event: iterator +0, kind +8, version +24.
      // The iterator is already the original engine's fully merged list.
      auto it = *(void **)payloads[i];
      read_service_candidates(it, s.candidates);
      ++s.revision;
      if (it)
        ((void (*)(void *))syms.at("_wxime_delete_candidate_iterator"))(it);
    } else if (types[i] == 2) {
      const char *text = *(const char **)payloads[i];
      uint32_t n = *(uint32_t *)((char *)payloads[i] + 8);
      if (text && n < 65536) {
        s.commits.emplace_back(text, n);
        s.selected.clear();
      }
    } else if (types[i] == 5) {
      // CCallbackHolder::sigPendingInputUpdated emits event 5.  Its payload is
      // the head pointer of PendingInput's singly-linked list; each node keeps
      // the next pointer at +8.  Count it while the callback owns the list.
      void *node = payloads[i];
      unsigned pending = 0;
      std::string raw, display;
      std::vector<size_t> stops{0};
      size_t displayCursor = 0;
      bool hasCursor = false;
      while (node && pending < 4096) {
        ++pending;
        const auto type = *(uint32_t *)node;
        if (type <= 1) {
          const auto length = *(uint32_t *)((char *)node + 24);
          const auto data = *(const char **)((char *)node + 32);
          if (length > 65536 || (!data && length))
            _exit(89);
          std::string value(data ? data : "", length);
          const auto begin = raw.size();
          raw += value;
          display += value;
          // PendingInputSelectedText (type 0) contributes a stop for each
          // selected character. ExactlyMatched text is divided by explicit
          // Separator nodes instead.
          if (type == 0)
            append_utf8_stops(stops, value, begin);
        } else if (type == 2 || type == 4 || type == 5) {
          // Deletion/replacement/exchange records retain the user's original
          // byte at +24 for display; their synthetic correction is not shown.
          const auto value = *(unsigned char *)((char *)node + 24);
          append_pending_scalar(raw, value);
          append_pending_scalar(display, value);
        } else if (type == 6) {
          // Every separator is visible. Only a separator whose stored byte is
          // an apostrophe was explicitly typed and therefore belongs to raw.
          if (stops.empty() || stops.back() != raw.size())
            stops.push_back(raw.size());
          display.push_back('\'');
          if (*(unsigned char *)((char *)node + 24) == '\'')
            raw.push_back('\'');
        } else if (type == 7) {
          displayCursor = display.size();
          hasCursor = true;
        }
        node = *(void **)((char *)node + 8);
      }
      if (node)
        _exit(89);
      s.pending = pending;
      s.pendingRaw = std::move(raw);
      s.displayPreedit = std::move(display);
      s.displayCursor = hasCursor ? displayCursor : s.displayPreedit.size();
      s.cursorStops = std::move(stops);
    }
  }
}
static bool service_select(ServiceSession &s, unsigned rank,
                           int64_t expected = -1) {
  ServiceCandidate c;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    const std::vector<ServiceCandidate> *candidates = &s.candidates;
    if (expected >= 0) {
      const auto snapshot = std::find_if(
          s.published.begin(), s.published.end(), [expected](const auto &item) {
            return item.revision == uint64_t(expected);
          });
      if (snapshot == s.published.end())
        return false;
      candidates = &snapshot->candidates;
    }
    if (rank >= candidates->size())
      return false;
    c = (*candidates)[rank];
    s.published.clear();
  }
  using Fn = void (*)(uint64_t, const char *, uint32_t, const void *, uint32_t,
                      const char *, uint32_t, const void *, uint32_t,
                      const void *, uint32_t, const void *);
  ((Fn)syms.at("_wxime_select_candidate"))(
      s.engine, c.text.data(), c.text.size(), c.id.data(), c.id.size(), nullptr,
      0, nullptr, 0, nullptr, 0, nullptr);
  bool finishVMode = false;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    finishVMode = s.vMode && !s.commits.empty();
    if (c.cover <= s.preedit.size())
      s.preedit.erase(0, c.cover);
    s.cursor = s.preedit.size();
    if (s.pending && s.commits.empty())
      s.selected += c.text;
    if (!s.pending) {
      s.preedit.clear();
      s.selected.clear();
      s.cursor = 0;
      s.pendingRaw.clear();
      s.displayPreedit.clear();
      s.displayCursor = 0;
      s.cursorStops.clear();
    }
  }
  if (finishVMode) {
    // Calculator candidates commit a result without consuming the expression
    // like a pinyin candidate. End VModeV2 before accepting the next key.
    ((void (*)(uint64_t, uint32_t, bool))syms.at(
        "_wxime_session_set_bool_option"))(s.engine, 0x11, false);
    ((void (*)(uint64_t))syms.at("_wxime_reset_session"))(s.engine);
    std::lock_guard<std::mutex> lock(s.mutex);
    s.vMode = false;
    s.pending = 0;
    s.preedit.clear();
    s.selected.clear();
    s.cursor = 0;
    s.pendingRaw.clear();
    s.displayPreedit.clear();
    s.displayCursor = 0;
    s.cursorStops.clear();
    s.candidates.clear();
    ++s.revision;
  }
  return true;
}
static void service_loop() {
  std::map<uint64_t, std::unique_ptr<ServiceSession>> sessions;
  ServiceTransport transport;
  puts("{\"event\":\"ready\",\"protocol\":1,\"backend\":\"original-wetype-"
       "native\"}");
  fflush(stdout);
  std::string line;
  int source = 0;
  while (transport.next(line, source)) {
    if (line.size() > 65536)
      _exit(90);
    auto request = wire::parse(line);
    if (!request || !json_object_is_type(request.get(), json_type_object)) {
      if (source == 0)
        _exit(90);
      transport.reply(source, "{\"error\":\"invalid JSON request\"}");
      continue;
    }
    auto op = wire::str(request.get(), "op");
    auto client = wire::number(request.get(), "session");
    auto seq = wire::number(request.get(), "seq");
    auto epoch = wire::number(request.get(), "epoch");
    if (source != 0 && op != "hotword_list" && op != "hotword_set" &&
        op != "group_sync") {
      transport.reply(source,
                      "{\"error\":\"management operation not allowed\"}");
      continue;
    }
    auto response = wire::object();
    wire::put(response.get(), "session", client);
    wire::put(response.get(), "seq", seq);
    wire::put(response.get(), "epoch", epoch);
    if (op == "quit")
      break;
    if (client <= 0 || seq < 0) {
      wire::put(response.get(), "error", std::string("invalid session"));
    } else if (op == "group_sync") {
      const auto group = wire::number(request.get(), "group_id");
      const auto functions = wire::number(request.get(), "func_switch");
      if (!getenv("WETYPE_NETWORK_LIVE") || group <= 0 || functions < 0) {
        wire::put(response.get(), "error",
                  std::string("network disabled or invalid device group"));
      } else {
        configure_group_sync(group, functions, 0, 0);
        // The native core schedules synchronization asynchronously. This
        // acknowledges the request, not a completed dictionary download.
        wire::boolean(response.get(), "requested", true);
      }
    } else if (op == "hotword_list" || op == "hotword_set") {
      if (op == "hotword_set") {
        auto id = wire::str(request.get(), "id"),
             key = wire::str(request.get(), "key"),
             words = wire::str(request.get(), "words");
        if (id.size() > 256 || key.size() > 256 || words.size() > 16384 ||
            (id.empty() && words.empty()))
          wire::put(response.get(), "error", std::string("invalid hotword"));
        else {
          HotWord item{};
          item.id = id.empty() ? nullptr : id.data();
          item.idlen = id.size();
          item.key = key.empty() ? nullptr : key.data();
          item.keylen = key.size();
          item.words = words.empty() ? nullptr : words.data();
          item.wordslen = words.size();
          item.flag = 0; // Do not transplant Android enum values into this ABI.
          bool ok =
              ((bool (*)(HotWord))syms.at("_wxime_set_user_hot_word"))(item);
          if (!ok)
            wire::put(response.get(), "error",
                      std::string("original engine rejected hotword"));
        }
      }
      auto *array = json_object_new_array();
      ((void (*)(decltype(&hotword_collect), void *))syms.at(
          "_wxime_get_user_hot_word"))(hotword_collect, array);
      json_object_object_add(response.get(), "hotwords", array);
    } else if (op == "close") {
      auto it = sessions.find(client);
      if (it != sessions.end()) {
        ((void (*)(uint64_t))syms.at("_wxime_destroy_session"))(
            it->second->engine);
        sessions.erase(it);
      }
    } else {
      if (!sessions.count(client)) {
        if (sessions.size() >= 64) {
          wire::put(response.get(), "error", std::string("session limit"));
          transport.reply(source, wire::dump(response.get()));
          continue;
        }
        auto state = std::make_unique<ServiceSession>();
        alignas(16) unsigned char config[2048]{};
        config[1] = 1;
        if (getenv("WETYPE_NETWORK_LIVE"))
          configure_cloud_session(config);
        uint32_t keyboard =
            wire::number(request.get(), "keyboard", 0) == 5 ? 5 : 0;
        memcpy(config + 4, &keyboard, sizeof(keyboard));
        uint32_t doublePin = std::clamp<int>(
            wire::number(request.get(), "double_scheme", 0), 0, 7);
        memcpy(config + 0x20, &doublePin, sizeof(doublePin));
        // SessionCreator copies config[0x24..0x2a] into
        // WubiSessionV2::InitArgs. The first four bytes are the WubiSolution
        // enum (86/98/New Century).
        uint32_t wubiSolution = std::clamp<int>(
            wire::number(request.get(), "wubi_solution", 0), 0, 2);
        memcpy(config + 0x24, &wubiSolution, sizeof(wubiSolution));
        config[0x28] = wire::number(request.get(), "wubi_pinyin", 0);
        config[0x29] = wire::number(request.get(), "wubi_unique_commit", 0);
        config[0x2a] = wire::number(request.get(), "wubi_next_commit", 0);
        config[0xaf] = wire::number(request.get(), "wubi_wildcard_comment", 0);
        if (wire::number(request.get(), "smart_input", 1)) {
          // InputAugmenter: transpose, neighbouring-key, insertion and skip
          // correction. Offsets are recovered from the original JNI bridge.
          for (unsigned offset : {0x8u, 0x9u, 0xau, 0xbu, 0xcu})
            config[offset] = 1;
        }
        config[0x0d] = wire::number(request.get(), "fuzzy_nl", 0);
        config[0x0e] = wire::number(request.get(), "fuzzy_rl", 0);
        config[0x0f] = wire::number(request.get(), "fuzzy_hf", 0);
        config[0x10] = wire::number(request.get(), "fuzzy_gk", 0);
        config[0x11] = wire::number(request.get(), "fuzzy_an_ang", 0);
        config[0x12] = wire::number(request.get(), "fuzzy_ian_iang", 0);
        config[0x13] = wire::number(request.get(), "fuzzy_uan_uang", 0);
        config[0x14] = wire::number(request.get(), "fuzzy_c_ch", 0);
        config[0x15] = wire::number(request.get(), "fuzzy_s_sh", 0);
        config[0x16] = wire::number(request.get(), "fuzzy_z_zh", 0);
        config[0x17] = wire::number(request.get(), "fuzzy_hui_fei", 0);
        config[0x18] = wire::number(request.get(), "fuzzy_en_eng", 0);
        config[0x19] = wire::number(request.get(), "fuzzy_in_ing", 0);
        config[0x1a] = wire::number(request.get(), "fuzzy_on_ong", 0);
        config[0x1b] = wire::number(request.get(), "fuzzy_huang_wang", 0);
        config[0x1c] = wire::number(request.get(), "fuzzy_un_ong", 0);
        config[0x1d] = wire::number(request.get(), "fuzzy_un_iong", 0);
        config[0x1e] = wire::number(request.get(), "fuzzy_an_ai", 0);
        config[0x1f] = wire::number(request.get(), "fuzzy_eng_ong", 0);
        if (wire::number(request.get(), "emoji_recommend", 1)) {
          config[0x33] = wire::number(request.get(), "wechat_emoji", 1);
          config[0x34] = wire::number(request.get(), "normal_emoji", 1);
          config[0x35] = wire::number(request.get(), "kaomoji", 1);
          config[0x36] = wire::number(request.get(), "large_emoji", 1);
          config[0x39] = wire::number(request.get(), "symbol_emoji", 1);
        }
        // SessionCreator gates construction of VModeV2 on config +0x125.
        // The UI later activates that wrapper with session bool option 0x11.
        config[0x125] = wire::number(request.get(), "v_mode", 1);
        *(uint32_t *)(config + 0xb8) =
            wire::number(request.get(), "traditional", 0) ? 1 : 0;
        state->keyboard = keyboard;
        state->engine =
            ((uint64_t (*)(void *))syms.at("_wxime_create_session"))(config);
        if (!state->engine)
          _exit(91);
        ((void (*)(uint64_t, decltype(&service_listener), void *))syms.at(
            "_wxime_add_session_listener"))(state->engine, service_listener,
                                            state.get());
        sessions[client] = std::move(state);
      }
      auto &s = *sessions.at(client);
      if (op == "select" && wire::number(request.get(), "revision", -1) < 0 &&
          wire::number(request.get(), "index") == 0) {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.candidates.empty())
          op = "raw";
      }
      if (op != "poll" && op != "open" && op != "select") {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.published.clear();
      }
      std::string cursorCommit;
      int64_t cursorCommitPosition = -1;
      if (op == "predict") {
        auto before = wire::str(request.get(), "before");
        if (before.size() > 4096 || s.pending)
          wire::put(response.get(), "error",
                    std::string("invalid prediction context"));
        else {
          using Surround =
              void (*)(uint64_t, const char *, uint32_t, const char *, uint32_t,
                       const char *, uint32_t, int);
          ((Surround)syms.at("_wxime_set_text_around_cursor"))(
              s.engine, before.data(), before.size(), "", 0, "", 0, 0);
          ((void (*)(uint64_t))syms.at("_wxime_get_most_likely_sequel"))(
              s.engine);
        }
      } else if (op == "vmode") {
        const bool enabled = wire::number(request.get(), "enabled", 1);
        s.vMode = enabled;
        ((void (*)(uint64_t, uint32_t, bool))syms.at(
            "_wxime_session_set_bool_option"))(s.engine, 0x11, enabled);
        if (!enabled)
          ((void (*)(uint64_t))syms.at("_wxime_reset_session"))(s.engine);
        s.pending = 0;
        s.preedit.clear();
        s.selected.clear();
        s.cursor = 0;
        s.pendingRaw.clear();
        s.displayPreedit.clear();
        s.displayCursor = 0;
        s.cursorStops.clear();
        s.candidates.clear();
      } else if (op == "key") {
        auto key = wire::str(request.get(), "key");
        if (key.size() != 1 || (unsigned char)key[0] < 32 ||
            (unsigned char)key[0] > 126)
          _exit(90);
        if (s.preedit.size() >= 128 || s.selected.size() >= 384)
          wire::put(response.get(), "error", std::string("composition limit"));
        else {
          s.cursor = std::min(s.cursor, s.preedit.size());
          s.preedit.insert(s.cursor, key);
          s.cursor += key.size();
          ((void (*)(uint64_t, const char *, uint32_t, const void *,
                     uint32_t))syms.at("_wxime_process_input"))(
              s.engine, key.data(), 1, nullptr, 0);
        }
      } else if (op == "punctuation") {
        static const std::map<std::string, std::string> map = {
            {",", "，"}, {".", "。"}, {";", "；"}, {":", "："},
            {"?", "？"}, {"!", "！"}, {"'", "'"},  {"/", "、"}};
        auto key = wire::str(request.get(), "key");
        auto it = map.find(key);
        if (key.size() != 1 || (unsigned char)key[0] < 0x20 ||
            (unsigned char)key[0] > 0x7e)
          _exit(90);
        auto mapped = wire::str(request.get(), "mapped");
        if (mapped.size() > 32 || mapped.find('\0') != std::string::npos)
          _exit(90);
        auto pairCursor = wire::number(request.get(), "pair_cursor", -1);
        if (pairCursor < -1 || pairCursor > 32)
          _exit(90);
        if (!s.preedit.empty() || !s.selected.empty())
          service_select(s, 0);
        if (!s.preedit.empty() || !s.selected.empty()) {
          auto raw = s.selected + s.preedit;
          ((void (*)(uint64_t))syms.at("_wxime_reset_session"))(s.engine);
          std::lock_guard<std::mutex> lock(s.mutex);
          s.commits.push_back(raw);
          s.preedit.clear();
          s.selected.clear();
          s.cursor = 0;
          s.pendingRaw.clear();
          s.displayPreedit.clear();
          s.displayCursor = 0;
          s.cursorStops.clear();
          s.pending = 0;
          s.candidates.clear();
        }
        if (pairCursor >= 0 && !mapped.empty()) {
          cursorCommit = mapped;
          cursorCommitPosition = pairCursor;
        } else {
          std::lock_guard<std::mutex> lock(s.mutex);
          s.commits.push_back(
              !mapped.empty()
                  ? mapped
                  : (it != map.end() && wire::number(request.get(),
                                                     "chinese_punctuation", 1)
                         ? it->second
                         : key));
        }
      } else if (op == "backspace") {
        if (s.cursor && !s.preedit.empty()) {
          s.cursor = std::min(s.cursor, s.preedit.size());
          s.preedit.erase(--s.cursor, 1);
          ((void (*)(uint64_t, uint8_t, const void *, uint32_t))syms.at(
              "_wxime_drop_last"))(s.engine, 1, nullptr, 0);
        }
      } else if (op == "move") {
        auto position = wire::number(request.get(), "cursor", -1);
        if (position < int64_t(s.selected.size()) ||
            size_t(position) > s.selected.size() + s.preedit.size())
          wire::put(response.get(), "error", std::string("invalid cursor"));
        else {
          s.cursor = size_t(position) - s.selected.size();
          ((void (*)(uint64_t, uint32_t))syms.at(
              "_wxime_reset_internal_cursor"))(
              s.engine, uint32_t(s.preedit.size() - s.cursor));
        }
      } else if (op == "delete") {
        s.cursor = std::min(s.cursor, s.preedit.size());
        if (s.cursor < s.preedit.size()) {
          ((void (*)(uint64_t, uint32_t))syms.at(
              "_wxime_reset_internal_cursor"))(
              s.engine, uint32_t(s.preedit.size() - s.cursor - 1));
          ((void (*)(uint64_t, uint8_t, const void *, uint32_t))syms.at(
              "_wxime_drop_last"))(s.engine, 1, nullptr, 0);
          s.preedit.erase(s.cursor, 1);
          ((void (*)(uint64_t, uint32_t))syms.at(
              "_wxime_reset_internal_cursor"))(
              s.engine, uint32_t(s.preedit.size() - s.cursor));
        }
      } else if (op == "select") {
        auto expected = wire::number(request.get(), "revision", -1);
        if (!service_select(s, wire::number(request.get(), "index"), expected))
          wire::put(response.get(), "error", std::string("stale candidate"));
      } else if (op == "reset" || op == "raw") {
        auto raw = s.selected + s.preedit;
        ((void (*)(uint64_t))syms.at("_wxime_reset_session"))(s.engine);
        std::lock_guard<std::mutex> lock(s.mutex);
        s.pending = 0;
        s.vMode = false;
        s.preedit.clear();
        s.selected.clear();
        s.cursor = 0;
        s.pendingRaw.clear();
        s.displayPreedit.clear();
        s.displayCursor = 0;
        s.cursorStops.clear();
        s.candidates.clear();
        if (op == "raw" && !raw.empty())
          s.commits.push_back(raw);
      } else if (op != "open" && op != "poll")
        wire::put(response.get(), "error", std::string("unknown operation"));
      {
        std::lock_guard<std::mutex> lock(s.mutex);
        // PendingInput entries can combine letters (e.g. "zh"). Their
        // count is not the number of typed ASCII bytes.
        if (!s.pending && (!s.keyboard || !s.commits.empty())) {
          s.preedit.clear();
          s.selected.clear();
          s.cursor = 0;
          s.pendingRaw.clear();
          s.displayPreedit.clear();
          s.displayCursor = 0;
          s.cursorStops.clear();
        }
        if (!s.vMode && s.preedit.empty() && s.selected.empty() &&
            op != "predict" && !s.candidates.empty()) {
          s.candidates.clear();
          s.published.clear();
          ++s.revision;
        }
        publish_service_candidates(s);
        wire::put(response.get(), "revision", int64_t(s.revision));
        wire::put(response.get(), "preedit", s.selected + s.preedit);
        wire::put(response.get(), "cursor",
                  int64_t(s.selected.size() + s.cursor));
        wire::put(response.get(), "editable_begin", int64_t(s.selected.size()));
        if (s.pendingRaw == s.selected + s.preedit) {
          wire::put(response.get(), "display_preedit", s.displayPreedit);
          wire::put(response.get(), "display_cursor", int64_t(s.displayCursor));
          auto *stops = json_object_new_array();
          for (auto stop : s.cursorStops)
            json_object_array_add(stops, json_object_new_int64(stop));
          json_object_object_add(response.get(), "cursor_stops", stops);
        }
        auto *array = json_object_new_array();
        for (auto &c : s.candidates)
          json_object_array_add(
              array, json_object_new_string_len(c.text.data(), c.text.size()));
        json_object_object_add(response.get(), "candidates", array);
        array = json_object_new_array();
        for (auto &c : s.commits)
          json_object_array_add(array,
                                json_object_new_string_len(c.data(), c.size()));
        json_object_object_add(response.get(), "commits", array);
        if (!cursorCommit.empty()) {
          wire::put(response.get(), "cursor_commit", cursorCommit);
          wire::put(response.get(), "cursor_commit_position",
                    cursorCommitPosition);
        }
        s.commits.clear();
      }
    }
    transport.reply(source, wire::dump(response.get()));
  }
  for (auto &[client, s] : sessions)
    ((void (*)(uint64_t))syms.at("_wxime_destroy_session"))(s->engine);
  ((void (*)())syms.at("_wxime_finalize"))();
}
