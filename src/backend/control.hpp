// A same-user management channel. It shares the serialized engine thread with
// keyboard input, so settings never race the original dictionary
// implementation.
#pragma once
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <poll.h>
#include <string>
#include <sys/socket.h>
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
    server_ = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, "/work/control.sock");
    unlink(address.sun_path);
    mode_t mask = umask(0077);
    bool ok = server_ >= 0 &&
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
    for (auto &[fd, p] : peers_)
      close(fd);
    if (server_ >= 0) {
      close(server_);
      unlink("/work/control.sock");
    }
  }
  bool next(std::string &line, int &source) {
    while (ready_.empty()) {
      std::vector<pollfd> fds = {{STDIN_FILENO, POLLIN, 0}};
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
