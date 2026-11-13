// 测试真实 IPC 循环传入原版 API 的参数，不加载专有运行时或用户数据。
#define main wetypex_engine_host_main
#include "../src/backend/macos_host.cpp"
#undef main

struct CapturedGroupInfo {
  bool unknown, debug;
  unsigned char padding[6];
  uint64_t group, functions, phrases, dictionary;
};
static_assert(sizeof(CapturedGroupInfo) == 40);

static std::vector<CapturedGroupInfo> created, notified;
static unsigned warmed = 0, reset = 0, destroyed = 0;

static uint64_t capture_session(void *config) {
  CapturedGroupInfo info{};
  memcpy(&info, static_cast<unsigned char *>(config) + 0xf8, sizeof(info));
  created.push_back(info);
  return created.size();
}
static void capture_notification(CapturedGroupInfo info) {
  notified.push_back(info);
}
static void add_listener(uint64_t, decltype(&service_listener), void *) {}
static int warm_session(uint64_t) { ++warmed; return 0; }
static void reset_session(uint64_t) { ++reset; }
static void destroy_session(uint64_t) { ++destroyed; }
static void finalize() {}

static void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
static void expect(const CapturedGroupInfo &actual,
                   const CapturedGroupInfo &expected) {
  require(actual.unknown == expected.unknown, "unknown state changed");
  require(actual.debug == expected.debug, "debug state changed");
  require(actual.group == expected.group, "device group changed");
  require(actual.functions == expected.functions, "sync switches changed");
  require(actual.phrases == expected.phrases, "phrase version changed");
  require(actual.dictionary == expected.dictionary, "dictionary version changed");
}

int main(int argc, char **argv) {
  std::filesystem::path temporary;
  try {
    require(argc == 3, "expected scenario and warm-up mode");
    const std::string scenario = argv[1];
    const bool warmup = std::string(argv[2]) == "warmup";
    require(scenario == "unknown" || scenario == "configured" ||
                scenario == "refreshed", "unknown scenario");
    std::string pattern =
        (std::filesystem::temp_directory_path() / "wetypex-session-sync-XXXXXX").string();
    const auto directory = mkdtemp(pattern.data());
    require(directory, "cannot create private fixture directory");
    temporary = directory;
    workspace_root = temporary.string();
    setenv("WETYPE_SUPPORT_DIR", directory, 1);
    setenv("WETYPE_NETWORK_LIVE", "1", 1);
    if (warmup)
      setenv("WETYPE_QEMU_WRAPPED", "1", 1);
    else
      unsetenv("WETYPE_QEMU_WRAPPED");
    service_mode = true;
    syms["_wxime_create_session"] = reinterpret_cast<uintptr_t>(&capture_session);
    syms["_wxime_group_sync_info_changed"] =
        reinterpret_cast<uintptr_t>(&capture_notification);
    syms["_wxime_add_session_listener"] = reinterpret_cast<uintptr_t>(&add_listener);
    syms["_wxime_warm_up_session"] = reinterpret_cast<uintptr_t>(&warm_session);
    syms["_wxime_reset_session"] = reinterpret_cast<uintptr_t>(&reset_session);
    syms["_wxime_destroy_session"] = reinterpret_cast<uintptr_t>(&destroy_session);
    syms["_wxime_finalize"] = reinterpret_cast<uintptr_t>(&finalize);

    CapturedGroupInfo initial{true, false, {}, 0, 0, 0, 0};
    if (scenario != "unknown") {
      initial = {false, false, {}, 42, 15, 17, 23};
      configure_group_sync(42, 15, 17, 23);
    }
    std::string requests =
        "{\"op\":\"open\",\"session\":1,\"seq\":1,\"epoch\":1}\n"
        "{\"op\":\"open\",\"session\":1,\"seq\":2,\"epoch\":1}\n"
        "{\"op\":\"close\",\"session\":1,\"seq\":3,\"epoch\":1}\n";
    if (scenario == "refreshed")
      requests += "{\"op\":\"group_sync\",\"session\":1,\"seq\":4,"
                  "\"epoch\":1,\"group_id\":84,\"func_switch\":0}\n";
    requests +=
        "{\"op\":\"open\",\"session\":1,\"seq\":5,\"epoch\":1}\n"
        "{\"op\":\"quit\",\"session\":1,\"seq\":6,\"epoch\":1}\n";
    int input[2];
    require(pipe(input) == 0, "cannot create protocol input pipe");
    require(write(input[1], requests.data(), requests.size()) ==
                static_cast<ssize_t>(requests.size()), "cannot stage requests");
    close(input[1]);
    require(dup2(input[0], STDIN_FILENO) >= 0, "cannot redirect protocol input");
    close(input[0]);
    service_loop();

    // 同一会话重复 open 不会重建；close 后则必须使用最新同步信息。
    require(created.size() == 2 + unsigned(warmup), "unexpected session count");
    for (size_t i = 0; i + 1 < created.size(); ++i)
      expect(created[i], initial);
    const CapturedGroupInfo refreshed{false, false, {}, 84, 0, 0, 0};
    expect(created.back(), scenario == "refreshed" ? refreshed : initial);
    require(notified.size() == (scenario == "unknown" ? 0u :
                               scenario == "configured" ? 1u : 2u),
            "unexpected notification count");
    if (!notified.empty())
      expect(notified.front(), initial);
    if (scenario == "refreshed")
      expect(notified.back(), refreshed);
    require(warmed == unsigned(warmup) && reset == unsigned(warmup),
            "warm-up lifecycle changed");
    require(destroyed == created.size(), "session was not destroyed");
    std::filesystem::remove_all(temporary);
    return 0;
  } catch (const std::exception &error) {
    fprintf(stderr, "session group sync: %s\n", error.what());
    if (!temporary.empty())
      std::filesystem::remove_all(temporary);
    return 1;
  }
}
