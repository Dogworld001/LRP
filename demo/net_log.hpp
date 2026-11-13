#pragma once
// UDP 遥测日志:把关键输出镜像到收集端(默认广播,同一 WiFi 下电脑即收,无需连线)
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace netlog {

constexpr uint16_t kLogPort = 18999;

struct Logger {
  int fd = -1;
  sockaddr_in dst {};
  bool on = false;

  void init(const char* ip, uint16_t port = kLogPort) {
    if (!ip || !*ip || std::strcmp(ip, "off") == 0) return;
    fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof yes);
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &dst.sin_addr) != 1) return;
    on = true;
  }

  void send(const char* line) {
    if (!on) return;
    sendto(fd, line, std::strlen(line), 0, reinterpret_cast<sockaddr*>(&dst), sizeof dst);
  }
};

inline Logger& logger() {
  static Logger l;
  return l;
}

inline void init(const char* ip, uint16_t port = kLogPort) { logger().init(ip, port); }

// printf 兼容:stdout 同步打印 + UDP 回传
inline void say(const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (n <= 0) return;
  std::fputs(buf, stdout);
  size_t len = std::strlen(buf);
  if (len == 0 || buf[len - 1] != '\n') std::fputc('\n', stdout);
  logger().send(buf);
}

}  // namespace netlog
