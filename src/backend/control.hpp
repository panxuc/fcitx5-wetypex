// A same-user management channel. It shares the serialized engine thread with
// keyboard input, so settings never race the original dictionary
// implementation.
#pragma once
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <fcntl.h>
#include <map>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>
class ServiceTransport {
  struct Peer {
    std::string input, output;
    uint64_t active = 0;
  };
  int server_ = -1;
  int directory_ = -1;
  int wake_ = -1;
  bool pendingUpdate_ = false;
  std::map<int, Peer> peers_;
  std::string input_;
  std::deque<std::pair<int, std::string>> ready_;
  static uint64_t tick() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec;
  }
  void dropPeer(int fd) {
    close(fd);
    peers_.erase(fd);
    ready_.erase(std::remove_if(ready_.begin(), ready_.end(),
                               [fd](const auto &item) { return item.first == fd; }),
                 ready_.end());
  }
  bool collect(int fd, std::string &buffer) {
    size_t end;
    while ((end = buffer.find('\n')) != std::string::npos) {
      if (ready_.size() >= 128)
        return false;
      ready_.emplace_back(fd, buffer.substr(0, end));
      buffer.erase(0, end + 1);
    }
    return true;
  }

public:
  ServiceTransport() {
    wake_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    server_ = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    directory_ = open(workspace_root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    // Keep long Flatpak/private-home paths out of sockaddr_un's 108 bytes.
    const auto path = "/proc/self/fd/" + std::to_string(directory_) + "/control.sock";
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", path.c_str());
    if (directory_ >= 0)
      unlinkat(directory_, "control.sock", 0);
    mode_t mask = umask(0077);
    bool ok = server_ >= 0 && directory_ >= 0 &&
              bind(server_, (sockaddr *)&address, sizeof(address)) == 0 &&
              listen(server_, 4) == 0;
    umask(mask);
    if (!ok) {
      if (server_ >= 0)
        close(server_);
      server_ = -1;
    }
  }
  ~ServiceTransport() {
    if (wake_ >= 0)
      close(wake_);
    for (auto &[fd, p] : peers_)
      close(fd);
    if (server_ >= 0) {
      close(server_);
      unlinkat(directory_, "control.sock", 0);
    }
    if (directory_ >= 0)
      close(directory_);
  }
  bool supportsUpdates() const { return wake_ >= 0; }
  void notify() const {
    if (wake_ < 0)
      return;
    const uint64_t value = 1;
    while (write(wake_, &value, sizeof(value)) < 0 && errno == EINTR) {
    }
  }
  bool next(std::string &line, int &source) {
    while (ready_.empty()) {
      if (pendingUpdate_) {
        pendingUpdate_ = false;
        source = -1;
        line.clear();
        return true;
      }
      std::vector<pollfd> fds = {{STDIN_FILENO, POLLIN, 0}};
      if (wake_ >= 0)
        fds.push_back({wake_, POLLIN, 0});
      if (server_ >= 0)
        fds.push_back({server_, POLLIN, 0});
      for (auto &[fd, p] : peers_)
        fds.push_back(
            {fd, short(POLLIN | (p.output.empty() ? 0 : POLLOUT)), 0});
      int result = poll(fds.data(), fds.size(), 1000);
      if (result < 0) {
        if (errno == EINTR)
          continue;
        return false;
      }
      for (auto &f : fds) {
        if (f.fd == wake_) {
          if (f.revents & POLLIN) {
            uint64_t value;
            while (read(wake_, &value, sizeof(value)) < 0 && errno == EINTR) {
            }
            pendingUpdate_ = true;
          }
          continue;
        }
        if (f.fd == server_) {
          if (f.revents & POLLIN) {
            int peer = accept4(server_, nullptr, nullptr,
                               SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (peer >= 0) {
              ucred cred{};
              socklen_t length = sizeof(cred);
              if (peers_.size() >= 4 ||
                  getsockopt(peer, SOL_SOCKET, SO_PEERCRED, &cred, &length) ||
                  cred.uid != getuid())
                close(peer);
              else
                peers_.emplace(peer, Peer{{}, {}, tick()});
            }
          }
          continue;
        }
        bool dead = false;
        if (f.revents & POLLIN) {
          char buf[8192];
          ssize_t n = read(f.fd, buf, sizeof(buf));
          if (n > 0) {
            auto &buffer = f.fd == 0 ? input_ : peers_.at(f.fd).input;
            buffer.append(buf, n);
            if (buffer.size() > 65536) {
              if (f.fd == 0)
                return false;
              dead = true;
            } else if (!collect(f.fd, buffer)) {
              if (f.fd == 0)
                return false;
              dead = true;
            }
            if (f.fd != 0)
              peers_.at(f.fd).active = tick();
          } else if (n == 0 || (errno != EINTR && errno != EAGAIN))
            dead = true;
        }
        if (f.fd != 0 && (f.revents & POLLOUT)) {
          auto &p = peers_.at(f.fd);
          ssize_t n =
              send(f.fd, p.output.data(), p.output.size(), MSG_NOSIGNAL);
          if (n > 0) {
            p.output.erase(0, n);
            p.active = tick();
          } else if (errno != EINTR && errno != EAGAIN)
            dead = true;
        }
        if (f.revents & (POLLERR | POLLNVAL))
          dead = true;
        if ((f.revents & POLLHUP) && !(f.revents & POLLIN))
          dead = true;
        if (f.fd != 0 && tick() - peers_.at(f.fd).active > 30)
          dead = true;
        if (dead) {
          if (f.fd == 0)
            return false;
          dropPeer(f.fd);
        }
      }
    }
    auto item = std::move(ready_.front());
    ready_.pop_front();
    source = item.first;
    line = std::move(item.second);
    return true;
  }
  void reply(int source, const std::string &line) {
    if (source == STDIN_FILENO) {
      puts(line.c_str());
      fflush(stdout);
      return;
    }
    auto it = peers_.find(source);
    if (it == peers_.end())
      return;
    if (it->second.output.size() + line.size() > 4194304) {
      dropPeer(source);
      return;
    }
    it->second.output += line + '\n';
  }
};
