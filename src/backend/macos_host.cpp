// Fixed-address Mach-O compatibility host for the pinned original core.
// Unknown dependencies abort; they never silently return success.
#define _GNU_SOURCE 1
#include "../common/json.hpp"
#include <algorithm>
#include <array>
#include <arpa/inet.h>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <malloc.h>
#include <map>
#include <memory>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <set>
#include <setjmp.h>
#include <signal.h>
#include <sstream>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <vector>

static std::map<std::string, uintptr_t> syms;
static std::vector<std::pair<uintptr_t, std::string>> address_names;
static std::vector<std::pair<uintptr_t, uintptr_t>> mapped_image_ranges;
static uintptr_t tls_data = 0, tls_size = 0, tls_total = 0;
static uintptr_t stack_guard = 0xa35197bd82e436c1ULL;
static bool service_mode = false;
struct FlurryLocalCredentials {
  const char *ca_cert, *client_key, *client_cert;
  const char *server_key, *server_cert, *server_name;
};
static bool flurry_credentials_ready = false;
static std::string flurry_ca_cert, flurry_client_key, flurry_client_cert,
    flurry_server_key, flurry_server_cert, flurry_server_name;
static void flurry_credentials_callback(void *,
                                        const FlurryLocalCredentials *value,
                                        const char *error) {
  if (!value || error) {
    fprintf(stderr, "FLURRY_CREDENTIALS_ERROR %s\n", error ? error : "unknown");
    return;
  }
  fprintf(stderr,
          "FLURRY_CREDENTIALS_READY ca=%zu client_cert=%zu server_cert=%zu "
          "server_name=%zu\n",
          strlen(value->ca_cert), strlen(value->client_cert),
          strlen(value->server_cert), strlen(value->server_name));
  flurry_ca_cert = value->ca_cert;
  flurry_client_key = value->client_key;
  flurry_client_cert = value->client_cert;
  flurry_server_key = value->server_key;
  flurry_server_cert = value->server_cert;
  flurry_server_name = value->server_name;
  flurry_credentials_ready = true;
}
enum class FlurryStatusCode : int {
  Ok = 0,
  Canceled = 1,
  FilesystemFailed = 2
};
enum class FlurryLogLevel : int {
  Debug = 0,
  Info = 1,
  Warning = 2,
  Error = 3,
  Fatal = 4
};
enum class FlurryTaskEntryType : int { File = 0, Directory = 1 };
struct FlurryTaskInfo {
  const char *id;
  const char *file_path;
  size_t file_size;
  FlurryTaskEntryType entry_type;
};
struct FlurryUploadFileInfo {
  const char *file_path;
  const char *relative_path;
};
struct FlurryUploadDirectoryInfo {
  const char *directory_path;
  const char *relative_path;
};
class FlurryUploadDelegate {
public:
  virtual ~FlurryUploadDelegate() = default;
  virtual void OnReadFileError(const char *, FlurryStatusCode,
                               const char *) = 0;
  virtual void WillStartUploadTasks(const FlurryTaskInfo *, size_t) = 0;
  virtual void OnUploadTaskDone(const FlurryTaskInfo &, FlurryStatusCode,
                                const char *) = 0;
  virtual void OnUploadTaskProgress(const FlurryTaskInfo &, size_t, size_t) = 0;
};
class FlurryDownloadDelegate {
public:
  virtual ~FlurryDownloadDelegate() = default;
  virtual void WillStartDownloadTasks(const FlurryTaskInfo *, size_t) = 0;
  virtual void OnDownloadTaskDone(const FlurryTaskInfo &, FlurryStatusCode,
                                  const char *) = 0;
  virtual void OnDownloadTaskProgress(const FlurryTaskInfo &, size_t,
                                      size_t) = 0;
};
class FlurryMessageDelegate {
public:
  virtual ~FlurryMessageDelegate() = default;
  virtual void OnMessage(const char *, size_t) = 0;
  virtual void OnMessageSent(int, bool, const char *) = 0;
};
class FlurryLogger {
public:
  virtual ~FlurryLogger() = default;
  virtual void Log(FlurryLogLevel, const char *) = 0;
};
class FlurryDelegate : public FlurryUploadDelegate,
                       public FlurryDownloadDelegate,
                       public FlurryMessageDelegate,
                       public FlurryLogger {
public:
  ~FlurryDelegate() override = default;
  virtual void OnClientConnected() = 0;
  virtual void OnPeerDisconnected() = 0;
};
class FlurryWXP2PDelegate : public FlurryUploadDelegate,
                            public FlurryDownloadDelegate,
                            public FlurryMessageDelegate,
                            public FlurryLogger {
public:
  ~FlurryWXP2PDelegate() override = default;
  // Keep these declarations on the intermediate interface.  Clang places
  // them immediately after IFlurryUploadDelegate's slots, exactly as in the
  // upstream IFlurryWXP2PDelegate ABI. Declaring them only on the final class
  // moves them behind the other inherited interfaces and misroutes callbacks.
  virtual void OnConnected(int type) = 0;
  virtual void OnConnectBroken(int error) = 0;
  virtual void OnConnectEndInfo(uint64_t room, const char *report,
                                size_t length) = 0;
};

enum WXP2PTransferEvent {
  WXP2PNone = 0,
  WXP2PConnected = 1,
  WXP2PSendReady = 2,
  WXP2PSendResult = 3,
  WXP2PBroken = 4,
  WXP2PConnectEnd = 5,
};
class WXP2PTransferCallback {
public:
  virtual ~WXP2PTransferCallback() = default;
  virtual int OnRecvData(const unsigned char *, int, int) = 0;
  virtual int OnP2PEvent(WXP2PTransferEvent, int, void *, int) = 0;
  virtual void OnWriteLog(int, const char *, int, const char *, const char *,
                          unsigned) {}
};
class WXP2PTransfer {
public:
  virtual ~WXP2PTransfer() = default;
  virtual int StartSession() = 0;
  virtual int StopSession() = 0;
  virtual int SendData(const unsigned char *, int, int, int) = 0;
};
class WXP2PProbeCallback final : public WXP2PTransferCallback {
public:
  std::atomic<bool> connected{false}, ended{false};
  int OnRecvData(const unsigned char *, int length, int chunk) override {
    fprintf(stdout, "{\"event\":\"wxp2p_data\",\"length\":%d,\"chunk\":%d}\n",
            length, chunk);
    fflush(stdout);
    return 0;
  }
  int OnP2PEvent(WXP2PTransferEvent event, int error, void *data,
                 int length) override {
    int mode = 0;
    if (event == WXP2PConnected && data && length >= int(sizeof(mode)))
      memcpy(&mode, data, sizeof(mode));
    if (event == WXP2PConnected)
      connected = error == 0;
    if (event == WXP2PBroken || event == WXP2PConnectEnd)
      ended = true;
    fprintf(stdout,
            "{\"event\":\"wxp2p_status\",\"type\":%d,\"error\":%d,"
            "\"mode\":%d,\"bytes\":%d}\n",
            int(event), error, mode, length);
    fflush(stdout);
    return 0;
  }
  void OnWriteLog(int level, const char *file, int line, const char *function,
                  const char *data, unsigned length) override {
    if (getenv("WETYPE_HOST_DEBUG"))
      fprintf(stderr, "WXP2P[%d] %s:%d %s %.*s\n", level, file ? file : "",
              line, function ? function : "", int(length), data ? data : "");
  }
};

static int run_wxp2p_probe() {
  const char *path = getenv("WETYPE_WXP2P_DISPATCH_FILE");
  if (!path || !*path)
    return 2;
  std::ifstream input(path, std::ios::binary);
  std::string dispatch((std::istreambuf_iterator<char>(input)), {});
  if (dispatch.empty() || dispatch.size() > 1048576)
    return 2;
  std::vector<unsigned char> config;
  auto appendVarint = [&](uint64_t value) {
    do {
      unsigned char byte = value & 0x7f;
      value >>= 7;
      config.push_back(byte | (value ? 0x80 : 0));
    } while (value);
  };
  appendVarint(0x0a);
  appendVarint(dispatch.size());
  config.insert(config.end(), dispatch.begin(), dispatch.end());
  using Create =
      WXP2PTransfer *(*)(WXP2PTransferCallback *, const unsigned char *, int);
  auto create = reinterpret_cast<Create>(syms.at(
      "__ZN13wxp2ptransfer19CreateWXP2PTransferEPNS_22IWXP2PTransferCallbackE"
      "PKhi"));
  WXP2PProbeCallback callback;
  auto *session = create(&callback, config.data(), int(config.size()));
  if (!session) {
    fputs("WXP2P create failed\n", stderr);
    return 2;
  }
  int started = session->StartSession();
  fprintf(stdout, "{\"event\":\"wxp2p_started\",\"result\":%d}\n", started);
  fflush(stdout);
  for (unsigned i = 0; i < 600 && !callback.connected && !callback.ended; ++i)
    usleep(50000);
  session->StopSession();
  delete session;
  return callback.connected ? 0 : 3;
}
class FlurryTestDelegate final : public FlurryDelegate {
public:
  std::atomic<bool> connected{false}, received{false}, sent{false};
  std::string message;
  void OnReadFileError(const char *, FlurryStatusCode, const char *) override {}
  void WillStartUploadTasks(const FlurryTaskInfo *, size_t) override {}
  void OnUploadTaskDone(const FlurryTaskInfo &, FlurryStatusCode,
                        const char *) override {}
  void OnUploadTaskProgress(const FlurryTaskInfo &, size_t, size_t) override {}
  void WillStartDownloadTasks(const FlurryTaskInfo *, size_t) override {}
  void OnDownloadTaskDone(const FlurryTaskInfo &, FlurryStatusCode,
                          const char *) override {}
  void OnDownloadTaskProgress(const FlurryTaskInfo &, size_t, size_t) override {
  }
  void OnMessage(const char *data, size_t size) override {
    message.assign(data, size);
    received = true;
  }
  void OnMessageSent(int, bool success, const char *) override {
    sent = success;
  }
  void Log(FlurryLogLevel level, const char *text) override {
    if (getenv("WETYPE_HOST_DEBUG"))
      fprintf(stderr, "FLURRY[%d] %s\n", int(level), text ? text : "");
  }
  void OnClientConnected() override { connected = true; }
  void OnPeerDisconnected() override { connected = false; }
};
static std::mutex flurry_event_lock;
static void emit_flurry_event(const char *type, const char *path = nullptr,
                              size_t transferred = 0, size_t total = 0,
                              int status = 0, const char *error = nullptr) {
  auto event = wire::object();
  wire::put(event.get(), "event", type);
  if (path)
    wire::put(event.get(), "path", path);
  wire::put(event.get(), "transferred", int64_t(transferred));
  wire::put(event.get(), "total", int64_t(total));
  wire::put(event.get(), "status", int64_t(status));
  if (error)
    wire::put(event.get(), "error", error);
  std::lock_guard lock(flurry_event_lock);
  puts(wire::dump(event.get()).c_str());
  fflush(stdout);
}
static void emit_flurry_grpc_ready(unsigned port) {
  auto event = wire::object();
  wire::put(event.get(), "event", "grpc_ready");
  wire::put(event.get(), "port", int64_t(port));
  wire::put(event.get(), "ca_cert", flurry_ca_cert);
  wire::put(event.get(), "server_name", flurry_server_name);
  std::lock_guard lock(flurry_event_lock);
  puts(wire::dump(event.get()).c_str());
  fflush(stdout);
}
class FlurryLiveDelegate final : public FlurryDelegate {
public:
  std::atomic<bool> connected{false};
  void OnReadFileError(const char *path, FlurryStatusCode status,
                       const char *error) override {
    emit_flurry_event("read_error", path, 0, 0, int(status), error);
  }
  void WillStartUploadTasks(const FlurryTaskInfo *tasks,
                            size_t count) override {
    emit_flurry_event("upload_start",
                      count && tasks ? tasks[0].file_path : nullptr, 0, count);
  }
  void OnUploadTaskDone(const FlurryTaskInfo &task, FlurryStatusCode status,
                        const char *error) override {
    emit_flurry_event("upload_done", task.file_path, task.file_size,
                      task.file_size, int(status), error);
  }
  void OnUploadTaskProgress(const FlurryTaskInfo &task, size_t sent,
                            size_t total) override {
    emit_flurry_event("upload_progress", task.file_path, sent, total);
  }
  void WillStartDownloadTasks(const FlurryTaskInfo *tasks,
                              size_t count) override {
    emit_flurry_event("download_start",
                      count && tasks ? tasks[0].file_path : nullptr, 0, count);
  }
  void OnDownloadTaskDone(const FlurryTaskInfo &task, FlurryStatusCode status,
                          const char *error) override {
    emit_flurry_event("download_done", task.file_path, task.file_size,
                      task.file_size, int(status), error);
  }
  void OnDownloadTaskProgress(const FlurryTaskInfo &task, size_t written,
                              size_t total) override {
    emit_flurry_event("download_progress", task.file_path, written, total);
  }
  void OnMessage(const char *, size_t size) override {
    emit_flurry_event("message", nullptr, size, size);
  }
  void OnMessageSent(int id, bool success, const char *error) override {
    emit_flurry_event("message_sent", nullptr, id, id, success ? 0 : 1, error);
  }
  void Log(FlurryLogLevel level, const char *text) override {
    if (getenv("WETYPE_HOST_DEBUG"))
      fprintf(stderr, "FLURRY[%d] %s\n", int(level), text ? text : "");
  }
  void OnClientConnected() override {
    connected = true;
    emit_flurry_event("connected");
  }
  void OnPeerDisconnected() override {
    connected = false;
    emit_flurry_event("disconnected");
  }
};
class FlurryWXP2PLiveDelegate final : public FlurryWXP2PDelegate {
public:
  std::atomic<bool> connected{false};
  void OnReadFileError(const char *path, FlurryStatusCode status,
                       const char *error) override {
    emit_flurry_event("read_error", path, 0, 0, int(status), error);
  }
  void WillStartUploadTasks(const FlurryTaskInfo *tasks,
                            size_t count) override {
    emit_flurry_event("upload_start",
                      count && tasks ? tasks[0].file_path : nullptr, 0, count);
  }
  void OnUploadTaskDone(const FlurryTaskInfo &task, FlurryStatusCode status,
                        const char *error) override {
    emit_flurry_event("upload_done", task.file_path, task.file_size,
                      task.file_size, int(status), error);
  }
  void OnUploadTaskProgress(const FlurryTaskInfo &task, size_t sent,
                            size_t total) override {
    emit_flurry_event("upload_progress", task.file_path, sent, total);
  }
  void WillStartDownloadTasks(const FlurryTaskInfo *tasks,
                              size_t count) override {
    emit_flurry_event("download_start",
                      count && tasks ? tasks[0].file_path : nullptr, 0, count);
  }
  void OnDownloadTaskDone(const FlurryTaskInfo &task, FlurryStatusCode status,
                          const char *error) override {
    emit_flurry_event("download_done", task.file_path, task.file_size,
                      task.file_size, int(status), error);
  }
  void OnDownloadTaskProgress(const FlurryTaskInfo &task, size_t written,
                              size_t total) override {
    emit_flurry_event("download_progress", task.file_path, written, total);
  }
  void OnMessage(const char *, size_t size) override {
    emit_flurry_event("message", nullptr, size, size);
  }
  void OnMessageSent(int id, bool success, const char *error) override {
    emit_flurry_event("message_sent", nullptr, id, id, success ? 0 : 1, error);
  }
  void Log(FlurryLogLevel level, const char *text) override {
    if (getenv("WETYPE_HOST_DEBUG"))
      fprintf(stderr, "FLURRY_WXP2P[%d] %s\n", int(level), text ? text : "");
  }
  void OnConnected(int type) override {
    connected = true;
    emit_flurry_event("connected", nullptr, type, type);
  }
  void OnConnectBroken(int error) override {
    connected = false;
    emit_flurry_event("disconnected", nullptr, 0, 0, error);
  }
  void OnConnectEndInfo(uint64_t room, const char *, size_t length) override {
    emit_flurry_event("connect_end", nullptr, room, length);
  }
};
struct FlurryGrpcCredentials {
  const char *local_cert;
  const char *local_key;
  const char *remote_ca_cert;
  const char *server_name;
};
static bool run_flurry_loopback_test() {
  char server_download[] = "/tmp/wetypex-flurry-server.XXXXXX";
  char client_download[] = "/tmp/wetypex-flurry-client.XXXXXX";
  if (!mkdtemp(server_download) || !mkdtemp(client_download))
    return false;
  std::string server_temp = std::string(server_download) + "/.parts";
  std::string client_temp = std::string(client_download) + "/.parts";
  mkdir(server_temp.c_str(), 0700);
  mkdir(client_temp.c_str(), 0700);

  int probe = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
  sockaddr_in6 address{};
  address.sin6_family = AF_INET6;
  address.sin6_addr = in6addr_any;
  address.sin6_port = 0;
  socklen_t address_size = sizeof(address);
  if (probe < 0 ||
      bind(probe, reinterpret_cast<sockaddr *>(&address), sizeof(address)) ||
      getsockname(probe, reinterpret_cast<sockaddr *>(&address),
                  &address_size)) {
    if (probe >= 0)
      close(probe);
    return false;
  }
  const unsigned port = ntohs(address.sin6_port);
  close(probe);

  using Constructor =
      void (*)(void *, const char *, const char *, FlurryDelegate *, int);
  using StartServer =
      bool (*)(void *, const char *, const FlurryGrpcCredentials &);
  using Connect =
      bool (*)(void *, const char *, const FlurryGrpcCredentials &, int);
  using SendMessage = void (*)(void *, int, const char *, size_t);
  auto constructor = reinterpret_cast<Constructor>(
      syms.at("__ZN6flurry6FlurryC1EPKcS2_PNS_15IFlurryDelegateENS_"
              "14PeerAckSupportE"));
  auto start_server = reinterpret_cast<StartServer>(
      syms.at("__ZN6flurry6Flurry11StartServerEPKcRKNS_15GrpcCredentialsE"));
  auto connect = reinterpret_cast<Connect>(
      syms.at("__ZN6flurry6Flurry7ConnectEPKcRKNS_15GrpcCredentialsEi"));
  auto send_message = reinterpret_cast<SendMessage>(
      syms.at("__ZN6flurry6Flurry11SendMessageEiPKcm"));

  FlurryTestDelegate server_delegate, client_delegate;
  alignas(16) unsigned char server[32]{}, client[32]{};
  constructor(server, server_download, server_temp.c_str(), &server_delegate,
              0);
  constructor(client, client_download, client_temp.c_str(), &client_delegate,
              0);
  FlurryGrpcCredentials server_credentials{flurry_server_cert.c_str(),
                                           flurry_server_key.c_str(),
                                           flurry_ca_cert.c_str(), nullptr};
  FlurryGrpcCredentials client_credentials{
      flurry_client_cert.c_str(), flurry_client_key.c_str(),
      flurry_ca_cert.c_str(), flurry_server_name.c_str()};
  std::string listen = "[::]:" + std::to_string(port);
  std::string destination = "127.0.0.1:" + std::to_string(port);
  bool started = start_server(server, listen.c_str(), server_credentials);
  bool connected =
      started && connect(client, destination.c_str(), client_credentials, 5);
  static const char message[] = "wetypex-flurry-loopback";
  if (connected)
    send_message(client, 1, message, sizeof(message) - 1);
  for (unsigned i = 0;
       i < 100 && !(client_delegate.sent && server_delegate.received); ++i)
    usleep(20000);
  const bool passed = connected && client_delegate.sent &&
                      server_delegate.received &&
                      server_delegate.message == message;
  fprintf(stderr,
          "FLURRY_LOOPBACK server=%d connected=%d sent=%d received=%d\n",
          started, connected, bool(client_delegate.sent),
          bool(server_delegate.received));
  // The library requires destruction to happen asynchronously after its
  // disconnect callback. The loopback verifier exits immediately below, so
  // the operating system owns final teardown of the two short-lived peers.
  return passed;
}
static int run_flurry_server() {
  const char *download = getenv("WETYPE_FLURRY_DOWNLOAD_DIR");
  const char *temporary = getenv("WETYPE_FLURRY_TEMP_DIR");
  if (!download || !*download || !temporary || !*temporary) {
    fputs("flurry-server requires download and temporary directories\n",
          stderr);
    return 2;
  }
  mkdir(download, 0700);
  mkdir(temporary, 0700);
  int probe = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
  sockaddr_in6 address{};
  address.sin6_family = AF_INET6;
  address.sin6_addr = in6addr_any;
  socklen_t address_size = sizeof(address);
  if (probe < 0 ||
      bind(probe, reinterpret_cast<sockaddr *>(&address), sizeof(address)) ||
      getsockname(probe, reinterpret_cast<sockaddr *>(&address),
                  &address_size)) {
    if (probe >= 0)
      close(probe);
    return 2;
  }
  const unsigned port = ntohs(address.sin6_port);
  close(probe);
  using Constructor =
      void (*)(void *, const char *, const char *, FlurryDelegate *, int);
  using StartServer =
      bool (*)(void *, const char *, const FlurryGrpcCredentials &);
  using Connect =
      bool (*)(void *, const char *, const FlurryGrpcCredentials &, int);
  using UploadFiles = void (*)(void *, const FlurryUploadFileInfo *, size_t);
  using UploadEntries = void (*)(void *, const FlurryUploadFileInfo *, size_t,
                                 const FlurryUploadDirectoryInfo *, size_t);
  auto constructor = reinterpret_cast<Constructor>(
      syms.at("__ZN6flurry6FlurryC1EPKcS2_PNS_15IFlurryDelegateENS_"
              "14PeerAckSupportE"));
  auto start_server = reinterpret_cast<StartServer>(
      syms.at("__ZN6flurry6Flurry11StartServerEPKcRKNS_15GrpcCredentialsE"));
  auto connect_peer = reinterpret_cast<Connect>(
      syms.at("__ZN6flurry6Flurry7ConnectEPKcRKNS_15GrpcCredentialsEi"));
  auto upload_files = reinterpret_cast<UploadFiles>(
      syms.at("__ZN6flurry6Flurry11UploadFilesEPKNS_14UploadFileInfoEm"));
  auto upload_entries = reinterpret_cast<UploadEntries>(
      syms.at("__ZN6flurry6Flurry13UploadEntriesEPKNS_14UploadFileInfoEmPKNS_"
              "19UploadDirectoryInfoEm"));
  FlurryLiveDelegate delegate;
  alignas(16) unsigned char server[32]{};
  constructor(server, download, temporary, &delegate, 0);
  auto ready = wire::object();
  wire::put(ready.get(), "event", "ready");
  wire::put(ready.get(), "port", int64_t(port));
  wire::put(ready.get(), "ca_cert", flurry_ca_cert);
  wire::put(ready.get(), "server_name", flurry_server_name);
  puts(wire::dump(ready.get()).c_str());
  fflush(stdout);
  if (const char *ready_path = getenv("WETYPE_FLURRY_READY_FILE")) {
    std::ofstream output(ready_path, std::ios::trunc);
    output << wire::dump(ready.get()) << '\n';
  }
  std::string command;
  if (!std::getline(std::cin, command))
    return 2;
  auto role = wire::parse(command);
  const auto action = wire::str(role.get(), "action");
  const auto remote_ca_file = wire::str(role.get(), "remote_ca_file");
  std::ifstream remote_ca_stream(remote_ca_file);
  std::string remote_ca((std::istreambuf_iterator<char>(remote_ca_stream)), {});
  bool role_started = false;
  if (action == "server" && !remote_ca.empty()) {
    FlurryGrpcCredentials credentials{flurry_server_cert.c_str(),
                                      flurry_server_key.c_str(),
                                      remote_ca.c_str(), nullptr};
    std::string listen = "[::]:" + std::to_string(port);
    role_started = start_server(server, listen.c_str(), credentials);
  } else if (action == "connect" && !remote_ca.empty()) {
    const auto destination = wire::str(role.get(), "server_addr");
    const auto remote_server_name = wire::str(role.get(), "server_name");
    if (!destination.empty() && !remote_server_name.empty()) {
      FlurryGrpcCredentials credentials{
          flurry_client_cert.c_str(), flurry_client_key.c_str(),
          remote_ca.c_str(), remote_server_name.c_str()};
      role_started = connect_peer(server, destination.c_str(), credentials, 10);
    }
  }
  if (!role_started) {
    emit_flurry_event("start_failed");
    return 2;
  }
  emit_flurry_event("role_started");
  while (std::getline(std::cin, command)) {
    auto request = wire::parse(command);
    const auto request_action = wire::str(request.get(), "action");
    if (request_action == "quit")
      _exit(0);
    if (request_action != "upload" && request_action != "upload_directory")
      continue;
    const auto path = wire::str(request.get(), "path");
    struct stat file_status{};
    if (path.empty() || stat(path.c_str(), &file_status) ||
        (request_action == "upload" ? !S_ISREG(file_status.st_mode)
                                    : !S_ISDIR(file_status.st_mode))) {
      emit_flurry_event("read_error", path.c_str(), 0, 0,
                        int(FlurryStatusCode::FilesystemFailed),
                        "file does not exist");
      continue;
    }
    if (request_action == "upload") {
      FlurryUploadFileInfo file{path.c_str(), nullptr};
      upload_files(server, &file, 1);
      continue;
    }
    std::filesystem::path root(path);
    const auto root_name = root.filename().string();
    std::vector<std::string> file_paths, file_relatives, directory_paths,
        directory_relatives;
    directory_paths.push_back(root.string());
    directory_relatives.push_back(root_name);
    std::error_code traversal_error;
    for (std::filesystem::recursive_directory_iterator
             iterator(root, traversal_error),
         end;
         !traversal_error && iterator != end;
         iterator.increment(traversal_error)) {
      auto relative =
          (std::filesystem::path(root_name) /
           std::filesystem::relative(iterator->path(), root, traversal_error))
              .generic_string();
      if (traversal_error)
        break;
      if (iterator->is_directory()) {
        directory_paths.push_back(iterator->path().string());
        directory_relatives.push_back(std::move(relative));
      } else if (iterator->is_regular_file()) {
        file_paths.push_back(iterator->path().string());
        file_relatives.push_back(std::move(relative));
      }
    }
    if (traversal_error) {
      emit_flurry_event("read_error", path.c_str(), 0, 0,
                        int(FlurryStatusCode::FilesystemFailed),
                        traversal_error.message().c_str());
      continue;
    }
    std::vector<FlurryUploadFileInfo> files;
    std::vector<FlurryUploadDirectoryInfo> directories;
    for (size_t i = 0; i < file_paths.size(); ++i)
      files.push_back({file_paths[i].c_str(), file_relatives[i].c_str()});
    for (size_t i = 0; i < directory_paths.size(); ++i)
      directories.push_back(
          {directory_paths[i].c_str(), directory_relatives[i].c_str()});
    upload_entries(server, files.data(), files.size(), directories.data(),
                   directories.size());
  }
  _exit(0);
}
static int run_flurry_wxp2p_server() {
  const char *download = getenv("WETYPE_FLURRY_DOWNLOAD_DIR");
  const char *temporary = getenv("WETYPE_FLURRY_TEMP_DIR");
  if (!download || !*download || !temporary || !*temporary)
    return 2;
  mkdir(download, 0700);
  mkdir(temporary, 0700);
  using Constructor = void (*)(void *, const char *, const char *,
                               FlurryWXP2PLiveDelegate *, const char *, size_t);
  using GrpcConstructor =
      void (*)(void *, const char *, const char *, FlurryDelegate *, int);
  using StartServer =
      bool (*)(void *, const char *, const FlurryGrpcCredentials &);
  using UploadFiles = void (*)(void *, const FlurryUploadFileInfo *, size_t);
  using UploadEntries = void (*)(void *, const FlurryUploadFileInfo *, size_t,
                                 const FlurryUploadDirectoryInfo *, size_t);
  auto constructor = reinterpret_cast<Constructor>(
      syms.at("__ZN6flurry11FlurryWXP2PC1EPKcS2_PNS_20IFlurryWXP2PDelegateES2_"
              "m"));
  auto uploadFiles = reinterpret_cast<UploadFiles>(
      syms.at("__ZN6flurry11FlurryWXP2P11UploadFilesEPKNS_14UploadFileInfoEm"));
  auto uploadEntries = reinterpret_cast<UploadEntries>(syms.at(
      "__ZN6flurry11FlurryWXP2P13UploadEntriesEPKNS_14UploadFileInfoEmPKNS_"
      "19UploadDirectoryInfoEm"));
  auto grpcConstructor = reinterpret_cast<GrpcConstructor>(
      syms.at("__ZN6flurry6FlurryC1EPKcS2_PNS_15IFlurryDelegateENS_"
              "14PeerAckSupportE"));
  auto startServer = reinterpret_cast<StartServer>(
      syms.at("__ZN6flurry6Flurry11StartServerEPKcRKNS_15GrpcCredentialsE"));
  FlurryWXP2PLiveDelegate delegate;
  FlurryLiveDelegate grpcDelegate;
  alignas(16) unsigned char session[32]{};
  alignas(16) unsigned char grpcServer[32]{};
  std::string dispatchPath = getenv("WETYPE_WXP2P_DISPATCH_FILE")
                                 ? getenv("WETYPE_WXP2P_DISPATCH_FILE")
                                 : "";
  std::string remoteCaPath = getenv("WETYPE_FLURRY_REMOTE_CA_FILE")
                                 ? getenv("WETYPE_FLURRY_REMOTE_CA_FILE")
                                 : "";
  std::string pendingCommand;
  if (dispatchPath.empty()) {
    auto ready = wire::object();
    wire::put(ready.get(), "event", "ready");
    wire::put(ready.get(), "transport", "wxp2p");
    wire::put(ready.get(), "ca_cert", flurry_ca_cert);
    wire::put(ready.get(), "server_name", flurry_server_name);
    puts(wire::dump(ready.get()).c_str());
    fflush(stdout);
    while (std::getline(std::cin, pendingCommand)) {
      auto request = wire::parse(pendingCommand);
      const auto action = wire::str(request.get(), "action");
      if (action == "quit")
        _exit(0);
      if (action == "wxp2p_start") {
        dispatchPath = wire::str(request.get(), "dispatch_path");
        remoteCaPath = wire::str(request.get(), "remote_ca_file");
        break;
      }
    }
  }
  std::ifstream dispatchInput(dispatchPath, std::ios::binary);
  std::string dispatch((std::istreambuf_iterator<char>(dispatchInput)), {});
  if (dispatch.empty() || dispatch.size() > 1048576) {
    emit_flurry_event("start_failed", dispatchPath.c_str(), 0, 0, 2,
                      "invalid WXP2P dispatch result");
    return 2;
  }
  constructor(session, download, temporary, &delegate, dispatch.data(),
              dispatch.size());
  emit_flurry_event("role_started");
  if (!remoteCaPath.empty()) {
    std::ifstream remoteCaInput(remoteCaPath);
    std::string remoteCa((std::istreambuf_iterator<char>(remoteCaInput)), {});
    int probe = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    address.sin6_addr = in6addr_any;
    socklen_t addressSize = sizeof(address);
    bool serverReady = probe >= 0 &&
                       !bind(probe, reinterpret_cast<sockaddr *>(&address),
                             sizeof(address)) &&
                       !getsockname(probe,
                                    reinterpret_cast<sockaddr *>(&address),
                                    &addressSize);
    const unsigned port = serverReady ? ntohs(address.sin6_port) : 0;
    if (probe >= 0)
      close(probe);
    if (serverReady && !remoteCa.empty()) {
      // Current mobile clients advertise capability 0x2, which the upstream
      // controller maps to PeerAckSupport::Supported. Without this, the file
      // is saved locally but the sender never receives its completion ACK and
      // reports a false failure.
      grpcConstructor(grpcServer, download, temporary, &grpcDelegate, 1);
      FlurryGrpcCredentials credentials{flurry_server_cert.c_str(),
                                        flurry_server_key.c_str(),
                                        remoteCa.c_str(), nullptr};
      const std::string listen = "[::]:" + std::to_string(port);
      serverReady = startServer(grpcServer, listen.c_str(), credentials);
    } else {
      serverReady = false;
    }
    if (serverReady)
      emit_flurry_grpc_ready(port);
    else
      emit_flurry_event("grpc_start_failed", remoteCaPath.c_str(), 0, 0, 2,
                        "unable to start Flurry server");
  }
  std::string command;
  while (std::getline(std::cin, command)) {
    auto request = wire::parse(command);
    const auto action = wire::str(request.get(), "action");
    if (action == "quit")
      _exit(0);
    if (action != "upload" && action != "upload_directory")
      continue;
    const auto path = wire::str(request.get(), "path");
    struct stat fileStatus{};
    if (path.empty() || stat(path.c_str(), &fileStatus) ||
        (action == "upload" ? !S_ISREG(fileStatus.st_mode)
                            : !S_ISDIR(fileStatus.st_mode))) {
      emit_flurry_event("read_error", path.c_str(), 0, 0,
                        int(FlurryStatusCode::FilesystemFailed),
                        "file does not exist");
      continue;
    }
    if (action == "upload") {
      FlurryUploadFileInfo file{path.c_str(), nullptr};
      uploadFiles(session, &file, 1);
      continue;
    }
    std::filesystem::path root(path);
    const auto rootName = root.filename().string();
    std::vector<std::string> filePaths, fileRelatives, directoryPaths,
        directoryRelatives;
    directoryPaths.push_back(root.string());
    directoryRelatives.push_back(rootName);
    std::error_code traversalError;
    for (std::filesystem::recursive_directory_iterator
             iterator(root, traversalError),
         end;
         !traversalError && iterator != end;
         iterator.increment(traversalError)) {
      auto relative =
          (std::filesystem::path(rootName) /
           std::filesystem::relative(iterator->path(), root, traversalError))
              .generic_string();
      if (traversalError)
        break;
      if (iterator->is_directory()) {
        directoryPaths.push_back(iterator->path().string());
        directoryRelatives.push_back(std::move(relative));
      } else if (iterator->is_regular_file()) {
        filePaths.push_back(iterator->path().string());
        fileRelatives.push_back(std::move(relative));
      }
    }
    if (traversalError) {
      emit_flurry_event("read_error", path.c_str(), 0, 0,
                        int(FlurryStatusCode::FilesystemFailed),
                        traversalError.message().c_str());
      continue;
    }
    std::vector<FlurryUploadFileInfo> files;
    std::vector<FlurryUploadDirectoryInfo> directories;
    for (size_t i = 0; i < filePaths.size(); ++i)
      files.push_back({filePaths[i].c_str(), fileRelatives[i].c_str()});
    for (size_t i = 0; i < directoryPaths.size(); ++i)
      directories.push_back(
          {directoryPaths[i].c_str(), directoryRelatives[i].c_str()});
    uploadEntries(session, files.data(), files.size(), directories.data(),
                  directories.size());
  }
  _exit(0);
}
alignas(16) static unsigned char default_rune_locale[4096];
static uint32_t rune_mask(int c) {
  uint32_t r = 0;
  if (c >= 0 && c < 256) {
    if (isalpha(c))
      r |= 0x100;
    if (iscntrl(c))
      r |= 0x200;
    if (isdigit(c))
      r |= 0x400;
    if (islower(c))
      r |= 0x1000;
    if (ispunct(c))
      r |= 0x2000;
    if (isspace(c))
      r |= 0x4000;
    if (isupper(c))
      r |= 0x8000;
    if (isxdigit(c))
      r |= 0x10000;
    if (isblank(c))
      r |= 0x20000;
    if (isprint(c))
      r |= 0x40000;
  }
  return r;
}
extern "C" unsigned long shim_maskrune(int c, unsigned long mask) {
  return rune_mask(c) & mask;
}
static void initialize_rune_locale() {
  memcpy(default_rune_locale, "RuneMagA", 8);
  memcpy(default_rune_locale + 8, "UTF-8", 6);
  *(int32_t *)(default_rune_locale + 56) = -1;
  for (int c = 0; c < 256; c++) {
    *(uint32_t *)(default_rune_locale + 60 + c * 4) = rune_mask(c);
    *(int32_t *)(default_rune_locale + 1084 + c * 4) = tolower(c);
    *(int32_t *)(default_rune_locale + 2108 + c * 4) = toupper(c);
  }
}
static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
static std::map<void *, pthread_mutex_t *> mutexes;
static std::map<void *, pthread_cond_t *> conditions;
static std::map<void *, pthread_rwlock_t *> rwlocks;
static std::map<void *, pthread_attr_t *> thread_attributes;
static pthread_attr_t *thread_attribute_for(void *value) {
  auto found = thread_attributes.find(value);
  return found == thread_attributes.end() ? nullptr : found->second;
}
extern "C" int shim_pthread_attr_init(void *value) {
  auto *attribute = new pthread_attr_t;
  int result = pthread_attr_init(attribute);
  if (result) {
    delete attribute;
    return result;
  }
  pthread_mutex_lock(&registry_lock);
  thread_attributes[value] = attribute;
  pthread_mutex_unlock(&registry_lock);
  return 0;
}
extern "C" int shim_pthread_attr_destroy(void *value) {
  pthread_mutex_lock(&registry_lock);
  auto *attribute = thread_attribute_for(value);
  if (attribute)
    thread_attributes.erase(value);
  pthread_mutex_unlock(&registry_lock);
  if (!attribute)
    return EINVAL;
  int result = pthread_attr_destroy(attribute);
  delete attribute;
  return result;
}
extern "C" int shim_pthread_attr_setdetachstate(void *value, int state) {
  pthread_mutex_lock(&registry_lock);
  auto *attribute = thread_attribute_for(value);
  int result = attribute ? pthread_attr_setdetachstate(
                               attribute, state == 2 ? PTHREAD_CREATE_DETACHED
                                                     : PTHREAD_CREATE_JOINABLE)
                         : EINVAL;
  pthread_mutex_unlock(&registry_lock);
  return result;
}
extern "C" int shim_pthread_attr_setstacksize(void *value, size_t size) {
  pthread_mutex_lock(&registry_lock);
  auto *attribute = thread_attribute_for(value);
  int result = attribute ? pthread_attr_setstacksize(attribute, size) : EINVAL;
  pthread_mutex_unlock(&registry_lock);
  return result;
}
extern "C" int shim_pthread_create(pthread_t *thread, void *value,
                                   void *(*start)(void *), void *argument) {
  pthread_mutex_lock(&registry_lock);
  auto *attribute = value ? thread_attribute_for(value) : nullptr;
  int result = pthread_create(thread, attribute, start, argument);
  pthread_mutex_unlock(&registry_lock);
  return result;
}
static pthread_rwlock_t *rwlock_for(void *p) {
  pthread_mutex_lock(&registry_lock);
  auto &r = rwlocks[p];
  if (!r) {
    r = new pthread_rwlock_t;
    pthread_rwlock_init(r, nullptr);
  }
  pthread_mutex_unlock(&registry_lock);
  return r;
}
extern "C" int shim_rw_init(void *p, const void *a) {
  if (a) {
    errno = ENOTSUP;
    return ENOTSUP;
  }
  rwlock_for(p);
  return 0;
}
extern "C" int shim_rw_rdlock(void *p) {
  return pthread_rwlock_rdlock(rwlock_for(p));
}
extern "C" int shim_rw_wrlock(void *p) {
  return pthread_rwlock_wrlock(rwlock_for(p));
}
extern "C" int shim_rw_tryrdlock(void *p) {
  return pthread_rwlock_tryrdlock(rwlock_for(p));
}
extern "C" int shim_rw_trywrlock(void *p) {
  return pthread_rwlock_trywrlock(rwlock_for(p));
}
extern "C" int shim_rw_unlock(void *p) {
  return pthread_rwlock_unlock(rwlock_for(p));
}
extern "C" int shim_rw_destroy(void *p) {
  pthread_mutex_lock(&registry_lock);
  auto it = rwlocks.find(p);
  int r = 0;
  if (it != rwlocks.end()) {
    r = pthread_rwlock_destroy(it->second);
    if (!r) {
      delete it->second;
      rwlocks.erase(it);
    }
  }
  pthread_mutex_unlock(&registry_lock);
  return r;
}
static pthread_mutex_t *mutex_for(void *key) {
  pthread_mutex_lock(&registry_lock);
  auto &m = mutexes[key];
  if (!m) {
    m = new pthread_mutex_t;
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    // Darwin recursive static initialization uses signature 0x32AAABA2.
    if (*(uint32_t *)key == 0x32aaaba2)
      pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &attr);
    pthread_mutexattr_destroy(&attr);
  }
  pthread_mutex_unlock(&registry_lock);
  return m;
}
static pthread_cond_t *condition_for(void *key) {
  pthread_mutex_lock(&registry_lock);
  auto &c = conditions[key];
  if (!c) {
    c = new pthread_cond_t;
    pthread_cond_init(c, nullptr);
  }
  pthread_mutex_unlock(&registry_lock);
  return c;
}
extern "C" int shim_mutex_init(void *p, const void *attr) {
  pthread_mutex_lock(&registry_lock);
  auto &mutex = mutexes[p];
  if (!mutex) {
    mutex = new pthread_mutex_t;
    pthread_mutexattr_t native_attr;
    pthread_mutexattr_init(&native_attr);
    if (attr)
      pthread_mutexattr_settype(&native_attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(mutex, &native_attr);
    pthread_mutexattr_destroy(&native_attr);
  }
  pthread_mutex_unlock(&registry_lock);
  return 0;
}
extern "C" int shim_mutex_lock(void *p) {
  return pthread_mutex_lock(mutex_for(p));
}
extern "C" int shim_mutex_unlock(void *p) {
  return pthread_mutex_unlock(mutex_for(p));
}
extern "C" int shim_mutex_trylock(void *p) {
  return pthread_mutex_trylock(mutex_for(p));
}
extern "C" int shim_mutex_destroy(void *p) {
  pthread_mutex_lock(&registry_lock);
  auto it = mutexes.find(p);
  int r = 0;
  if (it != mutexes.end()) {
    r = pthread_mutex_destroy(it->second);
    if (!r) {
      delete it->second;
      mutexes.erase(it);
    }
  }
  pthread_mutex_unlock(&registry_lock);
  return r;
}
extern "C" int shim_cond_init(void *p, const void *a) {
  if (a) {
    fputs("UNSUPPORTED cond attribute\n", stderr);
    _exit(86);
  }
  condition_for(p);
  return 0;
}
extern "C" int shim_cond_signal(void *p) {
  return pthread_cond_signal(condition_for(p));
}
extern "C" int shim_cond_broadcast(void *p) {
  return pthread_cond_broadcast(condition_for(p));
}
extern "C" int shim_cond_wait(void *p, void *m) {
  return pthread_cond_wait(condition_for(p), mutex_for(m));
}
static int darwin_pthread_result(int result) {
  // pthread functions return errno values directly. Darwin's ETIMEDOUT is 60,
  // while Linux uses 110; Abseil checks the numeric Darwin value.
  return result == ETIMEDOUT ? 60 : result;
}
extern "C" int shim_cond_timedwait(void *p, void *m, const timespec *t) {
  return darwin_pthread_result(
      pthread_cond_timedwait(condition_for(p), mutex_for(m), t));
}
extern "C" int shim_cond_relative(void *p, void *m, const timespec *t) {
  timespec abs;
  clock_gettime(CLOCK_REALTIME, &abs);
  abs.tv_sec += t->tv_sec;
  abs.tv_nsec += t->tv_nsec;
  if (abs.tv_nsec >= 1000000000) {
    abs.tv_nsec -= 1000000000;
    ++abs.tv_sec;
  }
  return shim_cond_timedwait(p, m, &abs);
}
extern "C" int shim_cond_destroy(void *p) {
  pthread_mutex_lock(&registry_lock);
  auto it = conditions.find(p);
  int r = 0;
  if (it != conditions.end()) {
    r = pthread_cond_destroy(it->second);
    if (!r) {
      delete it->second;
      conditions.erase(it);
    }
  }
  pthread_mutex_unlock(&registry_lock);
  return r;
}
extern "C" void shim_cpp_cv_wait(void *p, void *lock) {
  int r = shim_cond_wait(p, *(void **)lock);
  if (r)
    _exit(86);
}
extern "C" void shim_cpp_cv_timed_wait(void *p, void *lock,
                                       int64_t nanoseconds_since_epoch) {
  timespec deadline{};
  deadline.tv_sec = nanoseconds_since_epoch / 1000000000LL;
  deadline.tv_nsec = nanoseconds_since_epoch % 1000000000LL;
  int result = shim_cond_timedwait(p, *(void **)lock, &deadline);
  if (result && result != 60)
    _exit(86);
}
extern "C" bool shim_cpp_mutex_try_lock(void *p) {
  return shim_mutex_trylock(p) == 0;
}
extern "C" void shim_promise_void_get_future(void *output, void *promise) {
  // Flurry constructs libc++'s __assoc_sub_state inline with Darwin's static
  // pthread signatures, then calls the dynamically imported get_future().
  // Linux libc++ operates on the embedded native pthread objects directly.
  // Convert those two fields before crossing that ABI boundary.
  auto *state = promise ? *static_cast<void **>(promise) : nullptr;
  if (state) {
    auto *bytes = static_cast<unsigned char *>(state);
    auto *mutex = reinterpret_cast<pthread_mutex_t *>(bytes + 0x18);
    auto *condition = reinterpret_cast<pthread_cond_t *>(bytes + 0x58);
    if (*reinterpret_cast<uint64_t *>(mutex) == 0x32aaaba7ULL) {
      memset(mutex, 0, sizeof(*mutex));
      pthread_mutex_init(mutex, nullptr);
    }
    if (*reinterpret_cast<uint64_t *>(condition) == 0x3cb0b1bbULL) {
      memset(condition, 0, sizeof(*condition));
      pthread_cond_init(condition, nullptr);
    }
  }
  using Original = void (*)(void *, void *);
  static Original original = [] {
    void *library = dlopen("libc++.so.1", RTLD_NOW | RTLD_NOLOAD);
    return reinterpret_cast<Original>(
        dlsym(library, "_ZNSt3__17promiseIvE10get_futureEv"));
  }();
  if (!original)
    _exit(86);
  original(output, promise);
}
extern "C" int shim_threadid(pthread_t t, uint64_t *out) {
  if (t && !pthread_equal(t, pthread_self()))
    return ENOTSUP;
  *out = syscall(SYS_gettid);
  return 0;
}
extern "C" int *shim_error() { return &errno; }
extern "C" int shim_toupper(int c) {
  return c >= -1 && c <= 255 ? toupper(c) : towupper(c);
}
extern "C" int shim_tolower(int c) {
  return c >= -1 && c <= 255 ? tolower(c) : towlower(c);
}
extern "C" void shim_pattern16(void *d, const void *p, size_t n) {
  unsigned char pattern[16];
  memcpy(pattern, p, 16);
  auto q = (unsigned char *)d;
  while (n) {
    size_t part = std::min(n, size_t(16));
    memcpy(q, pattern, part);
    q += part;
    n -= part;
  }
}
static int timezone_object;
static std::string timezone_name;
extern "C" void *shim_timezone_default() {
  const char *tz = getenv("TZ");
  timezone_name = (tz && *tz) ? tz : "Etc/UTC";
  return &timezone_object;
}
extern "C" void *shim_timezone_name(void *p) {
  if (p != &timezone_object)
    _exit(86);
  return &timezone_name;
}
extern "C" const char *shim_cf_string_ptr(void *p, uint32_t enc) {
  if (p != &timezone_name)
    _exit(86);
  if (enc != 0x08000100 && enc != 0x600)
    return nullptr;
  return timezone_name.c_str();
}
extern "C" bool shim_cf_string_copy(void *p, char *out, long cap,
                                    uint32_t enc) {
  auto s = shim_cf_string_ptr(p, enc);
  if (!s || cap <= (long)strlen(s))
    return false;
  strcpy(out, s);
  return true;
}
extern "C" long shim_cf_string_length(void *p) {
  if (p != &timezone_name)
    _exit(86);
  return timezone_name.size();
}
extern "C" long shim_cf_string_max(long n, uint32_t enc) {
  if (n < 0)
    return -1;
  if (enc == 0x08000100)
    return n <= INT64_MAX / 3 ? n * 3 : -1;
  if (enc == 0x600)
    return n;
  return -1;
}
extern "C" void shim_cf_release(void *p) {
  if (p != &timezone_object && p != &timezone_name) {
    fprintf(stderr, "UNSUPPORTED CFRelease object\n");
    _exit(86);
  }
}
extern "C" void *shim_cf_retain(void *p) {
  shim_cf_release(p);
  return p;
}
extern "C" double shim_cf_time() {
  timespec t;
  clock_gettime(CLOCK_REALTIME, &t);
  return t.tv_sec - 978307200.0 + t.tv_nsec / 1e9;
}
struct DarwinStat {
  int32_t dev;
  uint16_t mode, nlink;
  uint64_t ino;
  uint32_t uid, gid;
  int32_t rdev;
  uint32_t pad;
  timespec atime, mtime, ctime, birthtime;
  int64_t size, blocks;
  int32_t blksize;
  uint32_t flags, gen;
  int32_t spare;
  int64_t qspare[2];
};
static_assert(sizeof(DarwinStat) == 144);
static void convert_stat(const struct stat &s, DarwinStat *d) {
  memset(d, 0, sizeof(*d));
  d->dev = s.st_dev;
  d->mode = s.st_mode;
  d->nlink = s.st_nlink;
  d->ino = s.st_ino;
  d->uid = s.st_uid;
  d->gid = s.st_gid;
  d->rdev = s.st_rdev;
  d->atime = s.st_atim;
  d->mtime = s.st_mtim;
  d->ctime = s.st_ctim;
  d->size = s.st_size;
  d->blocks = s.st_blocks;
  d->blksize = s.st_blksize;
}
extern "C" int shim_fstat(int fd, DarwinStat *d) {
  struct stat s;
  int r = fstat(fd, &s);
  if (!r)
    convert_stat(s, d);
  return r;
}
extern "C" int shim_stat(const char *p, DarwinStat *d) {
  struct stat s;
  int r = stat(p, &s);
  if (getenv("WETYPE_HOST_DEBUG"))
    fprintf(stderr, "DARWIN_STAT path=%s result=%d errno=%d\n", p, r, errno);
  if (!r)
    convert_stat(s, d);
  return r;
}
extern "C" int shim_lstat(const char *p, DarwinStat *d) {
  struct stat s;
  int r = lstat(p, &s);
  if (!r)
    convert_stat(s, d);
  return r;
}
struct DarwinStatFs {
  uint32_t block_size;
  int32_t io_size;
  uint64_t blocks, free_blocks, available_blocks, files, free_files;
  int32_t fsid[2];
  uint32_t owner, type, flags, subtype;
  char type_name[16];
  char mount_on[1024];
  char mount_from[1024];
  uint32_t extended_flags;
  uint32_t reserved[7];
};
static_assert(sizeof(DarwinStatFs) == 2168);
static void convert_statfs(const struct statvfs &source, DarwinStatFs *target) {
  memset(target, 0, sizeof(*target));
  target->block_size = source.f_bsize;
  target->io_size = source.f_frsize;
  target->blocks = source.f_blocks;
  target->free_blocks = source.f_bfree;
  target->available_blocks = source.f_bavail;
  target->files = source.f_files;
  target->free_files = source.f_ffree;
  target->flags = source.f_flag;
  memcpy(target->type_name, "linux", 6);
}
extern "C" int shim_fstatfs(int fd, DarwinStatFs *target) {
  struct statvfs source;
  int result = fstatvfs(fd, &source);
  if (!result)
    convert_statfs(source, target);
  return result;
}
extern "C" int shim_statfs(const char *path, DarwinStatFs *target) {
  struct statvfs source;
  int result = statvfs(path, &source);
  if (!result)
    convert_statfs(source, target);
  return result;
}
static int open_flags(int f) {
  int n = f & 3;
  const std::pair<int, int> m[] = {
      {4, O_NONBLOCK},       {8, O_APPEND},       {0x80, O_SYNC},
      {0x100, O_NOFOLLOW},   {0x200, O_CREAT},    {0x400, O_TRUNC},
      {0x800, O_EXCL},       {0x20000, O_NOCTTY}, {0x100000, O_DIRECTORY},
      {0x1000000, O_CLOEXEC}};
  int known = 3;
  for (auto [a, b] : m) {
    known |= a;
    if (f & a)
      n |= b;
  }
  if (f & ~known) {
    fprintf(stderr, "UNSUPPORTED open flags %x\n", f);
    errno = EINVAL;
    return -1;
  }
  return n;
}
extern "C" int shim_open(const char *p, int f, ...) {
  mode_t mode = 0;
  if (f & 0x200) {
    va_list a;
    va_start(a, f);
    mode = va_arg(a, int);
    va_end(a);
  }
  int flags = open_flags(f);
  if (flags < 0)
    return -1;
  int result = open(p, flags, mode);
  if (getenv("WETYPE_HOST_DEBUG"))
    fprintf(stderr,
            "DARWIN_OPEN path=%s flags=%x translated=%x mode=%o result=%d "
            "errno=%d\n",
            p, f, flags, unsigned(mode), result, errno);
  return result;
}
struct DarwinDirent {
  uint64_t ino, seek;
  uint16_t reclen, namlen;
  uint8_t type;
  char name[1024];
};
extern "C" DarwinDirent *shim_readdir(DIR *d) {
  auto e = readdir(d);
  if (!e)
    return nullptr;
  static thread_local DarwinDirent out{};
  out.ino = e->d_ino;
  out.seek = e->d_off;
  out.namlen = strlen(e->d_name);
  out.type = e->d_type;
  memcpy(out.name, e->d_name, out.namlen + 1);
  out.reclen = (21 + out.namlen + 1 + 3) & ~3;
  return &out;
}
struct DarwinFlock {
  int64_t start, len;
  int32_t pid;
  int16_t type, whence;
};
extern "C" int shim_fcntl(int fd, int cmd, ...) {
  va_list ap;
  va_start(ap, cmd);
  long arg = 0;
  if (cmd != 1 && cmd != 3)
    arg = va_arg(ap, long);
  va_end(ap);
  if (getenv("WETYPE_HOST_DEBUG"))
    fprintf(stderr, "DARWIN_FCNTL fd=%d command=%d argument=%ld\n", fd, cmd,
            arg);
  if (cmd == 7 || cmd == 8 || cmd == 9) {
    auto d = (DarwinFlock *)arg;
    struct flock f{};
    f.l_start = d->start;
    f.l_len = d->len;
    f.l_pid = d->pid;
    f.l_type = d->type == 1 ? F_RDLCK : d->type == 2 ? F_UNLCK : F_WRLCK;
    f.l_whence = d->whence;
    int r = fcntl(fd, cmd == 7 ? F_GETLK : cmd == 8 ? F_SETLK : F_SETLKW, &f);
    if (!r && cmd == 7) {
      d->type = f.l_type == F_RDLCK ? 1 : f.l_type == F_UNLCK ? 2 : 3;
      d->pid = f.l_pid;
      d->start = f.l_start;
      d->len = f.l_len;
    }
    return r;
  }
  if (cmd == 50) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(path, (char *)arg, 1023);
    if (n < 0)
      return -1;
    ((char *)arg)[n] = 0;
    return 0;
  }
  if (cmd == 51)
    return fsync(fd);
  if (cmd == 67)
    return fcntl(fd, F_DUPFD_CLOEXEC, arg);
  if (cmd == 0 || cmd == 1 || cmd == 2)
    return fcntl(fd, cmd, arg);
  if (cmd == 4) {
    int flags = open_flags(arg);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags);
  }
  if (cmd == 3) {
    int f = fcntl(fd, F_GETFL);
    if (f < 0)
      return f;
    return (f & 3) | ((f & O_NONBLOCK) ? 4 : 0) | ((f & O_APPEND) ? 8 : 0);
  }
  fprintf(stderr, "UNSUPPORTED fcntl %d\n", cmd);
  errno = EINVAL;
  return -1;
}
extern "C" void *shim_mmap(void *p, size_t n, int prot, int flags, int fd,
                           off_t offset) {
  int native = flags & 0x13; // SHARED, PRIVATE, FIXED are equal.
  if (flags & 0x1000)
    native |= MAP_ANONYMOUS;
  if (flags & 0x40)
    native |= MAP_NORESERVE;
  if (flags & ~0x1053) {
    fprintf(stderr, "UNSUPPORTED mmap flags=%x\n", flags);
    errno = EINVAL;
    return MAP_FAILED;
  }
  return mmap(p, n, prot, native, fd, offset);
}
extern "C" void *shim_dlsym(void *h, const char *n) {
  if (h == (void *)-1 || h == (void *)-2 || h == (void *)-3 || h == (void *)-5)
    h = RTLD_DEFAULT;
  if (!strcmp(n, "__cxa_throw")) {
    static void *abi = dlopen("libc++abi.so.1", RTLD_NOW);
    return dlsym(abi, n);
  }
  return dlsym(h, n);
}
extern "C" void shim_bzero(void *p, size_t n) { memset(p, 0, n); }
extern "C" int shim_atexit(void (*function)()) { return atexit(function); }
static void *sec_random_default = nullptr;
extern "C" int shim_sec_random_copy_bytes(void *, size_t size,
                                          unsigned char *output) {
  size_t offset = 0;
  while (offset < size) {
    ssize_t count = getrandom(output + offset, size - offset, 0);
    if (count < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    offset += static_cast<size_t>(count);
  }
  return 0;
}
extern "C" void *shim_malloc_zone_malloc(void *, size_t size) {
  return malloc(size);
}
extern "C" void *shim_malloc_zone_calloc(void *, size_t count, size_t size) {
  return calloc(count, size);
}
extern "C" void *shim_malloc_zone_valloc(void *, size_t size) {
  void *result = nullptr;
  return posix_memalign(&result, 4096, size) ? nullptr : result;
}
extern "C" void shim_malloc_zone_free(void *, void *pointer) { free(pointer); }
extern "C" void *shim_malloc_zone_realloc(void *, void *pointer, size_t size) {
  return realloc(pointer, size);
}
extern "C" size_t shim_malloc_size(const void *pointer) {
  return pointer ? malloc_usable_size(const_cast<void *>(pointer)) : 0;
}
extern "C" size_t shim_malloc_zone_size(void *, const void *pointer) {
  return shim_malloc_size(pointer);
}
extern "C" void shim_malloc_zone_destroy(void *) {}
struct DarwinMallocZone {
  void *reserved1;
  void *reserved2;
  size_t (*size)(void *, const void *);
  void *(*malloc_fn)(void *, size_t);
  void *(*calloc_fn)(void *, size_t, size_t);
  void *(*valloc_fn)(void *, size_t);
  void (*free_fn)(void *, void *);
  void *(*realloc_fn)(void *, void *, size_t);
  void (*destroy_fn)(void *);
  const char *zone_name;
};
static DarwinMallocZone default_malloc_zone{nullptr,
                                            nullptr,
                                            shim_malloc_zone_size,
                                            shim_malloc_zone_malloc,
                                            shim_malloc_zone_calloc,
                                            shim_malloc_zone_valloc,
                                            shim_malloc_zone_free,
                                            shim_malloc_zone_realloc,
                                            shim_malloc_zone_destroy,
                                            "fcitx5-wetypex"};
extern "C" void *shim_malloc_default_zone() { return &default_malloc_zone; }
extern "C" void *shim_malloc_create_zone(size_t, unsigned) {
  return &default_malloc_zone;
}
extern "C" void shim_malloc_set_zone_name(void *, const char *) {}
extern "C" void *shim_reallocf(void *pointer, size_t size) {
  void *result = realloc(pointer, size);
  if (!result && size)
    free(pointer);
  return result;
}
static int sysctl_copy(const void *value, size_t size, void *old_value,
                       size_t *old_size) {
  if (!old_size) {
    errno = EINVAL;
    return -1;
  }
  if (!old_value) {
    *old_size = size;
    return 0;
  }
  if (*old_size < size) {
    *old_size = size;
    errno = ENOMEM;
    return -1;
  }
  memcpy(old_value, value, size);
  *old_size = size;
  return 0;
}
extern "C" int shim_sysctlbyname(const char *name, void *old_value,
                                 size_t *old_size, const void *, size_t) {
  if (!name) {
    errno = EINVAL;
    return -1;
  }
  if (!strcmp(name, "hw.ncpu") || !strcmp(name, "hw.logicalcpu") ||
      !strcmp(name, "hw.logicalcpu_max") || !strcmp(name, "hw.physicalcpu") ||
      !strcmp(name, "hw.physicalcpu_max")) {
    int count = static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
    if (count < 1)
      count = 1;
    return sysctl_copy(&count, sizeof(count), old_value, old_size);
  }
  if (!strcmp(name, "hw.memsize")) {
    uint64_t memory = static_cast<uint64_t>(sysconf(_SC_PHYS_PAGES)) *
                      static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
    return sysctl_copy(&memory, sizeof(memory), old_value, old_size);
  }
  const char *text = nullptr;
  if (!strcmp(name, "hw.model")) {
    text = getenv("WETYPE_DEVICE_MODEL");
    if (!text || !*text)
      text = "LINUX";
  } else if (!strcmp(name, "hw.machine")) {
    text = "x86_64";
  } else if (!strcmp(name, "kern.osrelease")) {
    text = "24.2.0";
  } else if (!strcmp(name, "kern.osproductversion")) {
    text = "15.2";
  } else if (!strcmp(name, "machdep.cpu.brand_string")) {
    text = "Linux x86_64";
  }
  if (text)
    return sysctl_copy(text, strlen(text) + 1, old_value, old_size);
  fprintf(stderr, "UNSUPPORTED sysctlbyname %s\n", name);
  errno = ENOENT;
  return -1;
}
extern "C" int shim_sysctl(const int *name, unsigned name_length,
                           void *, size_t *old_size, const void *, size_t) {
  // WXP2P uses CTL_NET/PF_ROUTE queries only to discover the system's default
  // IPv4/IPv6 gateway. Linux exposes that information through netlink rather
  // than Darwin routing sysctl records. Reporting the optional route table as
  // unavailable makes the original library skip gateway-assisted direct
  // probing and continue with its fully supported relay path.
  if (getenv("WETYPE_HOST_DEBUG")) {
    fputs("DARWIN_SYSCTL mib=", stderr);
    for (unsigned i = 0; i < name_length; ++i)
      fprintf(stderr, "%s%d", i ? "," : "", name ? name[i] : -1);
    fputc('\n', stderr);
  }
  if (old_size)
    *old_size = 0;
  errno = ENOENT;
  return -1;
}
extern "C" __attribute__((used, noinline)) void *shim_tlv_impl(void *d) {
  struct Desc {
    void *p;
    uintptr_t key, off;
  };
  auto desc = (Desc *)d;
  static thread_local void *block = nullptr;
  if (!block) {
    block = calloc(1, tls_total);
    memcpy(block, (void *)tls_data, tls_size);
  }
  if (desc->off >= tls_total) {
    fputs("BAD TLV offset\n", stderr);
    _exit(86);
  }
  return (char *)block + desc->off;
}
extern "C" void shim_tlv();
extern "C" void shim_chkstk();
asm(".text\n.global shim_chkstk\nshim_chkstk:\npush %rcx\npush %rax\nlea "
    "24(%rsp),%rcx\n"
    "cmp $4096,%rax\njb 2f\n1: sub $4096,%rcx\ntestb $0,(%rcx)\nsub "
    "$4096,%rax\ncmp $4096,%rax\njae 1b\n"
    "2: sub %rax,%rcx\ntestb $0,(%rcx)\npop %rax\npop %rcx\nret\n");
asm(".text\n.global shim_tlv\nshim_tlv:\n"
    "push %rdi\npush %rsi\npush %rdx\npush %rcx\npush %r8\npush %r9\npush "
    "%r10\npush %r11\n"
    "sub $264,%rsp\nmovdqu %xmm0,0(%rsp)\nmovdqu %xmm1,16(%rsp)\nmovdqu "
    "%xmm2,32(%rsp)\nmovdqu %xmm3,48(%rsp)\n"
    "movdqu %xmm4,64(%rsp)\nmovdqu %xmm5,80(%rsp)\nmovdqu "
    "%xmm6,96(%rsp)\nmovdqu %xmm7,112(%rsp)\n"
    "movdqu %xmm8,128(%rsp)\nmovdqu %xmm9,144(%rsp)\nmovdqu "
    "%xmm10,160(%rsp)\nmovdqu %xmm11,176(%rsp)\n"
    "movdqu %xmm12,192(%rsp)\nmovdqu %xmm13,208(%rsp)\nmovdqu "
    "%xmm14,224(%rsp)\nmovdqu %xmm15,240(%rsp)\ncall shim_tlv_impl\n"
    "movdqu 0(%rsp),%xmm0\nmovdqu 16(%rsp),%xmm1\nmovdqu "
    "32(%rsp),%xmm2\nmovdqu 48(%rsp),%xmm3\n"
    "movdqu 64(%rsp),%xmm4\nmovdqu 80(%rsp),%xmm5\nmovdqu "
    "96(%rsp),%xmm6\nmovdqu 112(%rsp),%xmm7\n"
    "movdqu 128(%rsp),%xmm8\nmovdqu 144(%rsp),%xmm9\nmovdqu "
    "160(%rsp),%xmm10\nmovdqu 176(%rsp),%xmm11\n"
    "movdqu 192(%rsp),%xmm12\nmovdqu 208(%rsp),%xmm13\nmovdqu "
    "224(%rsp),%xmm14\nmovdqu 240(%rsp),%xmm15\n"
    "add $264,%rsp\npop %r11\npop %r10\npop %r9\npop %r8\npop %rcx\npop "
    "%rdx\npop %rsi\npop %rdi\nret\n");
extern "C" int __cxa_thread_atexit_impl(void (*)(void *), void *, void *);
extern "C" int shim_tlv_atexit(void (*f)(void *), void *p) {
  return __cxa_thread_atexit_impl(f, p, (void *)&stack_guard);
}
static void show_address(uintptr_t pc) {
  auto i =
      std::upper_bound(address_names.begin(), address_names.end(), pc,
                       [](uintptr_t v, const auto &r) { return v < r.first; });
  if (i != address_names.begin()) {
    --i;
    fprintf(stderr, "0x%lx %s +0x%lx\n", pc, i->second.c_str(), pc - i->first);
  } else
    fprintf(stderr, "0x%lx\n", pc);
}
static void crash(int sig, siginfo_t *info, void *ctx) {
  auto uc = (ucontext_t *)ctx;
  fprintf(stderr, "FAULT signal=%d address=%p rip=", sig, info->si_addr);
  show_address(uc->uc_mcontext.gregs[REG_RIP]);
  fprintf(stderr, "REG rdi=%llx rsi=%llx rdx=%llx rax=%llx rbp=%llx rsp=%llx\n",
          (long long)uc->uc_mcontext.gregs[REG_RDI],
          (long long)uc->uc_mcontext.gregs[REG_RSI],
          (long long)uc->uc_mcontext.gregs[REG_RDX],
          (long long)uc->uc_mcontext.gregs[REG_RAX],
          (long long)uc->uc_mcontext.gregs[REG_RBP],
          (long long)uc->uc_mcontext.gregs[REG_RSP]);
  auto sp = (uintptr_t *)uc->uc_mcontext.gregs[REG_RSP];
  for (int i = 0; i < 12; ++i) {
    if (sp[i] >= 0x100000000 && sp[i] < 0x104000000)
      show_address(sp[i]);
  }
  _exit(128 + sig);
}
extern "C" void missing_symbol(const char *n) {
  fprintf(stderr, "UNIMPLEMENTED %s\n", n);
  _exit(85);
}
static unsigned char *trap_arena = nullptr;
static size_t trap_offset = 0;
static void *trap(const std::string &n) {
  if (!trap_arena) {
    trap_arena = (unsigned char *)mmap(nullptr, 1048576, PROT_READ | PROT_WRITE,
                                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (trap_arena == MAP_FAILED)
      _exit(92);
  }
  if (trap_offset + 32 > 1048576)
    _exit(92);
  auto code = trap_arena + trap_offset;
  trap_offset += 32;
  char *name = strdup(n.c_str());
  code[0] = 0x48;
  code[1] = 0xbf;
  memcpy(code + 2, &name, 8);
  void *fn = (void *)&missing_symbol;
  code[10] = 0x48;
  code[11] = 0xb8;
  memcpy(code + 12, &fn, 8);
  code[20] = 0xff;
  code[21] = 0xe0;
  return code;
}
struct DispatchSemaphore {
  sem_t value;
};
static pthread_mutex_t dispatch_semaphore_lock = PTHREAD_MUTEX_INITIALIZER;
static std::set<void *> dispatch_semaphores;
static pthread_mutex_t once_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t once_condition = PTHREAD_COND_INITIALIZER;
static std::map<void *, int> once_states;
static int shim_pthread_once(void *token, void (*initializer)()) {
  pthread_mutex_lock(&once_lock);
  auto [entry, inserted] = once_states.emplace(token, 1);
  if (!inserted) {
    while (entry->second == 1)
      pthread_cond_wait(&once_condition, &once_lock);
    pthread_mutex_unlock(&once_lock);
    return 0;
  }
  pthread_mutex_unlock(&once_lock);
  initializer();
  pthread_mutex_lock(&once_lock);
  once_states[token] = 2;
  pthread_cond_broadcast(&once_condition);
  pthread_mutex_unlock(&once_lock);
  return 0;
}
static void *shim_dispatch_semaphore_create(long value) {
  auto *semaphore = new DispatchSemaphore;
  if (sem_init(&semaphore->value, 0, std::max(0L, value))) {
    delete semaphore;
    return nullptr;
  }
  pthread_mutex_lock(&dispatch_semaphore_lock);
  dispatch_semaphores.insert(semaphore);
  pthread_mutex_unlock(&dispatch_semaphore_lock);
  return semaphore;
}
static long shim_dispatch_semaphore_signal(void *value) {
  return sem_post(&static_cast<DispatchSemaphore *>(value)->value);
}
static long shim_dispatch_semaphore_wait(void *value, uint64_t timeout) {
  auto *semaphore = static_cast<DispatchSemaphore *>(value);
  int result = 0;
  if (timeout == UINT64_MAX) {
    do {
      result = sem_wait(&semaphore->value);
    } while (result && errno == EINTR);
  } else {
    timespec deadline{};
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout / 1000000000ULL;
    deadline.tv_nsec += timeout % 1000000000ULL;
    if (deadline.tv_nsec >= 1000000000L) {
      ++deadline.tv_sec;
      deadline.tv_nsec -= 1000000000L;
    }
    do {
      result = sem_timedwait(&semaphore->value, &deadline);
    } while (result && errno == EINTR);
  }
  return result == 0 ? 0 : 1;
}
static void shim_dispatch_release(void *value) {
  // libdispatch uses dispatch_release for queues, groups and semaphores.  Only
  // semaphores are represented by a Linux object in this host; the other
  // objects are owned by the original module.  Treating every value as our
  // DispatchSemaphore corrupts its allocator during gRPC credential cleanup.
  pthread_mutex_lock(&dispatch_semaphore_lock);
  auto found = dispatch_semaphores.find(value);
  if (found == dispatch_semaphores.end()) {
    pthread_mutex_unlock(&dispatch_semaphore_lock);
    return;
  }
  dispatch_semaphores.erase(found);
  pthread_mutex_unlock(&dispatch_semaphore_lock);
  auto *semaphore = static_cast<DispatchSemaphore *>(value);
  sem_destroy(&semaphore->value);
  delete semaphore;
}
static int shim_connectx() {
  errno = ENOTSUP;
  return -1;
}
static int darwin_errno(int value) {
  switch (value) {
  case EAGAIN:
    return 35;
  case EINPROGRESS:
    return 36;
  case EALREADY:
    return 37;
  case ENOTSOCK:
    return 38;
  case EDESTADDRREQ:
    return 39;
  case EMSGSIZE:
    return 40;
  case EPROTOTYPE:
    return 41;
  case ENOPROTOOPT:
    return 42;
  case EPROTONOSUPPORT:
    return 43;
  case ESOCKTNOSUPPORT:
    return 44;
  case ENOTSUP:
    return 45;
  case EAFNOSUPPORT:
    return 47;
  case EADDRINUSE:
    return 48;
  case EADDRNOTAVAIL:
    return 49;
  case ENETDOWN:
    return 50;
  case ENETUNREACH:
    return 51;
  case ENETRESET:
    return 52;
  case ECONNABORTED:
    return 53;
  case ECONNRESET:
    return 54;
  case ENOBUFS:
    return 55;
  case EISCONN:
    return 56;
  case ENOTCONN:
    return 57;
  case ESHUTDOWN:
    return 58;
  case ETIMEDOUT:
    return 60;
  case ECONNREFUSED:
    return 61;
  case ELOOP:
    return 62;
  case ENAMETOOLONG:
    return 63;
  case EHOSTUNREACH:
    return 65;
  case ENOTEMPTY:
    return 66;
  default:
    return value;
  }
}
template <typename T> static T translate_errno_result(T result) {
  if (result < 0)
    errno = darwin_errno(errno);
  return result;
}
static ssize_t shim_read(int fd, void *buffer, size_t size) {
  return translate_errno_result(read(fd, buffer, size));
}
static ssize_t shim_write(int fd, const void *buffer, size_t size) {
  return translate_errno_result(write(fd, buffer, size));
}
static ssize_t shim_writev(int fd, const iovec *vectors, int count) {
  return translate_errno_result(writev(fd, vectors, count));
}
static int shim_poll(pollfd *fds, nfds_t count, int timeout) {
  return translate_errno_result(poll(fds, count, timeout));
}
static int linux_socket_domain(int domain) {
  return domain == 30 ? AF_INET6 : domain;
}
static socklen_t linux_sockaddr(const void *source, socklen_t source_size,
                                sockaddr_storage &destination) {
  memset(&destination, 0, sizeof(destination));
  if (!source || source_size < 2)
    return 0;
  const auto *bytes = static_cast<const unsigned char *>(source);
  const unsigned family = bytes[1];
  socklen_t size = std::min<socklen_t>(bytes[0] ? bytes[0] : source_size,
                                       sizeof(destination));
  memcpy(&destination, source, size);
  reinterpret_cast<sockaddr *>(&destination)->sa_family =
      linux_socket_domain(family);
  return size;
}
static void darwin_sockaddr(const sockaddr_storage &source,
                            socklen_t native_size, void *destination,
                            socklen_t *destination_size) {
  if (!destination_size)
    return;
  socklen_t size = std::min(*destination_size, native_size);
  if (destination && size) {
    memcpy(destination, &source, size);
    auto *bytes = static_cast<unsigned char *>(destination);
    bytes[0] = static_cast<unsigned char>(native_size);
    bytes[1] = source.ss_family == AF_INET6 ? 30 : source.ss_family;
  }
  *destination_size = native_size;
}
static int shim_socket(int domain, int type, int protocol) {
  int result = translate_errno_result(
      socket(linux_socket_domain(domain), type, protocol));
  if (getenv("WETYPE_HOST_DEBUG"))
    fprintf(stderr,
            "DARWIN_SOCKET domain=%d type=%d protocol=%d result=%d "
            "errno=%d\n",
            domain, type, protocol, result, errno);
  return result;
}
static const char *shim_inet_ntop(int family, const void *source,
                                  char *destination, socklen_t size) {
  return inet_ntop(linux_socket_domain(family), source, destination, size);
}
static int shim_inet_pton(int family, const char *source, void *destination) {
  return inet_pton(linux_socket_domain(family), source, destination);
}
struct DarwinAddrInfo {
  int flags;
  int family;
  int socket_type;
  int protocol;
  socklen_t address_length;
  char *canonical_name;
  void *address;
  DarwinAddrInfo *next;
};
static_assert(sizeof(DarwinAddrInfo) == 48);
static int shim_getaddrinfo(const char *node, const char *service,
                            const DarwinAddrInfo *hints,
                            DarwinAddrInfo **result) {
  if (!result)
    return EAI_FAIL;
  addrinfo native_hints{};
  const addrinfo *native_hints_pointer = nullptr;
  if (hints) {
    native_hints.ai_flags = hints->flags;
    native_hints.ai_family = linux_socket_domain(hints->family);
    native_hints.ai_socktype = hints->socket_type;
    native_hints.ai_protocol = hints->protocol;
    native_hints_pointer = &native_hints;
  }
  addrinfo *native = nullptr;
  int status = getaddrinfo(node, service, native_hints_pointer, &native);
  if (status)
    return status;
  DarwinAddrInfo *head = nullptr, **tail = &head;
  for (auto *item = native; item; item = item->ai_next) {
    auto *copy = new DarwinAddrInfo{};
    copy->flags = item->ai_flags;
    copy->family = item->ai_family == AF_INET6 ? 30 : item->ai_family;
    copy->socket_type = item->ai_socktype;
    copy->protocol = item->ai_protocol;
    copy->address_length = item->ai_addrlen;
    if (item->ai_canonname)
      copy->canonical_name = strdup(item->ai_canonname);
    copy->address = calloc(1, item->ai_addrlen);
    if (copy->address) {
      memcpy(copy->address, item->ai_addr, item->ai_addrlen);
      auto *bytes = static_cast<unsigned char *>(copy->address);
      bytes[0] = static_cast<unsigned char>(item->ai_addrlen);
      bytes[1] = static_cast<unsigned char>(copy->family);
    }
    *tail = copy;
    tail = &copy->next;
  }
  freeaddrinfo(native);
  *result = head;
  if (getenv("WETYPE_HOST_DEBUG"))
    fprintf(stderr, "DARWIN_GETADDRINFO node=%s service=%s result=%p\n",
            node ? node : "", service ? service : "", (void *)head);
  return 0;
}
static void shim_freeaddrinfo(DarwinAddrInfo *value) {
  while (value) {
    auto *next = value->next;
    free(value->canonical_name);
    free(value->address);
    delete value;
    value = next;
  }
}
static int shim_bind(int socket_fd, const void *address, socklen_t size) {
  sockaddr_storage translated{};
  socklen_t translated_size = linux_sockaddr(address, size, translated);
  if (!translated_size) {
    errno = EINVAL;
    return -1;
  }
  int result = translate_errno_result(bind(
      socket_fd, reinterpret_cast<sockaddr *>(&translated), translated_size));
  if (getenv("WETYPE_HOST_DEBUG"))
    fprintf(stderr, "DARWIN_BIND fd=%d family=%d size=%u result=%d errno=%d\n",
            socket_fd, int(translated.ss_family), unsigned(translated_size),
            result, errno);
  return result;
}
static int shim_connect(int socket_fd, const void *address, socklen_t size) {
  sockaddr_storage translated{};
  socklen_t translated_size = linux_sockaddr(address, size, translated);
  if (!translated_size) {
    errno = EINVAL;
    return -1;
  }
  return translate_errno_result(connect(
      socket_fd, reinterpret_cast<sockaddr *>(&translated), translated_size));
}
static int shim_getsockname(int socket_fd, void *address, socklen_t *size) {
  sockaddr_storage native{};
  socklen_t native_size = sizeof(native);
  int result = translate_errno_result(getsockname(
      socket_fd, reinterpret_cast<sockaddr *>(&native), &native_size));
  if (!result)
    darwin_sockaddr(native, native_size, address, size);
  if (getenv("WETYPE_HOST_DEBUG"))
    fprintf(stderr,
            "DARWIN_GETSOCKNAME fd=%d family=%d size=%u result=%d errno=%d\n",
            socket_fd, int(native.ss_family), unsigned(native_size), result,
            errno);
  return result;
}
static int shim_listen(int socket_fd, int backlog) {
  int result = translate_errno_result(listen(socket_fd, backlog));
  if (getenv("WETYPE_HOST_DEBUG"))
    fprintf(stderr, "DARWIN_LISTEN fd=%d backlog=%d result=%d errno=%d\n",
            socket_fd, backlog, result, errno);
  return result;
}
static pthread_mutex_t socket_state_lock = PTHREAD_MUTEX_INITIALIZER;
static std::set<int> no_sigpipe_sockets;
static int shim_accept(int socket_fd, void *address, socklen_t *size) {
  sockaddr_storage native{};
  socklen_t native_size = sizeof(native);
  int result = translate_errno_result(
      accept(socket_fd, reinterpret_cast<sockaddr *>(&native),
             address ? &native_size : nullptr));
  if (result >= 0 && address)
    darwin_sockaddr(native, native_size, address, size);
  return result;
}
struct DarwinMessageHeader {
  void *name;
  socklen_t name_length;
  uint32_t name_padding;
  iovec *vectors;
  int vector_count;
  uint32_t vector_padding;
  void *control;
  socklen_t control_length;
  int flags;
};
static_assert(sizeof(DarwinMessageHeader) == 48);
static int linux_message_flags(int flags) {
  int translated = flags & ~(0x80 | 0x80000);
  if (flags & 0x80) // Darwin MSG_DONTWAIT
    translated |= MSG_DONTWAIT;
#ifdef MSG_CMSG_CLOEXEC
  if (flags & 0x80000)
    translated |= MSG_CMSG_CLOEXEC;
#endif
  return translated;
}
static bool socket_uses_no_sigpipe(int socket_fd) {
  pthread_mutex_lock(&socket_state_lock);
  bool enabled = no_sigpipe_sockets.count(socket_fd);
  pthread_mutex_unlock(&socket_state_lock);
  return enabled;
}
static ssize_t shim_send(int socket_fd, const void *buffer, size_t size,
                         int flags) {
  int translated = linux_message_flags(flags);
  if (socket_uses_no_sigpipe(socket_fd))
    translated |= MSG_NOSIGNAL;
  return translate_errno_result(send(socket_fd, buffer, size, translated));
}
static ssize_t shim_sendto(int socket_fd, const void *buffer, size_t size,
                           int flags, const void *address,
                           socklen_t address_size) {
  sockaddr_storage translated_address{};
  socklen_t translated_size =
      address ? linux_sockaddr(address, address_size, translated_address) : 0;
  int translated_flags = linux_message_flags(flags);
  if (socket_uses_no_sigpipe(socket_fd))
    translated_flags |= MSG_NOSIGNAL;
  return translate_errno_result(sendto(
      socket_fd, buffer, size, translated_flags,
      address ? reinterpret_cast<sockaddr *>(&translated_address) : nullptr,
      translated_size));
}
static ssize_t shim_recvfrom(int socket_fd, void *buffer, size_t size,
                             int flags, void *address,
                             socklen_t *address_size) {
  sockaddr_storage native_address{};
  socklen_t native_size = sizeof(native_address);
  ssize_t result = translate_errno_result(recvfrom(
      socket_fd, buffer, size, linux_message_flags(flags),
      address ? reinterpret_cast<sockaddr *>(&native_address) : nullptr,
      address ? &native_size : nullptr));
  if (result >= 0 && address)
    darwin_sockaddr(native_address, native_size, address, address_size);
  return result;
}
static ssize_t shim_sendmsg(int socket_fd, const DarwinMessageHeader *message,
                            int flags) {
  if (!message) {
    errno = EINVAL;
    return -1;
  }
  sockaddr_storage translated_address{};
  msghdr native{};
  if (message->name) {
    native.msg_namelen =
        linux_sockaddr(message->name, message->name_length, translated_address);
    native.msg_name = &translated_address;
  }
  native.msg_iov = message->vectors;
  native.msg_iovlen = std::max(0, message->vector_count);
  // TCP transfer frames do not carry ancillary data. Passing Darwin cmsghdr
  // bytes to Linux would be unsafe because its length field is wider.
  if (message->control_length) {
    errno = 22;
    return -1;
  }
  int translated_flags = linux_message_flags(flags);
  if (socket_uses_no_sigpipe(socket_fd))
    translated_flags |= MSG_NOSIGNAL;
  ssize_t result =
      translate_errno_result(sendmsg(socket_fd, &native, translated_flags));
  if (getenv("WETYPE_HOST_DEBUG"))
    fprintf(stderr,
            "DARWIN_SENDMSG fd=%d vectors=%d control=%u flags=%x result=%zd "
            "errno=%d\n",
            socket_fd, message->vector_count, message->control_length, flags,
            result, errno);
  return result;
}
static ssize_t shim_recvmsg(int socket_fd, DarwinMessageHeader *message,
                            int flags) {
  if (!message) {
    errno = EINVAL;
    return -1;
  }
  sockaddr_storage native_address{};
  msghdr native{};
  if (message->name) {
    native.msg_name = &native_address;
    native.msg_namelen = sizeof(native_address);
  }
  native.msg_iov = message->vectors;
  native.msg_iovlen = std::max(0, message->vector_count);
  std::vector<unsigned char> native_control;
  if (message->control && message->control_length) {
    native_control.resize(message->control_length + 32);
    native.msg_control = native_control.data();
    native.msg_controllen = native_control.size();
  }
  ssize_t result = translate_errno_result(
      recvmsg(socket_fd, &native, linux_message_flags(flags)));
  if (getenv("WETYPE_HOST_DEBUG"))
    fprintf(stderr,
            "DARWIN_RECVMSG fd=%d vectors=%d control=%u flags=%x result=%zd "
            "errno=%d\n",
            socket_fd, message->vector_count, message->control_length, flags,
            result, errno);
  if (result < 0)
    return result;
  if (message->name)
    darwin_sockaddr(native_address, native.msg_namelen, message->name,
                    &message->name_length);
  message->flags = native.msg_flags;
  // No ancillary messages are used by the gRPC TCP transport. Report no
  // Darwin control bytes rather than exposing Linux cmsghdr layout.
  message->control_length = 0;
  return result;
}
static int linux_socket_level(int level) {
  return level == 0xffff ? SOL_SOCKET : level;
}
static int linux_socket_option(int level, int option) {
  if (level == 0xffff) {
    switch (option) {
    case 0x4:
      return SO_REUSEADDR;
    case 0x8:
      return SO_KEEPALIVE;
    case 0x10:
      return SO_DONTROUTE;
    case 0x20:
      return SO_BROADCAST;
    case 0x80:
      return SO_LINGER;
    case 0x100:
      return SO_OOBINLINE;
    case 0x200:
      return SO_REUSEPORT;
    case 0x1001:
      return SO_SNDBUF;
    case 0x1002:
      return SO_RCVBUF;
    case 0x1003:
      return SO_SNDLOWAT;
    case 0x1004:
      return SO_RCVLOWAT;
    case 0x1005:
      return SO_SNDTIMEO;
    case 0x1006:
      return SO_RCVTIMEO;
    case 0x1007:
      return SO_ERROR;
    case 0x1008:
      return SO_TYPE;
    case 0x1022: // SO_NOSIGPIPE; Linux uses MSG_NOSIGNAL.
      return -1;
    default:
      return option;
    }
  }
  if (level == IPPROTO_IPV6 && option == 27)
    return IPV6_V6ONLY;
  return option;
}
static int shim_setsockopt(int socket_fd, int level, int option,
                           const void *value, socklen_t size) {
  int translated = linux_socket_option(level, option);
  if (translated < 0) {
    bool enabled =
        value && size >= sizeof(int) && *static_cast<const int *>(value);
    pthread_mutex_lock(&socket_state_lock);
    if (enabled)
      no_sigpipe_sockets.insert(socket_fd);
    else
      no_sigpipe_sockets.erase(socket_fd);
    pthread_mutex_unlock(&socket_state_lock);
    if (getenv("WETYPE_HOST_DEBUG"))
      fprintf(stderr, "DARWIN_SETSOCKOPT fd=%d SO_NOSIGPIPE=%d result=0\n",
              socket_fd, enabled);
    return 0;
  }
  int result = translate_errno_result(setsockopt(
      socket_fd, linux_socket_level(level), translated, value, size));
  if (getenv("WETYPE_HOST_DEBUG"))
    fprintf(stderr,
            "DARWIN_SETSOCKOPT fd=%d level=%d option=%d translated=%d/%d "
            "result=%d errno=%d\n",
            socket_fd, level, option, linux_socket_level(level), translated,
            result, errno);
  return result;
}
static int shim_getsockopt(int socket_fd, int level, int option, void *value,
                           socklen_t *size) {
  int translated = linux_socket_option(level, option);
  if (translated < 0) {
    pthread_mutex_lock(&socket_state_lock);
    bool enabled = no_sigpipe_sockets.count(socket_fd);
    pthread_mutex_unlock(&socket_state_lock);
    if (value && size && *size >= sizeof(int)) {
      *static_cast<int *>(value) = enabled;
      *size = sizeof(int);
    }
    if (getenv("WETYPE_HOST_DEBUG"))
      fprintf(stderr, "DARWIN_GETSOCKOPT fd=%d SO_NOSIGPIPE=%d result=0\n",
              socket_fd, enabled);
    return 0;
  }
  int result = translate_errno_result(getsockopt(
      socket_fd, linux_socket_level(level), translated, value, size));
  if (getenv("WETYPE_HOST_DEBUG"))
    fprintf(stderr,
            "DARWIN_GETSOCKOPT fd=%d level=%d option=%d translated=%d/%d "
            "result=%d errno=%d\n",
            socket_fd, level, option, linux_socket_level(level), translated,
            result, errno);
  return result;
}
static int shim_strerror_r(int error, char *buffer, size_t size) {
  const char *message = strerror(error);
  if (!buffer || !size)
    return ERANGE;
  size_t length = strlen(message);
  if (length >= size) {
    memcpy(buffer, message, size - 1);
    buffer[size - 1] = 0;
    return ERANGE;
  }
  memcpy(buffer, message, length + 1);
  return 0;
}
static int shim_close(int fd) {
  if (getenv("WETYPE_HOST_DEBUG"))
    fprintf(stderr, "DARWIN_CLOSE fd=%d caller=%p\n", fd,
            __builtin_return_address(0));
  pthread_mutex_lock(&socket_state_lock);
  no_sigpipe_sockets.erase(fd);
  pthread_mutex_unlock(&socket_state_lock);
  return close(fd);
}
static int shim_ioctl(int fd, unsigned long request, void *argument) {
  constexpr unsigned long DarwinFionbio = 0x8004667eUL;
  constexpr unsigned long DarwinFionread = 0x4004667fUL;
  unsigned long native = request;
  if (request == DarwinFionbio)
    native = FIONBIO;
  else if (request == DarwinFionread)
    native = FIONREAD;
  int result = ioctl(fd, native, argument);
  if (getenv("WETYPE_HOST_DEBUG"))
    fprintf(stderr,
            "DARWIN_IOCTL fd=%d request=%lx native=%lx result=%d errno=%d\n",
            fd, request, native, result, errno);
  return translate_errno_result(result);
}
static const char *libcpp_path_cstr(const void *path) {
  if (!path)
    return nullptr;
  const auto *bytes = static_cast<const unsigned char *>(path);
  if (bytes[0] & 1) {
    const char *value = nullptr;
    memcpy(&value, bytes + 16, sizeof(value));
    return value;
  }
  return reinterpret_cast<const char *>(bytes + 1);
}
static void set_libcpp_error_code(void *errorCode, int value) {
  if (errorCode)
    memcpy(errorCode, &value, sizeof(value));
}
static int copy_across_filesystems(const char *source, const char *target) {
  int input = open(source, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (input < 0)
    return -1;
  struct stat sourceStatus {};
  if (fstat(input, &sourceStatus) || !S_ISREG(sourceStatus.st_mode)) {
    const int error = errno ? errno : EINVAL;
    close(input);
    errno = error;
    return -1;
  }
  std::string temporary = std::string(target) + ".wetypex.XXXXXX";
  std::vector<char> name(temporary.begin(), temporary.end());
  name.push_back(0);
  int output = mkstemp(name.data());
  if (output < 0) {
    const int error = errno;
    close(input);
    errno = error;
    return -1;
  }
  fcntl(output, F_SETFD, FD_CLOEXEC);
  fchmod(output, sourceStatus.st_mode & 0777);
  std::array<unsigned char, 131072> buffer{};
  int error = 0;
  while (!error) {
    ssize_t count = read(input, buffer.data(), buffer.size());
    if (!count)
      break;
    if (count < 0) {
      if (errno == EINTR)
        continue;
      error = errno;
      break;
    }
    ssize_t offset = 0;
    while (offset < count) {
      ssize_t written = write(output, buffer.data() + offset, count - offset);
      if (written > 0)
        offset += written;
      else if (written < 0 && errno == EINTR)
        continue;
      else {
        error = errno ? errno : EIO;
        break;
      }
    }
  }
  if (!error && fsync(output))
    error = errno;
  if (close(output) && !error)
    error = errno;
  close(input);
  if (!error && rename(name.data(), target))
    error = errno;
  if (!error && unlink(source))
    error = errno;
  if (error) {
    unlink(name.data());
    errno = error;
    return -1;
  }
  return 0;
}
static void shim_filesystem_rename(const void *sourcePath,
                                   const void *targetPath, void *errorCode) {
  const char *source = libcpp_path_cstr(sourcePath);
  const char *target = libcpp_path_cstr(targetPath);
  int result = -1;
  if (source && target)
    result = rename(source, target);
  if (result && errno == EXDEV)
    result = copy_across_filesystems(source, target);
  const int error = result ? (errno ? errno : EIO) : 0;
  set_libcpp_error_code(errorCode, error);
  if (getenv("WETYPE_HOST_DEBUG"))
    fprintf(stderr,
            "DARWIN_FILESYSTEM_RENAME source=%s target=%s result=%d errno=%d\n",
            source ? source : "(null)", target ? target : "(null)", result,
            error);
}
static int shim_notify_register() { return ENOTSUP; }
static int shim_notify_cancel() { return 0; }
static int shim_fd_overflow(int fd) { return fd >= FD_SETSIZE; }
static bool address_is_in_mapped_image(const void *value) {
  const auto address = reinterpret_cast<uintptr_t>(value);
  for (const auto &[begin, end] : mapped_image_ranges)
    if (address >= begin && address < end)
      return true;
  return false;
}
static void shim_free(void *value) {
  // Some bundled BoringSSL/OpenSSL compatibility objects are backed directly
  // by Mach-O __DATA and are accepted by Darwin's allocator cleanup path.  A
  // glibc free on those image addresses aborts.  They remain owned by the
  // mapped image and must live until the compatibility host exits.
  if (!value || address_is_in_mapped_image(value))
    return;
  free(value);
}
static void shim_uuid_generate(unsigned char *output) {
  if (!output)
    return;
  size_t offset = 0;
  while (offset < 16) {
    ssize_t count = getrandom(output + offset, 16 - offset, 0);
    if (count > 0)
      offset += size_t(count);
    else if (count < 0 && errno == EINTR)
      continue;
    else
      abort();
  }
  output[6] = (output[6] & 0x0f) | 0x40;
  output[8] = (output[8] & 0x3f) | 0x80;
}
static void shim_uuid_unparse(const unsigned char *value, char *output) {
  if (!value || !output)
    return;
  snprintf(output, 37,
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
           "%02x%02x%02x%02x%02x%02x",
           value[0], value[1], value[2], value[3], value[4], value[5], value[6],
           value[7], value[8], value[9], value[10], value[11], value[12],
           value[13], value[14], value[15]);
}
static void *resolve(const std::string &name) {
  if (auto symbol = syms.find(name); symbol != syms.end())
    return reinterpret_cast<void *>(symbol->second);
  static std::map<std::string, void *> overrides = {
      {"___stack_chk_guard", &stack_guard},
      {"___error", (void *)shim_error},
      {"___bzero", (void *)shim_bzero},
      {"_atexit", (void *)shim_atexit},
      {"_SecRandomCopyBytes", (void *)shim_sec_random_copy_bytes},
      {"_kSecRandomDefault", &sec_random_default},
      {"___stderrp", &stderr},
      {"___stdoutp", &stdout},
      {"___stdinp", &stdin},
      {"__tlv_bootstrap", (void *)shim_tlv},
      {"__tlv_atexit", (void *)shim_tlv_atexit},
      {"____chkstk_darwin", (void *)shim_chkstk},
      {"_dlsym", (void *)shim_dlsym},
      {"_malloc_create_zone", (void *)shim_malloc_create_zone},
      {"_malloc_default_zone", (void *)shim_malloc_default_zone},
      {"_malloc_set_zone_name", (void *)shim_malloc_set_zone_name},
      {"_malloc_size", (void *)shim_malloc_size},
      {"_malloc_zone_free", (void *)shim_malloc_zone_free},
      {"_malloc_zone_malloc", (void *)shim_malloc_zone_malloc},
      {"_malloc_zone_realloc", (void *)shim_malloc_zone_realloc},
      {"_reallocf", (void *)shim_reallocf},
      {"_sysctlbyname", (void *)shim_sysctlbyname},
      {"_sysctl", (void *)shim_sysctl},
      {"_mmap", (void *)shim_mmap},
      {"___darwin_check_fd_set_overflow", (void *)shim_fd_overflow},
      {"_accept", (void *)shim_accept},
      {"_bind", (void *)shim_bind},
      {"_close", (void *)shim_close},
      {"_connect", (void *)shim_connect},
      {"_connectx", (void *)shim_connectx},
      {"_dispatch_release", (void *)shim_dispatch_release},
      {"_dispatch_semaphore_create", (void *)shim_dispatch_semaphore_create},
      {"_dispatch_semaphore_signal", (void *)shim_dispatch_semaphore_signal},
      {"_dispatch_semaphore_wait", (void *)shim_dispatch_semaphore_wait},
      {"_fopen$DARWIN_EXTSN", (void *)fopen},
      {"_free", (void *)shim_free},
      {"_freeaddrinfo", (void *)shim_freeaddrinfo},
      {"_getaddrinfo", (void *)shim_getaddrinfo},
      {"_getsockname", (void *)shim_getsockname},
      {"_getsockopt", (void *)shim_getsockopt},
      {"_inet_ntop", (void *)shim_inet_ntop},
      {"_inet_pton", (void *)shim_inet_pton},
      {"_ioctl", (void *)shim_ioctl},
      {"_listen", (void *)shim_listen},
      {"_notify_cancel", (void *)shim_notify_cancel},
      {"_notify_register_file_descriptor", (void *)shim_notify_register},
      {"_poll", (void *)shim_poll},
      {"_pthread_atfork", (void *)pthread_atfork},
      {"_pthread_attr_destroy", (void *)shim_pthread_attr_destroy},
      {"_pthread_attr_init", (void *)shim_pthread_attr_init},
      {"_pthread_attr_setdetachstate",
       (void *)shim_pthread_attr_setdetachstate},
      {"_pthread_attr_setstacksize", (void *)shim_pthread_attr_setstacksize},
      {"_pthread_create", (void *)shim_pthread_create},
      {"_pthread_once", (void *)shim_pthread_once},
      {"_read", (void *)shim_read},
      {"_recvfrom", (void *)shim_recvfrom},
      {"_recvmsg", (void *)shim_recvmsg},
      {"_select$1050", (void *)select},
      {"_select$DARWIN_EXTSN", (void *)select},
      {"_sigsetjmp", (void *)__sigsetjmp},
      {"_setsockopt", (void *)shim_setsockopt},
      {"_send", (void *)shim_send},
      {"_sendmsg", (void *)shim_sendmsg},
      {"_sendto", (void *)shim_sendto},
      {"_socket", (void *)shim_socket},
      {"_strerror_r", (void *)shim_strerror_r},
      {"_write", (void *)shim_write},
      {"_writev", (void *)shim_writev},
      {"_uuid_generate", (void *)shim_uuid_generate},
      {"_uuid_unparse", (void *)shim_uuid_unparse},
      {"__ZNSt3__14__fs10filesystem8__renameERKNS1_4pathES4_PNS_10error_codeE",
       (void *)shim_filesystem_rename},
      {"_fstat$INODE64", (void *)shim_fstat},
      {"_fstatfs$INODE64", (void *)shim_fstatfs},
      {"_stat$INODE64", (void *)shim_stat},
      {"_statfs$INODE64", (void *)shim_statfs},
      {"_lstat$INODE64", (void *)shim_lstat},
      {"_open", (void *)shim_open},
      {"_opendir$INODE64", (void *)opendir},
      {"_readdir$INODE64", (void *)shim_readdir},
      {"_fcntl", (void *)shim_fcntl},
      {"_pthread_rwlock_init", (void *)shim_rw_init},
      {"_pthread_rwlock_rdlock", (void *)shim_rw_rdlock},
      {"_pthread_rwlock_wrlock", (void *)shim_rw_wrlock},
      {"_pthread_rwlock_tryrdlock", (void *)shim_rw_tryrdlock},
      {"_pthread_rwlock_trywrlock", (void *)shim_rw_trywrlock},
      {"_pthread_rwlock_unlock", (void *)shim_rw_unlock},
      {"_pthread_rwlock_destroy", (void *)shim_rw_destroy},
      {"_CFTimeZoneCopyDefault", (void *)shim_timezone_default},
      {"_CFTimeZoneGetName", (void *)shim_timezone_name},
      {"_CFStringGetCStringPtr", (void *)shim_cf_string_ptr},
      {"_CFStringGetCString", (void *)shim_cf_string_copy},
      {"_CFStringGetLength", (void *)shim_cf_string_length},
      {"_CFRelease", (void *)shim_cf_release},
      {"_CFRetain", (void *)shim_cf_retain},
      {"_CFAbsoluteTimeGetCurrent", (void *)shim_cf_time},
      {"_CFStringGetMaximumSizeForEncoding", (void *)shim_cf_string_max},
      {"___toupper", (void *)shim_toupper},
      {"___tolower", (void *)shim_tolower},
      {"__DefaultRuneLocale", default_rune_locale},
      {"___maskrune", (void *)shim_maskrune},
      {"_memset_pattern16", (void *)shim_pattern16},
      {"_pthread_mutex_init", (void *)shim_mutex_init},
      {"_pthread_mutex_lock", (void *)shim_mutex_lock},
      {"_pthread_mutex_unlock", (void *)shim_mutex_unlock},
      {"_pthread_mutex_trylock", (void *)shim_mutex_trylock},
      {"_pthread_mutex_destroy", (void *)shim_mutex_destroy},
      {"_pthread_cond_init", (void *)shim_cond_init},
      {"_pthread_cond_signal", (void *)shim_cond_signal},
      {"_pthread_cond_broadcast", (void *)shim_cond_broadcast},
      {"_pthread_cond_wait", (void *)shim_cond_wait},
      {"_pthread_cond_timedwait", (void *)shim_cond_timedwait},
      {"_pthread_cond_timedwait_relative_np", (void *)shim_cond_relative},
      {"_pthread_cond_destroy", (void *)shim_cond_destroy},
      {"_pthread_threadid_np", (void *)shim_threadid},
      {"__ZNSt3__15mutex4lockEv", (void *)shim_mutex_lock},
      {"__ZNSt3__15mutex6unlockEv", (void *)shim_mutex_unlock},
      {"__ZNSt3__15mutex8try_lockEv", (void *)shim_cpp_mutex_try_lock},
      {"__ZNSt3__15mutexD1Ev", (void *)shim_mutex_destroy},
      {"__ZNSt3__118condition_variable4waitERNS_11unique_lockINS_5mutexEEE",
       (void *)shim_cpp_cv_wait},
      {"__ZNSt3__118condition_variable10notify_oneEv",
       (void *)shim_cond_signal},
      {"__ZNSt3__118condition_variable10notify_allEv",
       (void *)shim_cond_broadcast},
      {"__ZNSt3__118condition_variable15__do_timed_waitERNS_11unique_lockINS_"
       "5mutexEEENS_6chrono10time_pointINS5_12system_clockENS5_8durationIxNS_"
       "5ratioILl1ELl1000000000EEEEEEE",
       (void *)shim_cpp_cv_timed_wait},
      {"__ZNSt3__118condition_variableD1Ev", (void *)shim_cond_destroy}};
  overrides.emplace("__ZNSt3__17promiseIvE10get_futureEv",
                    (void *)shim_promise_void_get_future);
  auto it = overrides.find(name);
  if (it != overrides.end())
    return it->second;
  if (name == "__ZNKSt3__16locale9use_facetERNS0_2idE")
    return dlsym(RTLD_DEFAULT, "wetype_locale_use_facet");
  if (name == "__ZNSt3__115__get_classnameEPKcb")
    return dlsym(RTLD_DEFAULT, "wetype_get_classname");
  // Never attempt to use macOS framework objects as native ELF objects.
  if (name.rfind("_OBJC_", 0) == 0 || name.rfind("_$s", 0) == 0)
    return nullptr;
  return dlsym(RTLD_DEFAULT, name.c_str() + 1);
}
static void exception_callback(const char *s, unsigned len) {
  fprintf(stderr, "ENGINE_EXCEPTION %.*s\n", (int)len, s);
}
static void engine_log(int level, const char *s) {
  if (!service_mode)
    fprintf(stderr, "IME[%d] %s\n", level, s ? s : "(null)");
}
#include "business_services.hpp"
#include "service.hpp"
struct LlmResultItem {
  const unsigned char *data;
  uint32_t data_length, pad0;
  const char *text;
  uint32_t text_length;
  int32_t type;
  uint32_t reserved;
  bool flag;
  unsigned char pad2[3];
};
static_assert(sizeof(LlmResultItem) == 40);
struct LlmEvent {
  int32_t error_code, chat_id, frame;
  bool is_end;
  unsigned char pad0[3];
  LlmResultItem *results;
  uint32_t result_count, pad1;
  const unsigned char *cookie;
  uint32_t cookie_length, pad2;
  const char *additional;
  uint32_t additional_length, pad3;
};
static_assert(sizeof(LlmEvent) == 64);
static std::mutex llm_service_mutex;
static std::vector<unsigned char> llm_service_cookie;
static unsigned llm_service_events = 0;
static std::atomic<bool> llm_service_finished{false};
static void llm_service_callback(LlmEvent event) {
  bool finished = false;
  fprintf(stdout, "LLM_EVENT chat=%d frame=%d error=%d end=%d results=%u\n",
          event.chat_id, event.frame, event.error_code, event.is_end,
          event.result_count);
  fprintf(stdout, "LLM_COOKIE bytes=%u\n", event.cookie_length);
  if (event.cookie_length > 4194304 || (event.cookie_length && !event.cookie) ||
      event.additional_length > 4194304 ||
      (event.additional_length && !event.additional))
    _exit(89);
  if (event.result_count > 64 || (event.result_count && !event.results))
    _exit(89);
  for (unsigned i = 0; i < event.result_count; ++i) {
    auto &item = event.results[i];
    if (item.data_length > 1048576 || (!item.data && item.data_length) ||
        item.text_length > 1048576 || (!item.text && item.text_length))
      _exit(89);
    fprintf(stdout, "LLM_DATA index=%u bytes=%u text=%.*s\n", i,
            item.data_length, int(item.data_length),
            item.data ? reinterpret_cast<const char *>(item.data) : "");
    fprintf(stdout, "LLM_RESULT index=%u type=%d text=%.*s\n", i, item.type,
            int(item.text_length), item.text ? item.text : "");
    std::string metadata(item.text ? item.text : "", item.text_length);
    if (metadata.find("\"is_answer_end\":1") != std::string::npos ||
        metadata.find("\"is_answer_end\":true") != std::string::npos)
      finished = true;
  }
  // is_end terminates this transport frame. A nonempty cookie continues the
  // answer in a later request, even when the current frame is complete.
  if (event.is_end && (event.error_code || !event.cookie_length))
    finished = true;
  {
    std::lock_guard lock(llm_service_mutex);
    llm_service_cookie.assign(event.cookie,
                              event.cookie ? event.cookie + event.cookie_length
                                           : event.cookie);
    ++llm_service_events;
  }
  fflush(stdout);
  // Do not let the main thread exit before the final rendered document has
  // been flushed by this callback.
  if (finished)
    llm_service_finished = true;
}
struct LlmSearchParam {
  const char *text;
  uint32_t text_length, pad0;
  const unsigned char *cookie;
  uint32_t cookie_length;
  int32_t accept_text_type;
  uint64_t wechat_ability;
  int32_t return_key_type, pad1;
  const char *app_name;
  uint32_t app_name_length;
  bool warm_up;
  unsigned char pad2[3];
};
static_assert(sizeof(LlmSearchParam) == 64);
static void run_llm_service(const char *question) {
  const char *app = "LINUX";
  using Search = uint32_t (*)(decltype(&llm_service_callback), LlmSearchParam);
  auto search = (Search)syms.at("_wxime_cloud_llm_search_without_session");
  auto invoke = [&](const char *text,
                    const std::vector<unsigned char> &cookie) {
    LlmSearchParam parameter{text,
                             uint32_t(strlen(text)),
                             0,
                             cookie.empty() ? nullptr : cookie.data(),
                             uint32_t(cookie.size()),
                             1,
                             0,
                             0,
                             0,
                             app,
                             uint32_t(strlen(app)),
                             false,
                             {}};
    return search(llm_service_callback, parameter);
  };
  uint32_t request = invoke(question, {});
  fprintf(stdout, "LLM_REQUEST id=%u\n", request);
  fflush(stdout);
  unsigned handled = 0;
  for (unsigned tick = 0; tick < 600 && !llm_service_finished; ++tick) {
    std::vector<unsigned char> cookie;
    {
      std::lock_guard lock(llm_service_mutex);
      if (llm_service_events > handled) {
        handled = llm_service_events;
        cookie = llm_service_cookie;
      }
    }
    if (!cookie.empty()) {
      request = invoke("", cookie);
      fprintf(stdout, "LLM_CONTINUE id=%u frame=%u\n", request, handled);
      fflush(stdout);
    }
    usleep(50000);
  }
}
static std::vector<unsigned char> ehframe;
static void emit32(std::vector<unsigned char> &b, uint32_t n) {
  for (int i = 0; i < 4; i++)
    b.push_back(n >> (8 * i));
}
static void emit64(std::vector<unsigned char> &b, uint64_t n) {
  for (int i = 0; i < 8; i++)
    b.push_back(n >> (8 * i));
}
static void uleb(std::vector<unsigned char> &b, uint64_t n) {
  do {
    unsigned char c = n & 127;
    n >>= 7;
    b.push_back(c | (n ? 128 : 0));
  } while (n);
}
static void record(std::vector<unsigned char> &b) {
  while (b.size() % 8 != 4)
    b.push_back(0);
  emit32(ehframe, b.size());
  ehframe.insert(ehframe.end(), b.begin(), b.end());
}
static void setup_unwind(const std::vector<std::string> &entries) {
  std::vector<unsigned char> cie;
  emit32(cie, 0);
  for (auto c : {1, 'z' + 0, 'P' + 0, 'L' + 0, 'R' + 0, 0, 1, 0x78, 16, 11, 0})
    cie.push_back(c);
  emit64(cie, (uintptr_t)dlsym(RTLD_DEFAULT, "__gxx_personality_v0"));
  cie.insert(cie.end(), {0, 0, 0x0c, 7, 8, 0x90, 1});
  record(cie);
  const unsigned regs[] = {0, 3, 12, 13, 14, 15, 6};
  for (auto &line : entries) {
    std::istringstream s(line);
    std::string tag;
    uintptr_t addr, len, enc, lsda;
    s >> tag >> std::hex >> addr >> len >> enc >> lsda;
    std::vector<unsigned char> b;
    emit32(b, ehframe.size() + 4);
    emit64(b, addr);
    emit64(b, len);
    b.push_back(8);
    emit64(b, lsda);
    b.insert(b.end(), {0x0c, 6, 16, 0x86, 2});
    unsigned offset = (enc >> 16) & 255;
    for (unsigned i = 0; i < 5; i++) {
      unsigned reg = (enc >> (3 * i)) & 7;
      if (reg && reg <= 6) {
        b.push_back(0x80 | regs[reg]);
        uleb(b, offset + 2 - i);
      }
    }
    record(b);
  }
  emit32(ehframe, 0);
  auto fn = (void (*)(void *))dlsym(RTLD_DEFAULT, "__register_frame");
  if (!fn) {
    fputs("missing ELF unwind registration\n", stderr);
    _exit(86);
  }
  fn(ehframe.data());
  fprintf(stderr, "UNWIND registered %zu RBP frame entries\n", entries.size());
}
int main(int argc, char **argv) {
  if (argc != 3 && argc != 4) {
    fprintf(stderr, "usage: host PREPARED_DIR MODE [ASCII_PINYIN]\n");
    return 2;
  }
  service_mode = !strcmp(argv[2], "serve") ||
                 !strcmp(argv[2], "flurry-server") ||
                 !strcmp(argv[2], "flurry-wxp2p-server") ||
                 !strcmp(argv[2], "wxp2p-probe");
  if (!service_mode) {
    const bool long_operation =
        !strcmp(argv[2], "llm-service") || !strcmp(argv[2], "flurry-loopback");
    alarm(long_operation ? 45 : 15);
  }
  initialize_rune_locale();
  setvbuf(stderr, nullptr, _IONBF, 0);
  struct sigaction sa{};
  sa.sa_sigaction = crash;
  sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, nullptr);
  sigaction(SIGBUS, &sa, nullptr);
  sigaction(SIGILL, &sa, nullptr);
  sigaction(SIGABRT, &sa, nullptr);
  if (!dlopen("libc++.so.1", RTLD_NOW | RTLD_GLOBAL) ||
      !dlopen("libc++abi.so.1", RTLD_NOW | RTLD_GLOBAL)) {
    fputs(dlerror(), stderr);
    return 2;
  }
  const char *support = getenv("WETYPE_SUPPORT_DIR");
  std::string support_dir = support ? support : argv[1];
  auto localePath = support_dir + "/locale.so";
  if (access(localePath.c_str(), R_OK) != 0)
    localePath = support_dir + "/wetypex-locale.so";
  if (!dlopen(localePath.c_str(), RTLD_NOW | RTLD_GLOBAL)) {
    fputs(dlerror(), stderr);
    return 2;
  }
  if (!dlopen((support_dir + "/libkqueue.so").c_str(),
              RTLD_NOW | RTLD_GLOBAL)) {
    fputs(dlerror(), stderr);
    return 2;
  }
  if (!dlopen((support_dir + "/wcwss_bridge.so").c_str(),
              RTLD_NOW | RTLD_GLOBAL)) {
    fputs(dlerror(), stderr);
    return 2;
  }
  if (std::string(argv[2]) == "network-info") {
    auto version = reinterpret_cast<const char *(*)()>(
        dlsym(RTLD_DEFAULT, "wcwss_bridge_curl_version"));
    auto provider = reinterpret_cast<const char *(*)()>(
        dlsym(RTLD_DEFAULT, "wcwss_bridge_curl_provider"));
    auto capabilities = reinterpret_cast<unsigned (*)()>(
        dlsym(RTLD_DEFAULT, "wcwss_bridge_capabilities"));
    if (!version || !provider || !capabilities)
      return 2;
    const auto flags = capabilities();
    auto report = wire::object();
    wire::put(report.get(), "libcurl_version", std::string(version()));
    wire::put(report.get(), "provider", std::string(provider()));
    wire::boolean(report.get(), "ws", flags & 1);
    wire::boolean(report.get(), "wss", flags & 2);
    wire::boolean(report.get(), "ipv6", flags & 4);
    wire::put(report.get(), "address_policy",
              std::string("happy-eyeballs-200ms-with-ipv4-retry"));
    puts(wire::dump(report.get()).c_str());
    return (flags & 3) == 3 ? 0 : 1;
  }
  std::string dir = argv[1], mode = argv[2], line;
  std::vector<std::string> binds, unwinds;
  std::vector<std::pair<uintptr_t, std::string>> ctors;
  auto loadModule = [&](const std::string &moduleDir) {
    std::ifstream symbols(moduleDir + "/symbols.txt");
    while (std::getline(symbols, line)) {
      std::istringstream stream(line);
      uintptr_t address;
      std::string name;
      stream >> std::hex >> address >> name;
      if (!name.empty())
        address_names.emplace_back(address, name);
    }
    std::ifstream manifest(moduleDir + "/manifest.txt");
    int module = open((moduleDir + "/image.macho").c_str(), O_RDONLY);
    if (module < 0)
      return false;
    while (std::getline(manifest, line)) {
      std::istringstream stream(line);
      std::string type;
      stream >> type;
      if (type == "SEG") {
        uintptr_t address, size, offset, length;
        int protection;
        std::string name;
        stream >> std::hex >> address >> size >> offset >> length >> std::dec >>
            protection >> name;
        size = (size + 4095) & ~4095ULL;
        if (name == "__LINKEDIT")
          continue;
        void *mapping = mmap(
            reinterpret_cast<void *>(address), size, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (mapping == MAP_FAILED) {
          perror("map segment");
          close(module);
          return false;
        }
        mapped_image_ranges.emplace_back(address, address + size);
        if (length) {
          if (length % 4096 || offset % 4096) {
            fputs("unsupported segment alignment\n", stderr);
            close(module);
            return false;
          }
          int nativeProtection =
              (protection & 4) ? PROT_READ | PROT_EXEC : PROT_READ | PROT_WRITE;
          if (mmap(mapping, length, nativeProtection, MAP_PRIVATE | MAP_FIXED,
                   module, offset) == MAP_FAILED) {
            perror("map file segment");
            close(module);
            return false;
          }
        }
      } else if (type == "BIND")
        binds.push_back(line);
      else if (type == "REBASE") {
        uintptr_t address, slide;
        stream >> std::hex >> address >> slide;
        *reinterpret_cast<uintptr_t *>(address) += slide;
      } else if (type == "OWN") {
        uintptr_t address, value;
        long addend;
        stream >> std::hex >> address >> value >> std::dec >> addend;
        *reinterpret_cast<uintptr_t *>(address) = value + addend;
      } else if (type == "UNWIND")
        unwinds.push_back(line);
      else if (type == "SYM") {
        uintptr_t address;
        std::string name;
        stream >> std::hex >> address >> name;
        syms[name] = address;
      } else if (type == "CTOR") {
        uintptr_t address;
        std::string name;
        stream >> std::hex >> address >> name;
        ctors.emplace_back(address, name);
      } else if (type == "SECTION") {
        uintptr_t address, size;
        std::string name;
        stream >> std::hex >> address >> size >> name;
        if (name == "__thread_data") {
          tls_data = address;
          tls_size = size;
        }
        if (name == "__thread_bss")
          tls_total = address + size;
      }
    }
    close(module);
    return true;
  };
  if (const char *auxiliary = getenv("WETYPE_AUX_RUNTIME");
      auxiliary && *auxiliary && !loadModule(auxiliary))
    return 2;
  if (!loadModule(dir))
    return 2;
  tls_total -= tls_data;
  std::map<std::string, void *> cache;
  unsigned unresolved = 0;
  for (auto &ln : binds) {
    std::istringstream s(ln);
    std::string tag, n;
    uintptr_t a;
    long add;
    s >> tag >> std::hex >> a >> std::dec >> add >> n;
    void *p;
    auto it = cache.find(n);
    if (it != cache.end())
      p = it->second;
    else {
      p = resolve(n);
      if (!p) {
        ++unresolved;
        if (mode == "module-map")
          fprintf(stderr, "UNRESOLVED %s\n", n.c_str());
        p = trap(n);
      }
      cache[n] = p;
    }
    *(uintptr_t *)a = (uintptr_t)p + add;
  }
  if (trap_arena && mprotect(trap_arena, 1048576, PROT_READ | PROT_EXEC))
    return 2;
  fprintf(stderr, "MAPPED imports=%zu unresolved=%u tls=%lu\n", cache.size(),
          unresolved, tls_total);
  if (mode == "module-map")
    return 0;
  if (mode == "module-run" || mode == "wxp2p-probe" ||
      mode == "flurry-wxp2p-server" || mode == "flurry-credentials" ||
      mode == "flurry-loopback" || mode == "flurry-server") {
    setup_unwind(unwinds);
    for (auto &[addr, name] : ctors) {
      if (getenv("WETYPE_HOST_DEBUG"))
        fprintf(stderr, "MODULE_CTOR %lx %s\n", addr, name.c_str());
      ((void (*)())addr)();
    }
    fprintf(stderr, "MODULE_CONSTRUCTORS_RETURNED count=%zu\n", ctors.size());
    if (mode == "module-run")
      return 0;
    if (mode == "wxp2p-probe")
      return run_wxp2p_probe();
    using Generate = void (*)(
        void *, void (*)(void *, const FlurryLocalCredentials *, const char *));
    ((Generate)syms.at("__ZN6flurry6Flurry23GenerateGrpcCredentialsEPvPFvS1_"
                       "PKNS_20GrpcLocalCredentialsEPKcE"))(
        nullptr, flurry_credentials_callback);
    if (!flurry_credentials_ready)
      return 2;
    if (mode == "flurry-wxp2p-server")
      return run_flurry_wxp2p_server();
    if (mode == "flurry-loopback")
      _exit(run_flurry_loopback_test() ? 0 : 2);
    if (mode == "flurry-server")
      return run_flurry_server();
    return 0;
  }
  fprintf(stderr, "VERSION %s\n",
          ((const char *(*)())syms.at("_wxime_get_version"))());
  if (mode == "version")
    return 0;
  setup_unwind(unwinds);
  for (auto &[a, n] : address_names)
    if (n == "__ZN5wxime5utils26SetReportExceptionCallBackEPFvPKcjE")
      ((void (*)(void (*)(const char *, unsigned)))a)(exception_callback);
  // Initialize the original C/C++ core region. AppKit/Swift UI constructors
  // remain outside this compatibility boundary.
  const bool verbose_host = getenv("WETYPE_HOST_DEBUG");
  for (auto &[addr, name] : ctors) {
    if (addr < 0x1005f51c0 || addr >= 0x101b00000)
      continue;
    if (verbose_host)
      fprintf(stderr, "CTOR %lx %s\n", addr, name.c_str());
    ((void (*)())addr)();
  }
  fprintf(stderr, "CORE_CONSTRUCTORS_RETURNED\n");
  if (mode == "business-service")
    business_service();
  // Version-pinned wxime initialization ABI.
  alignas(16) unsigned char config[1024]{};
  const char *work = getenv("WETYPE_WORK_DIR");
  if (!work || !*work)
    work = "/work/user";
  memcpy(config + 0x18, &work, 8);
  void *logger = (void *)engine_log;
  memcpy(config, &logger, 8);
  config[8] = 1;
  auto setString = [&](size_t offset, const char *value) {
    memcpy(config + offset, &value, sizeof(value));
    *(uint32_t *)(config + offset + 8) = strlen(value);
  };
  // Recovered from Android 3.5.4's InitInfo -> wxime_init_config bridge and
  // cross-checked against this Mac build's shared C ABI.
  // Account identity belongs to wxime_network_login.  The desktop init ABI
  // leaves this field empty; putting an account UIN here makes the core enter
  // its device-code generation path before network-login information exists.
  setString(0x58, "2.2.3(657)");
  // The upstream protocol has no Linux platform enum, so use the original Mac
  // platform value while presenting an honest Linux device name.
  const char *device_name = getenv("WETYPE_DEVICE_MODEL");
  setString(0x68, device_name && *device_name ? device_name : "LINUX");
  setString(0x78, "15.2.0");
  *(uint32_t *)(config + 0x50) = 5; // observed original desktop platform config
  *(uint32_t *)(config + 0xc0) = 3; // observed package config
  // +0x10 is JSON logging configuration, NOT the dictionary resource path.
  // Original dictionary loading is a separate wxime_config_dict operation.
  void *callback = (void *)exception_callback;
  memcpy(config + 0x88, &callback, 8);
  fprintf(stderr, "CALL wxime_initialize config=%p\n", config);
  try {
    ((void (*)(void *))syms.at("_wxime_initialize"))(config);
  } catch (const std::exception &e) {
    fprintf(stderr, "INITIALIZE_EXCEPTION %s\n", e.what());
    _exit(87);
  }
  fprintf(stderr, "WXIME_INITIALIZE_RETURNED (not candidate proof)\n");
  if (mode == "llm-service")
    network_login_service(false);
  if (mode == "llm-service") {
    run_llm_service(argc == 4 ? argv[3] : "用一句话介绍Linux");
    _exit(0);
  }
  if (mode == "serve" && getenv("WETYPE_NETWORK_LIVE"))
    network_login_service(false, false, false);
  struct Dict {
    void *asset;
    const char *path;
    uint32_t id, version;
  };
  struct DictConfig {
    Dict *dicts;
    uint32_t count;
    uint32_t padding;
    const char *user_path;
  };
  static_assert(sizeof(Dict) == 24 && sizeof(DictConfig) == 24);
  std::vector<std::string> paths;
  paths.reserve(100);
  std::vector<Dict> dicts;
  std::ifstream dictfile(dir + "/dicts.txt");
  while (std::getline(dictfile, line)) {
    std::istringstream s(line);
    uint32_t id, version;
    std::string name;
    s >> id >> version >> name;
    if (name.empty())
      continue;
    paths.push_back("/input/resources/" + name);
    dicts.push_back({nullptr, paths.back().c_str(), id, version});
  }
  DictConfig dc{dicts.data(), (uint32_t)dicts.size(), 0, "/work/userDict"};
  fprintf(stderr, "CALL wxime_config_dict count=%u\n", dc.count);
  bool loaded =
      ((bool (*)(const DictConfig *))syms.at("_wxime_config_dict"))(&dc);
  fprintf(stderr, "WXIME_CONFIG_DICT_RETURNED %d\n", loaded);
  if (!loaded)
    _exit(88);
  if (mode == "validate")
    _exit(0);
  if (mode == "serve" && getenv("WETYPE_NETWORK_LIVE")) {
    const auto groupId =
        strtoull(getenv("WETYPE_GROUP_ID") ? getenv("WETYPE_GROUP_ID") : "0",
                 nullptr, 10);
    const auto functions = strtoull(getenv("WETYPE_GROUP_FUNCTIONS")
                                        ? getenv("WETYPE_GROUP_FUNCTIONS")
                                        : "0",
                                    nullptr, 10);
    const auto phraseVersion = strtoull(getenv("WETYPE_HOTWORD_VERSION")
                                            ? getenv("WETYPE_HOTWORD_VERSION")
                                            : "0",
                                        nullptr, 10);
    if (groupId) {
      // Recovered from Windows 2.1.3.18's only call site and the macOS
      // 2.2.3.657 implementation/log labels: unknown, debug, group id,
      // function mask, common-phrase version, personal-dictionary version.
      configure_group_sync(groupId, functions, phraseVersion, 0);
      fprintf(stderr, "GROUP_SYNC_CONFIGURED group=%llu functions=%llu\n",
              groupId, functions);
    }
  }
  if (mode == "serve") {
    service_loop();
    _exit(0);
  }
  fprintf(stderr, "Unsupported engine-host mode: %s\n", mode.c_str());
  return 64;
}
