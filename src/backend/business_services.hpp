struct CapiNetworkLogin {
  const char *device_name;
  uint32_t device_name_len, pad0;
  const char *channel_id;
  uint32_t channel_id_len, pad1;
  const char *device_code;
  uint32_t device_code_len;
  bool debug;
  unsigned char pad2[3];
  uint32_t log_level;
  uint32_t reserved;
};
static_assert(sizeof(CapiNetworkLogin) == 56);

// wxime_network_login takes this object by value. The Android JNI bridge
// reserves the first 16 bytes before these fields, making the full ABI object
// 72 bytes. appVer is supplied by the earlier wxime_initialize config.
struct CapiNetworkLoginArgument {
  unsigned char reserved[16];
  CapiNetworkLogin info;
};
static_assert(sizeof(CapiNetworkLoginArgument) == 72);

static void network_login_service(bool exit_after = true,
                                  bool emit_result = true,
                                  bool wait_for_transport = true) {
  const char *uin = getenv("WETYPE_CORE_UIN");
  if (uin && *uin) {
    const size_t uinLength = strlen(uin);
    const bool numeric =
        uinLength <= 31 &&
        std::all_of(uin, uin + uinLength, [](unsigned char value) {
          return value >= '0' && value <= '9';
        });
    if (numeric)
      ((bool (*)(const char *, uint32_t))syms.at("_wxime_reset_user_id"))(
          uin, uint32_t(uinLength));
  }
  const char *seed = getenv("WETYPE_DEVICE_MODEL");
  if (!seed || !*seed)
    seed = "LINUX";
  char generated_code[1024]{};
  const char *code = getenv("WETYPE_CORE_DEVICE_CODE");
  size_t length = code && *code ? strlen(code) : 0;
  if (!length) {
    ((void (*)(uint32_t, const char *, uint32_t, char *, size_t *))syms.at(
        "_net_generate_device_code_v2"))(5, seed, strlen(seed), generated_code,
                                         &length);
    code = generated_code;
  }
  CapiNetworkLoginArgument login{};
  login.info.device_name = seed;
  login.info.device_name_len = strlen(seed);
  login.info.channel_id = "";
  login.info.channel_id_len = 0;
  login.info.device_code = code;
  login.info.device_code_len = length;
  login.info.log_level = 2;
  ((void (*)(CapiNetworkLoginArgument))syms.at("_wxime_network_login"))(login);
  auto stateFn = ((int (*)())dlsym(RTLD_DEFAULT, "wcwss_bridge_state"));
  auto errorFn = ((int (*)())dlsym(RTLD_DEFAULT, "wcwss_bridge_error"));
  unsigned waits =
      wait_for_transport ? (getenv("WETYPE_NETWORK_LIVE") ? 120 : 3) : 0;
  while (waits-- && !stateFn())
    usleep(100000);
  if (wait_for_transport && getenv("WETYPE_NETWORK_LIVE") && stateFn() == 1)
    usleep(2000000);
  auto calls = ((unsigned (*)())dlsym(RTLD_DEFAULT, "wcwss_bridge_calls"))();
  auto sends = ((unsigned (*)())dlsym(RTLD_DEFAULT, "wcwss_bridge_sends"))();
  auto receives =
      ((unsigned (*)())dlsym(RTLD_DEFAULT, "wcwss_bridge_receives"))();
  if (emit_result) {
    fprintf(stdout,
            "{\"network_login_returned\":true,\"device_code_bytes\":%zu,"
            "\"bridge_calls\":%u,\"transport_state\":%d,"
            "\"transport_error\":%d,\"sends\":%u,\"receives\":%u,"
            "\"server_contacted\":%s}\n",
            length, calls, stateFn(), errorFn(), sends, receives,
            getenv("WETYPE_NETWORK_LIVE") ? "true" : "false");
    fflush(stdout);
  }
  if (!exit_after)
    return;
  ((void (*)())syms.at("_wxime_network_logout"))();
  _exit(length ? 0 : 1);
}

struct BusinessNetworkConfig {
  uint32_t platform, pad0;
  const char *app_version;
  const char *device_name;
  const char *channel_id;
  const char *device_code;
  const char *system_version;
  uint8_t package_type;
  unsigned char pad1[7];
};
static_assert(sizeof(BusinessNetworkConfig) == 56);

struct BusinessConfig {
  uint32_t log_level, pad0;
  void (*log)(int, const char *);
  const char *work_path;
  bool use_debug_net;
  unsigned char pad1[7];
  const char *uin;
  uint32_t scene, pad2;
  BusinessNetworkConfig *network;
  bool need_get_uin;
  unsigned char pad3[7];
  const char *unique_id;
};
static_assert(sizeof(BusinessConfig) == 72);

static std::atomic<int> business_init_callback_count{0};
static std::atomic<int> business_init_error{-1};
static std::atomic<bool> business_init_has_uin{false};
static std::atomic<unsigned long long> business_init_uin{0};
static std::atomic<int> business_task_callback_count{0};
static std::atomic<int> business_task_error{-1};
static std::mutex business_task_mutex;
static std::vector<unsigned char> business_task_response;

static void business_log(int level, const char *message) {
  if (getenv("WETYPE_VOICE_STREAM") && level < 3)
    return;
  fprintf(stderr, "BUSINESS[%d] %s\n", level, message ? message : "");
}
static void business_init_callback(void *, int error, bool has_uin,
                                   unsigned long long uin) {
  business_init_error = error;
  if (has_uin) {
    business_init_has_uin = true;
    business_init_uin = uin;
  }
  ++business_init_callback_count;
}
static void business_status_callback(void *, int) {}
static void business_push_callback(int, const char *, uint32_t) {}
static void business_network_callback(bool) {}
static void business_user_callback(void *) {}
static void business_flush_callback() {}
static void business_task_callback(void *, int, int error, const char *data,
                                   uint32_t size, const char *, uint32_t,
                                   const char *, uint32_t) {
  {
    std::lock_guard lock(business_task_mutex);
    business_task_response.assign(data, data ? data + size : data);
  }
  business_task_error = error;
  business_task_callback_count.fetch_add(1, std::memory_order_release);
}

static void protobuf_varint(std::vector<unsigned char> &output,
                            uint64_t value) {
  while (value >= 0x80) {
    output.push_back(static_cast<unsigned char>(value) | 0x80);
    value >>= 7;
  }
  output.push_back(static_cast<unsigned char>(value));
}
static void protobuf_string(std::vector<unsigned char> &output, unsigned field,
                            const std::string &value) {
  protobuf_varint(output, (field << 3) | 2);
  protobuf_varint(output, value.size());
  output.insert(output.end(), value.begin(), value.end());
}
static void protobuf_uint(std::vector<unsigned char> &output, unsigned field,
                          uint64_t value) {
  protobuf_varint(output, field << 3);
  protobuf_varint(output, value);
}
static std::vector<unsigned char> p2p_link_info_from_file(const char *path,
                                                          const char *model) {
  // WTConnectionPlan parses link_net_info as JSON. In a P2P-only session the
  // official iOS client sends exactly device_name and preferred_client. The
  // QR-code host must prefer the server role so the scanning phone can be the
  // preferred client.
  bool preferredClient = false;
  std::string ip;
  if (path && *path) {
    std::ifstream stream(path);
    std::string json((std::istreambuf_iterator<char>(stream)), {});
    auto info = wire::parse(json);
    if (auto *value = wire::get(info.get(), "preferred_client");
        value && json_object_is_type(value, json_type_boolean))
      preferredClient = json_object_get_boolean(value);
    ip = wire::str(info.get(), "ip");
  }
  auto link = wire::object();
  auto *lan = json_object_new_array();
  if (!ip.empty() && ip != "0.0.0.0" && ip != "127.0.0.1")
    json_object_array_add(lan, json_object_new_string(ip.c_str()));
  json_object_object_add(link.get(), "lan", lan);
  wire::put(link.get(), "device_name", model ? model : "LINUX");
  wire::boolean(link.get(), "preferred_client", preferredClient);
  auto serialized = wire::dump(link.get());
  return {serialized.begin(), serialized.end()};
}
static std::vector<unsigned char> p2p_grpc_info_from_file(const char *path,
                                                          const char *model) {
  if (!path || !*path)
    return {};
  std::ifstream stream(path);
  std::string json((std::istreambuf_iterator<char>(stream)), {});
  auto info = wire::parse(json);
  const auto ip = wire::str(info.get(), "ip");
  const auto serverName = wire::str(info.get(), "server_name");
  const auto port = wire::number(info.get(), "port");
  if (ip.empty() || serverName.empty() || port <= 0 || port >= 65536)
    return {};
  auto grpc = wire::object();
  wire::put(grpc.get(), "platform", int64_t(5));
  wire::put(grpc.get(), "server_name", serverName);
  wire::put(grpc.get(), "device_name", model ? model : "LINUX");
  auto available = wire::object();
  auto *lan = json_object_new_array();
  auto endpoint = wire::object();
  wire::put(endpoint.get(), "ip", ip);
  wire::put(endpoint.get(), "port", port);
  json_object_array_add(lan, endpoint.release());
  json_object_object_add(available.get(), "lan", lan);
  json_object_object_add(grpc.get(), "available_nets", available.release());
  auto serialized = wire::dump(grpc.get());
  return {serialized.begin(), serialized.end()};
}
static std::string p2p_public_cer_from_file(const char *path) {
  if (!path || !*path)
    return {};
  std::ifstream stream(path);
  std::string json((std::istreambuf_iterator<char>(stream)), {});
  auto info = wire::parse(json);
  return wire::str(info.get(), "ca_cert");
}
static std::string base64_encode(std::string_view input) {
  static constexpr char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string output;
  output.reserve((input.size() + 2) / 3 * 4);
  for (size_t i = 0; i < input.size(); i += 3) {
    uint32_t value = static_cast<unsigned char>(input[i]) << 16;
    if (i + 1 < input.size())
      value |= static_cast<unsigned char>(input[i + 1]) << 8;
    if (i + 2 < input.size())
      value |= static_cast<unsigned char>(input[i + 2]);
    output.push_back(alphabet[(value >> 18) & 63]);
    output.push_back(alphabet[(value >> 12) & 63]);
    output.push_back(i + 1 < input.size() ? alphabet[(value >> 6) & 63] : '=');
    output.push_back(i + 2 < input.size() ? alphabet[value & 63] : '=');
  }
  return output;
}
static bool protobuf_read_varint(const std::vector<unsigned char> &input,
                                 size_t &offset, uint64_t &value) {
  value = 0;
  for (unsigned shift = 0; shift < 64 && offset < input.size(); shift += 7) {
    unsigned char byte = input[offset++];
    value |= static_cast<uint64_t>(byte & 0x7f) << shift;
    if (!(byte & 0x80))
      return true;
  }
  return false;
}
static std::string
protobuf_string_field(const std::vector<unsigned char> &input,
                      unsigned wanted) {
  size_t offset = 0;
  while (offset < input.size()) {
    uint64_t tag = 0, value = 0;
    if (!protobuf_read_varint(input, offset, tag))
      break;
    unsigned wire = tag & 7;
    if (wire == 0) {
      if (!protobuf_read_varint(input, offset, value))
        break;
    } else if (wire == 2) {
      if (!protobuf_read_varint(input, offset, value) ||
          value > input.size() - offset)
        break;
      if ((tag >> 3) == wanted)
        return std::string(
            reinterpret_cast<const char *>(input.data() + offset), value);
      offset += value;
    } else {
      break;
    }
  }
  return {};
}
static uint64_t protobuf_uint_field(const std::vector<unsigned char> &input,
                                    unsigned wanted, uint64_t fallback) {
  size_t offset = 0;
  while (offset < input.size()) {
    uint64_t tag = 0, value = 0;
    if (!protobuf_read_varint(input, offset, tag))
      break;
    unsigned wire = tag & 7;
    if (wire == 0) {
      if (!protobuf_read_varint(input, offset, value))
        break;
      if ((tag >> 3) == wanted)
        return value;
    } else if (wire == 2) {
      if (!protobuf_read_varint(input, offset, value) ||
          value > input.size() - offset)
        break;
      offset += value;
    } else {
      break;
    }
  }
  return fallback;
}
static std::vector<std::vector<unsigned char>>
protobuf_messages_field(const std::vector<unsigned char> &input,
                        unsigned wanted) {
  std::vector<std::vector<unsigned char>> result;
  size_t offset = 0;
  while (offset < input.size()) {
    uint64_t tag = 0, value = 0;
    if (!protobuf_read_varint(input, offset, tag))
      break;
    unsigned wire = tag & 7;
    if (wire == 0) {
      if (!protobuf_read_varint(input, offset, value))
        break;
    } else if (wire == 2) {
      if (!protobuf_read_varint(input, offset, value) ||
          value > input.size() - offset)
        break;
      if ((tag >> 3) == wanted)
        result.emplace_back(input.begin() + offset,
                            input.begin() + offset + value);
      offset += value;
    } else {
      break;
    }
  }
  return result;
}

struct BusinessIdentity {
  std::string model;
  std::string unique_id;
  std::string device_code;
  unsigned long long uin = 0;
};
static BusinessIdentity load_business_identity(const char *path) {
  BusinessIdentity identity;
  if (!path || !*path)
    return identity;
  std::ifstream file(path);
  std::stringstream buffer;
  if (!file)
    return identity;
  buffer << file.rdbuf();
  auto json = wire::parse(buffer.str());
  if (!json)
    return identity;
  identity.model = wire::str(json.get(), "model");
  identity.unique_id = wire::str(json.get(), "unique_id");
  identity.device_code = wire::str(json.get(), "device_code");
  identity.uin = wire::number(json.get(), "uin");
  return identity;
}
static bool save_business_identity(const char *path,
                                   const BusinessIdentity &identity) {
  if (!path || !*path || !identity.uin)
    return false;
  auto json = wire::object();
  wire::put(json.get(), "version", int64_t(1));
  wire::put(json.get(), "model", identity.model);
  wire::put(json.get(), "unique_id", identity.unique_id);
  wire::put(json.get(), "device_code", identity.device_code);
  wire::put(json.get(), "uin", static_cast<int64_t>(identity.uin));
  std::string data = wire::dump(json.get()) + "\n";
  std::string temporary = std::string(path) + ".tmp";
  int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0)
    return false;
  size_t offset = 0;
  while (offset < data.size()) {
    ssize_t written = write(fd, data.data() + offset, data.size() - offset);
    if (written < 0 && errno == EINTR)
      continue;
    if (written <= 0) {
      close(fd);
      unlink(temporary.c_str());
      return false;
    }
    offset += written;
  }
  bool synced = fsync(fd) == 0;
  bool closed = close(fd) == 0;
  bool ok = synced && closed && rename(temporary.c_str(), path) == 0;
  if (ok)
    chmod(path, 0600);
  else
    unlink(temporary.c_str());
  return ok;
}

#include "native_voice.hpp"
static void business_service() {
  const char *identity_path = getenv("WETYPE_IDENTITY_FILE");
  BusinessIdentity identity = load_business_identity(identity_path);
  if (identity.model.empty()) {
    const char *model = getenv("WETYPE_DEVICE_MODEL");
    identity.model = model && *model ? model : "LINUX";
  }
  unsigned char uuid_bytes[16]{};
  if (identity.unique_id.empty()) {
    if (shim_sec_random_copy_bytes(nullptr, sizeof(uuid_bytes), uuid_bytes))
      _exit(93);
    uuid_bytes[6] = (uuid_bytes[6] & 0x0f) | 0x40;
    uuid_bytes[8] = (uuid_bytes[8] & 0x3f) | 0x80;
    char unique_id[37]{};
    snprintf(unique_id, sizeof(unique_id),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
             "%02x%02x%02x%02x%02x%02x",
             uuid_bytes[0], uuid_bytes[1], uuid_bytes[2], uuid_bytes[3],
             uuid_bytes[4], uuid_bytes[5], uuid_bytes[6], uuid_bytes[7],
             uuid_bytes[8], uuid_bytes[9], uuid_bytes[10], uuid_bytes[11],
             uuid_bytes[12], uuid_bytes[13], uuid_bytes[14], uuid_bytes[15]);
    identity.unique_id = unique_id;
  }
  if (identity.device_code.empty()) {
    char code[1024]{};
    size_t length = 0;
    ((void (*)(uint32_t, const char *, uint32_t, char *, size_t *))syms.at(
        "_business_generate_device_code_v2"))(
        5, identity.model.c_str(), identity.model.size(), code, &length);
    if (!length || length > sizeof(code))
      _exit(93);
    identity.device_code.assign(code, length);
  }
  unsigned long long starting_uin = identity.uin;
  business_init_callback_count = 0;
  business_init_error = -1;
  business_init_has_uin = starting_uin != 0;
  business_init_uin = starting_uin;
  std::string existing_uin =
      starting_uin ? std::to_string(starting_uin) : std::string();
  const char *business_work = getenv("WETYPE_WORK_DIR");
  if (!business_work || !*business_work)
    business_work = "/work/business";
  mkdir(business_work, 0700);
  BusinessNetworkConfig network{5,
                                0,
                                "2.2.3(657)",
                                identity.model.c_str(),
                                "",
                                identity.device_code.c_str(),
                                "15.2.0",
                                3,
                                {}};
  BusinessConfig config{1,
                        0,
                        business_log,
                        business_work,
                        false,
                        {},
                        existing_uin.c_str(),
                        2,
                        0,
                        &network,
                        !starting_uin,
                        {},
                        identity.unique_id.c_str()};
  using Initialize = void (*)(
      BusinessConfig *, void *, void (*)(void *, int, bool, unsigned long long),
      void (*)(void *, int), void (*)(int, const char *, uint32_t),
      void (*)(bool), void (*)(void *), void (*)());
  ((Initialize)syms.at("_business_initialize"))(
      &config, nullptr, business_init_callback, business_status_callback,
      business_push_callback, business_network_callback, business_user_callback,
      business_flush_callback);
  unsigned waits = getenv("WETYPE_NETWORK_LIVE") ? 120 : 3;
  while (waits-- && !business_init_callback_count)
    usleep(100000);
  if (business_init_error == 0 && business_init_uin) {
    identity.uin = business_init_uin.load();
    save_business_identity(identity_path, identity);
  }
  if (getenv("WETYPE_VOICE_STREAM")) {
    if (business_init_error != 0 || !business_init_uin)
      _exit(78);
    if (!starting_uin) {
      const auto uin = std::to_string(business_init_uin.load());
      ((void (*)(const char *))syms.at("_business_reset_uin"))(uin.c_str());
      // Apply the newly registered identity before its first voice request,
      // as in the existing one-shot business path. This runs while capture
      // is already in progress and is unnecessary on subsequent sessions.
      usleep(750000);
    }
    native_voice::run();
    _exit(0);
  }
  int pairing_task = -1;
  std::string collected_voice_text;
  uint32_t operation_subcommand = 0;
  if (getenv("WETYPE_PAIRING_CODE"))
    operation_subcommand = 30;
  else if (getenv("WETYPE_PAIRING_STATUS_CODE"))
    operation_subcommand = 40;
  else if (getenv("WETYPE_CONFIRM_BIND_ID"))
    operation_subcommand = 32;
  else if (getenv("WETYPE_GROUP_INFO"))
    operation_subcommand = 34;
  else if (getenv("WETYPE_UNBIND_UIN"))
    operation_subcommand = 35;
  else if (getenv("WETYPE_SEND_CLIPBOARD"))
    operation_subcommand = 36;
  else if (getenv("WETYPE_SEND_CLIPBOARD_IMAGE"))
    operation_subcommand = 36;
  else if (getenv("WETYPE_SET_FUNCTIONS"))
    operation_subcommand = 39;
  else if (getenv("WETYPE_VOICE_FILE"))
    operation_subcommand = 1000;
  else if (getenv("WETYPE_P2P_CODE"))
    operation_subcommand = 1001;
  else if (getenv("WETYPE_P2P_PEER"))
    operation_subcommand = 1002;
  else if (getenv("WETYPE_P2P_INIT_UIN"))
    operation_subcommand = 1003;
  else if (getenv("WETYPE_P2P_UPDATE"))
    operation_subcommand = 1004;
  if (operation_subcommand && business_init_error == 0 &&
      business_init_has_uin) {
    char uin_text[32]{};
    snprintf(uin_text, sizeof(uin_text), "%llu", business_init_uin.load());
    if (!starting_uin) {
      ((void (*)(const char *))syms.at("_business_reset_uin"))(uin_text);
      usleep(750000);
    }
    business_task_callback_count = 0;
    business_task_error = -1;
    {
      std::lock_guard lock(business_task_mutex);
      business_task_response.clear();
    }
    std::vector<unsigned char> request;
    std::vector<std::vector<unsigned char>> task_requests;
    uint32_t command_id = 4814;
    uint32_t wire_subcommand = operation_subcommand;
    if (operation_subcommand != 1000 && operation_subcommand != 1001 &&
        operation_subcommand != 1002 && operation_subcommand != 1003 &&
        operation_subcommand != 1004) {
      protobuf_uint(request, 1, business_init_uin.load());
      protobuf_uint(request, 2, 5);
    }
    if (operation_subcommand == 30) {
      std::vector<unsigned char> device;
      protobuf_string(device, 1, identity.model);
      protobuf_uint(device, 2, 5);
      protobuf_string(device, 4, "2.2.3(657)");
      protobuf_uint(device, 5, 1);
      protobuf_string(request, 3,
                      std::string(reinterpret_cast<const char *>(device.data()),
                                  device.size()));
    } else if (operation_subcommand == 40) {
      protobuf_string(request, 3, getenv("WETYPE_PAIRING_STATUS_CODE"));
    } else if (operation_subcommand == 32) {
      protobuf_string(request, 3, getenv("WETYPE_CONFIRM_BIND_ID"));
    } else if (operation_subcommand == 34) {
      // Read pending invitations without consuming any transfer code.
      protobuf_uint(request, 3, getenv("WETYPE_PENDING_FILE_INVITE") ? 65 : 31);
      // GetGroupInfoReq.current_clipboard_version.  Both native desktop
      // clients send their last accepted version as the synchronization
      // cursor. The client still compares the response because the service
      // may include the current payload again.
      protobuf_uint(request, 4,
                    strtoull(getenv("WETYPE_GROUP_INFO"), nullptr, 10));
    } else if (operation_subcommand == 35) {
      protobuf_uint(request, 3,
                    strtoull(getenv("WETYPE_UNBIND_UIN"), nullptr, 10));
    } else if (operation_subcommand == 39) {
      protobuf_uint(request, 3,
                    strtoull(getenv("WETYPE_GROUP_ID"), nullptr, 10));
      protobuf_uint(request, 4,
                    strtoull(getenv("WETYPE_SET_FUNCTIONS"), nullptr, 10));
    } else if (operation_subcommand == 36) {
      std::vector<unsigned char> clipboard;
      if (const char *imagePath = getenv("WETYPE_SEND_CLIPBOARD_IMAGE")) {
        std::ifstream image(imagePath, std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(image)), {});
        if (bytes.empty() || bytes.size() > 8388608)
          _exit(93);
        protobuf_uint(clipboard, 2, 5); // TypeImageOri
        protobuf_string(clipboard, 6, bytes);
      } else {
        std::string text = getenv("WETYPE_SEND_CLIPBOARD");
        protobuf_uint(clipboard, 2, 1); // TypeTextOri
        protobuf_string(clipboard, 3, text);
        protobuf_string(clipboard, 6, text);
      }
      protobuf_string(
          request, 3,
          std::string(reinterpret_cast<const char *>(clipboard.data()),
                      clipboard.size()));
      protobuf_uint(request, 4,
                    strtoull(getenv("WETYPE_GROUP_ID"), nullptr, 10));
      timespec now{};
      clock_gettime(CLOCK_REALTIME, &now);
      protobuf_uint(request, 5,
                    uint64_t(now.tv_sec) * 1000 + now.tv_nsec / 1000000);
    } else if (operation_subcommand == 1000) {
      std::ifstream voice(getenv("WETYPE_VOICE_FILE"), std::ios::binary);
      std::string audio((std::istreambuf_iterator<char>(voice)), {});
      if (audio.empty() || audio.size() > 8388608)
        _exit(93);
      // Android's symbol-rich libvoiceopus.so proves OPUS files use
      // "#!OPUS_RAW_V1" + rate/channel bytes + uint16 packet lengths.
      // Convert a standard Ogg Opus recording into that exact stream.
      if (audio.size() >= 4 && audio.compare(0, 4, "OggS") == 0) {
        std::vector<std::string> packets;
        std::string packet;
        size_t page = 0;
        while (page + 27 <= audio.size() &&
               audio.compare(page, 4, "OggS") == 0) {
          const uint8_t count = uint8_t(audio[page + 26]);
          if (page + 27 + count > audio.size())
            _exit(93);
          size_t body = page + 27 + count;
          size_t bodySize = 0;
          for (uint8_t i = 0; i < count; ++i)
            bodySize += uint8_t(audio[page + 27 + i]);
          if (body + bodySize > audio.size())
            _exit(93);
          size_t cursor = body;
          for (uint8_t i = 0; i < count; ++i) {
            const uint8_t size = uint8_t(audio[page + 27 + i]);
            packet.append(audio, cursor, size);
            cursor += size;
            if (size < 255) {
              packets.push_back(std::move(packet));
              packet.clear();
            }
          }
          page = body + bodySize;
        }
        if (!packet.empty() || packets.size() < 3)
          _exit(93);
        std::string raw("#!OPUS_RAW_V1", 13);
        raw.push_back(char(2)); // 16 kHz
        raw.push_back(char(1)); // mono
        raw.push_back(char(0));
        for (size_t i = 2; i < packets.size(); ++i) {
          const auto &packetData = packets[i];
          if (packetData.empty() || packetData.size() > 1500)
            _exit(93);
          raw.push_back(char(packetData.size() & 255));
          raw.push_back(char((packetData.size() >> 8) & 255));
          raw += packetData;
        }
        audio = std::move(raw);
      }
      timespec timestamp{};
      clock_gettime(CLOCK_REALTIME, &timestamp);
      std::string voice_id = std::to_string(business_init_uin.load()) + "-" +
                             std::to_string(timestamp.tv_sec) + "-" +
                             std::to_string(timestamp.tv_nsec);
      size_t offset = 0;
      // ShortSentenceContainerV3 starts reqSeq at 0 and increments before the
      // first request, so the first actual audio packet is sequence 1.
      uint32_t sequence = 1;
      while (offset < audio.size()) {
        const size_t size = std::min<size_t>(3000, audio.size() - offset);
        std::vector<unsigned char> inner, outer;
        protobuf_string(inner, 2, voice_id);
        protobuf_uint(inner, 3, 0); // normal dictation scene
        protobuf_string(inner, 4, audio.substr(offset, size));
        protobuf_uint(inner, 5, 5); // OPUS
        protobuf_uint(inner, 6, offset + size == audio.size());
        protobuf_uint(inner, 7, sequence++);
        protobuf_uint(inner, 11, offset + size);
        protobuf_uint(inner, 14,
                      getenv("WETYPE_VOICE_POLISH") &&
                          strcmp(getenv("WETYPE_VOICE_POLISH"), "0"));
        protobuf_uint(inner, 19, 0); // no server push; return in response
        protobuf_string(
            outer, 1,
            std::string(reinterpret_cast<const char *>(inner.data()),
                        inner.size()));
        task_requests.push_back(std::move(outer));
        offset += size;
      }
      command_id = 4547;
      wire_subcommand = 0;
    } else if (operation_subcommand == 1001) {
      // wetype_file_transfer.proto, checked against the macOS 2.2.3 Objective-C
      // metadata. link_net_info is JSON used for role selection; the scanning
      // phone reports preferred_client=true and the QR host reports false.
      auto link = p2p_link_info_from_file(getenv("WETYPE_P2P_INFO_FILE"),
                                          identity.model.c_str());
      auto publicCer =
          p2p_public_cer_from_file(getenv("WETYPE_P2P_INFO_FILE"));
      if (!publicCer.empty())
        protobuf_string(request, 1, publicCer);
      if (!link.empty())
        protobuf_string(request, 2,
                        std::string(reinterpret_cast<const char *>(link.data()),
                                    link.size()));
      protobuf_string(request, 3, identity.model);
      // The upstream desktop advertises capability 0x2 and, when folder
      // transfer is enabled, 0x4. These are feature bits rather than link
      // types; WXP2P availability is conveyed by dispatch_buf.
      protobuf_uint(request, 4, 6);
      command_id = 4814;
      wire_subcommand = 73;
    } else if (operation_subcommand == 1002) {
      protobuf_string(request, 1, getenv("WETYPE_P2P_PEER"));
      command_id = 4814;
      wire_subcommand = 75;
    } else if (operation_subcommand == 1003) {
      auto link = p2p_link_info_from_file(getenv("WETYPE_P2P_INFO_FILE"),
                                          identity.model.c_str());
      auto publicCer =
          p2p_public_cer_from_file(getenv("WETYPE_P2P_INFO_FILE"));
      protobuf_uint(request, 1,
                    strtoull(getenv("WETYPE_P2P_INIT_UIN"), nullptr, 10));
      if (!publicCer.empty())
        protobuf_string(request, 2, publicCer);
      if (!link.empty())
        protobuf_string(request, 3,
                        std::string(reinterpret_cast<const char *>(link.data()),
                                    link.size()));
      protobuf_string(request, 4, identity.model);
      protobuf_uint(request, 5, 6);
      command_id = 4814;
      wire_subcommand = 77;
    } else if (operation_subcommand == 1004) {
      auto link = p2p_link_info_from_file(getenv("WETYPE_P2P_INFO_FILE"),
                                          identity.model.c_str());
      auto grpc = p2p_grpc_info_from_file(getenv("WETYPE_P2P_INFO_FILE"),
                                          identity.model.c_str());
      auto publicCer =
          p2p_public_cer_from_file(getenv("WETYPE_P2P_INFO_FILE"));
      protobuf_string(request, 1, getenv("WETYPE_P2P_UPDATE"));
      if (!publicCer.empty())
        protobuf_string(request, 2, publicCer);
      if (!link.empty())
        protobuf_string(request, 3,
                        std::string(reinterpret_cast<const char *>(link.data()),
                                    link.size()));
      if (!grpc.empty())
        protobuf_string(request, 4,
                        std::string(reinterpret_cast<const char *>(grpc.data()),
                                    grpc.size()));
      protobuf_uint(request, 6, 0);
      protobuf_uint(request, 7, 6);
      command_id = 4814;
      wire_subcommand = 74;
    }
    struct TaskOption {
      uint32_t timeout_ms;
      uint8_t retry_times;
      bool enable_socket_callback;
      uint16_t pad;
      uint32_t network_type;
      bool need_network_extra_json;
      unsigned char trailing_pad[3];
    } option{operation_subcommand == 1000 ? 30000u : 5000u,
             operation_subcommand == 1000 ? uint8_t(2) : uint8_t(3),
             false,
             0,
             0,
             false,
             {}};
    static_assert(sizeof(TaskOption) == 16);
    using StartTask =
        int (*)(uint32_t, uint32_t, const char *, uint32_t, void *,
                void (*)(void *, int, int, const char *, uint32_t, const char *,
                         uint32_t, const char *, uint32_t),
                const TaskOption *);
    if (task_requests.empty())
      task_requests.push_back(request);
    if (operation_subcommand == 1000) {
      business_task_callback_count = 0;
      business_task_error = -1;
      for (const auto &task_request : task_requests) {
        pairing_task =
            ((StartTask)syms.at("_business_net_start_task_with_option"))(
                command_id, wire_subcommand,
                reinterpret_cast<const char *>(task_request.data()),
                task_request.size(), nullptr, business_task_callback, &option);
        if (pairing_task < 0)
          break;
        usleep(120000);
      }
      for (unsigned wait = 0;
           wait < 300 && business_task_callback_count.load() <
                             static_cast<int>(task_requests.size());
           ++wait)
        usleep(100000);
      auto translated = protobuf_string_field(business_task_response, 1);
      collected_voice_text = protobuf_string_field(
          std::vector<unsigned char>(translated.begin(), translated.end()), 4);
    } else
      for (const auto &task_request : task_requests) {
        for (unsigned attempt = 0; attempt < 10; ++attempt) {
          business_task_callback_count = 0;
          business_task_error = -1;
          pairing_task =
              ((StartTask)syms.at("_business_net_start_task_with_option"))(
                  command_id, wire_subcommand,
                  reinterpret_cast<const char *>(task_request.data()),
                  task_request.size(), nullptr, business_task_callback,
                  &option);
          for (unsigned wait = 0;
               wait < 60 &&
               !business_task_callback_count.load(std::memory_order_acquire);
               ++wait)
            usleep(100000);
          if (business_task_error != 200007)
            break;
          usleep(300000);
        }
        if (operation_subcommand == 1000 && business_task_error == 0) {
          auto translated = protobuf_string_field(business_task_response, 1);
          auto text = protobuf_string_field(
              std::vector<unsigned char>(translated.begin(), translated.end()),
              4);
          if (!text.empty())
            collected_voice_text = std::move(text);
        }
        if (business_task_error != 0)
          break;
      }
  }
  auto calls = ((unsigned (*)())dlsym(RTLD_DEFAULT, "wcwss_bridge_calls"))();
  auto sends = ((unsigned (*)())dlsym(RTLD_DEFAULT, "wcwss_bridge_sends"))();
  auto receives =
      ((unsigned (*)())dlsym(RTLD_DEFAULT, "wcwss_bridge_receives"))();
  std::string response_match_code =
      operation_subcommand == 30
          ? protobuf_string_field(business_task_response, 2)
          : std::string();
  std::string response_bind_id =
      operation_subcommand == 40
          ? protobuf_string_field(business_task_response, 2)
          : std::string();
  uint64_t response_status =
      operation_subcommand == 40
          ? protobuf_uint_field(business_task_response, 1, UINT64_MAX)
          : UINT64_MAX;
  uint64_t response_show_reconfirm =
      operation_subcommand == 40
          ? protobuf_uint_field(business_task_response, 3, 0)
          : 0;
  uint64_t response_error_code = UINT64_MAX;
  if (operation_subcommand == 30 || operation_subcommand == 32 ||
      operation_subcommand == 35 || operation_subcommand == 36 ||
      operation_subcommand == 39)
    response_error_code =
        protobuf_uint_field(business_task_response, 1, UINT64_MAX);
  uint64_t response_group_id =
      operation_subcommand == 32
          ? protobuf_uint_field(business_task_response, 2, 0)
          : 0;
  std::vector<unsigned char> group;
  uint64_t response_func_switch = 0, response_clipboard_version = 0,
           response_hotword_version = 0;
  uint64_t response_clipboard_type = 0, response_clipboard_expire_at = 0,
           response_clipboard_expiration = 0, response_clipboard_from_uin = 0;
  std::string response_clipboard_binary;
  std::string response_clipboard;
  auto devices = wire::Json(json_object_new_array());
  auto pendingInvites = wire::Json(json_object_new_array());
  if (operation_subcommand == 34) {
    response_error_code =
        protobuf_uint_field(business_task_response, 2, UINT64_MAX);
    auto groups = protobuf_messages_field(business_task_response, 1);
    if (!groups.empty()) {
      group = std::move(groups.front());
      response_group_id = protobuf_uint_field(group, 1, 0);
      response_func_switch = protobuf_uint_field(group, 5, 0);
      if (getenv("WETYPE_PENDING_FILE_INVITE")) {
        const auto invites = protobuf_messages_field(group, 7);
        for (size_t i = 0; i < std::min<size_t>(invites.size(), 32); ++i) {
          auto invite = wire::object();
          wire::put(invite.get(), "transfer_code",
                    protobuf_string_field(invites[i], 1));
          wire::put(invite.get(), "device_name",
                    protobuf_string_field(invites[i], 2));
          wire::put(invite.get(), "sender_uin",
                    int64_t(protobuf_uint_field(invites[i], 3, 0)));
          json_object_array_add(pendingInvites.get(), invite.release());
        }
      }
      auto hotwords = protobuf_messages_field(group, 4);
      if (!hotwords.empty())
        response_hotword_version = protobuf_uint_field(hotwords.front(), 1, 0);
      auto clips = protobuf_messages_field(group, 3);
      if (!clips.empty()) {
        response_clipboard_version = protobuf_uint_field(clips.front(), 1, 0);
        response_clipboard_type = protobuf_uint_field(clips.front(), 2, 0);
        response_clipboard = protobuf_string_field(clips.front(), 3);
        response_clipboard_expire_at = protobuf_uint_field(clips.front(), 4, 0);
        response_clipboard_expiration =
            protobuf_uint_field(clips.front(), 5, 0);
        response_clipboard_binary = protobuf_string_field(clips.front(), 6);
        response_clipboard_from_uin = protobuf_uint_field(clips.front(), 7, 0);
        if (response_clipboard.empty() && response_clipboard_type == 1)
          response_clipboard = response_clipboard_binary;
      }
      for (auto &bound : protobuf_messages_field(group, 2)) {
        auto item = wire::object();
        wire::put(item.get(), "uin", int64_t(protobuf_uint_field(bound, 1, 0)));
        auto infos = protobuf_messages_field(bound, 2);
        if (!infos.empty()) {
          wire::put(item.get(), "name",
                    protobuf_string_field(infos.front(), 1));
          wire::put(item.get(), "platform",
                    int64_t(protobuf_uint_field(infos.front(), 2, 0)));
          wire::put(item.get(), "bind_time",
                    int64_t(protobuf_uint_field(infos.front(), 3, 0)));
          wire::put(item.get(), "client_version",
                    protobuf_string_field(infos.front(), 4));
        }
        json_object_array_add(devices.get(), item.release());
      }
    }
  }
  if (operation_subcommand == 36)
    response_clipboard_version =
        protobuf_uint_field(business_task_response, 2, 0);
  std::string response_voice_text;
  std::string response_voice_payload;
  if (operation_subcommand == 1000) {
    auto translated = protobuf_string_field(business_task_response, 1);
    response_voice_payload = base64_encode(translated);
    response_voice_text = protobuf_string_field(
        std::vector<unsigned char>(translated.begin(), translated.end()), 4);
    if (response_voice_text.empty())
      response_voice_text = collected_voice_text;
  }
  std::string response_transfer_code, response_transfer_url,
      response_transfer_image;
  uint64_t response_transfer_expiration = 0, response_transfer_expire_at = 0;
  if (operation_subcommand == 1001) {
    response_error_code =
        protobuf_uint_field(business_task_response, 1, UINT64_MAX);
    response_transfer_code = protobuf_string_field(business_task_response, 2);
    response_transfer_expire_at =
        protobuf_uint_field(business_task_response, 3, 0);
    response_transfer_expiration =
        protobuf_uint_field(business_task_response, 4, 0);
    response_transfer_url = protobuf_string_field(business_task_response, 5);
    response_transfer_image = protobuf_string_field(business_task_response, 6);
  } else if (operation_subcommand == 1003) {
    response_error_code =
        protobuf_uint_field(business_task_response, 1, UINT64_MAX);
    response_transfer_code = protobuf_string_field(business_task_response, 2);
    response_transfer_expire_at =
        protobuf_uint_field(business_task_response, 3, 0);
    response_transfer_expiration =
        protobuf_uint_field(business_task_response, 4, 0);
  }
  auto response_peer = wire::object();
  std::string response_dispatch_buf, response_peer_dispatch_buf;
  if (operation_subcommand == 1002 || operation_subcommand == 1004) {
    response_error_code =
        protobuf_uint_field(business_task_response, 1, UINT64_MAX);
    auto peerBytes = protobuf_string_field(business_task_response, 2);
    std::vector<unsigned char> peer(peerBytes.begin(), peerBytes.end());
    wire::put(response_peer.get(), "uin",
              int64_t(protobuf_uint_field(peer, 1, 0)));
    wire::put(response_peer.get(), "public_cer",
              base64_encode(protobuf_string_field(peer, 2)));
    wire::put(response_peer.get(), "link_net_info",
              base64_encode(protobuf_string_field(peer, 3)));
    wire::put(response_peer.get(), "public_ip", protobuf_string_field(peer, 4));
    wire::put(response_peer.get(), "ready",
              bool(protobuf_uint_field(peer, 5, 0)));
    wire::put(response_peer.get(), "grpc_net_info",
              base64_encode(protobuf_string_field(peer, 6)));
    wire::put(response_peer.get(), "dispatch_buf",
              base64_encode(protobuf_string_field(peer, 7)));
    wire::put(response_peer.get(), "biz_uin",
              int64_t(protobuf_uint_field(peer, 8, 0)));
    wire::put(response_peer.get(), "platform",
              int64_t(protobuf_uint_field(peer, 9, 0)));
    wire::put(response_peer.get(), "direct_info",
              base64_encode(protobuf_string_field(peer, 10)));
    wire::put(response_peer.get(), "state",
              int64_t(protobuf_uint_field(peer, 11, 0)));
    wire::put(response_peer.get(), "device_name",
              protobuf_string_field(peer, 12));
    wire::put(response_peer.get(), "capabilities",
              int64_t(protobuf_uint_field(peer, 13, 0)));
    response_dispatch_buf =
        base64_encode(protobuf_string_field(business_task_response, 5));
    response_peer_dispatch_buf =
        base64_encode(protobuf_string_field(business_task_response, 6));
  }
  auto report = wire::object();
  wire::put(report.get(), "business_initialize_result", int64_t(0));
  wire::put(report.get(), "callback_count",
            int64_t(business_init_callback_count.load()));
  wire::put(report.get(), "callback_error",
            int64_t(business_init_error.load()));
  wire::put(report.get(), "has_uin", bool(business_init_has_uin));
  wire::put(report.get(), "uin", int64_t(business_init_uin.load()));
  wire::put(report.get(), "device_code_bytes",
            int64_t(identity.device_code.size()));
  wire::put(report.get(), "bridge_calls", int64_t(calls));
  wire::put(report.get(), "sends", int64_t(sends));
  wire::put(report.get(), "receives", int64_t(receives));
  wire::put(report.get(), "pairing_task", int64_t(pairing_task));
  wire::put(report.get(), "pairing_callback_count",
            int64_t(business_task_callback_count.load()));
  wire::put(report.get(), "pairing_error", int64_t(business_task_error.load()));
  wire::put(report.get(), "pairing_response_bytes",
            int64_t(business_task_response.size()));
  wire::put(report.get(), "operation_subcommand",
            int64_t(operation_subcommand));
  wire::put(report.get(), "response_error_code", int64_t(response_error_code));
  wire::put(report.get(), "match_code", response_match_code);
  wire::put(report.get(), "status", int64_t(response_status));
  wire::put(report.get(), "bind_id", response_bind_id);
  wire::put(report.get(), "show_reconfirm", bool(response_show_reconfirm));
  wire::put(report.get(), "group_id", int64_t(response_group_id));
  wire::put(report.get(), "func_switch", int64_t(response_func_switch));
  wire::put(report.get(), "clipboard_version",
            int64_t(response_clipboard_version));
  wire::put(report.get(), "hotword_version", int64_t(response_hotword_version));
  wire::put(report.get(), "clipboard", response_clipboard);
  wire::put(report.get(), "clipboard_type", int64_t(response_clipboard_type));
  wire::put(report.get(), "clipboard_expire_at",
            int64_t(response_clipboard_expire_at));
  wire::put(report.get(), "clipboard_expiration",
            int64_t(response_clipboard_expiration));
  wire::put(report.get(), "clipboard_from_uin",
            int64_t(response_clipboard_from_uin));
  wire::put(report.get(), "clipboard_binary",
            !response_clipboard_binary.empty()
                ? base64_encode(response_clipboard_binary)
                : std::string());
  wire::put(report.get(), "voice_text", response_voice_text);
  wire::put(report.get(), "voice_payload", response_voice_payload);
  wire::put(report.get(), "transfer_code", response_transfer_code);
  wire::put(report.get(), "transfer_url", response_transfer_url);
  wire::put(report.get(), "transfer_image", response_transfer_image);
  wire::put(report.get(), "transfer_expire_at",
            int64_t(response_transfer_expire_at));
  wire::put(report.get(), "transfer_expiration",
            int64_t(response_transfer_expiration));
  wire::put(report.get(), "dispatch_buf", response_dispatch_buf);
  wire::put(report.get(), "peer_dispatch_buf", response_peer_dispatch_buf);
  json_object_object_add(report.get(), "peer", response_peer.release());
  json_object_object_add(report.get(), "devices", devices.release());
  if (getenv("WETYPE_PENDING_FILE_INVITE"))
    json_object_object_add(report.get(), "pending_invites",
                           pendingInvites.release());
  puts(wire::dump(report.get()).c_str());
  fflush(stdout);
  // The original report service owns delayed timers that may outlive this
  // This command is isolated and short lived; the operating system reclaims
  // delayed upstream timers after the response has been serialized.
  _exit(0);
}
