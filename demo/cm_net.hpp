#pragma once
// CM 控制面握手:交换 QPN / rkey / 内存基址(demo 与 bench 共用)

#include <arpa/inet.h>
#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#if defined(ESP_PLATFORM)
#include <esp_random.h>
#endif

namespace cm {

// 8 字节对齐分配:ESP32 的 malloc 仅保证 4 对齐,而原子操作要求 va 严格 8 对齐
inline uint64_t* aligned_u64_alloc(size_t count) {
  void* p = nullptr;
  if (posix_memalign(&p, 8, count * sizeof(uint64_t)) != 0) return nullptr;
  std::memset(p, 0, count * sizeof(uint64_t));
  return static_cast<uint64_t*>(p);
}

inline void make_rnd16(uint8_t out[16]) {
#if defined(ESP_PLATFORM)
  esp_fill_random(out, 16);
#else
  FILE* f = std::fopen("/dev/urandom", "rb");
  if (f) {
    size_t n = std::fread(out, 1, 16, f);
    std::fclose(f);
    if (n == 16) return;
  }
  uint64_t mix = uint64_t(reinterpret_cast<uintptr_t>(out)) * 0x9e3779b97f4a7c15ull;
  for (int i = 0; i < 16; i++) {
    mix ^= mix << 13;
    mix ^= mix >> 7;
    mix ^= mix << 17;
    out[i] = uint8_t(mix >> (8 * (i % 8)));
  }
#endif
}

inline void rnd_hex(const uint8_t r[16], char out[33]) {
  static const char* d = "0123456789abcdef";
  for (int i = 0; i < 16; i++) {
    out[2 * i] = d[r[i] >> 4];
    out[2 * i + 1] = d[r[i] & 15];
  }
  out[32] = 0;
}

inline bool parse_rnd_hex(const char* s, uint8_t out[16]) {
  for (int i = 0; i < 16; i++) {
    unsigned v;
    if (std::sscanf(s + 2 * i, "%2x", &v) != 1) return false;
    out[i] = uint8_t(v);
  }
  return true;
}

inline int ctrl_bind(uint16_t port) {
  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return -1;
  int yes = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
  sockaddr_in a {};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0) {
    std::fprintf(stderr, "[CM] 控制端口 %u 绑定失败: %s\n", port, std::strerror(errno));
    close(fd);
    return -1;
  }
  timeval tv {2, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  return fd;
}

inline bool target_handshake(int fd, uint32_t rkey, uint64_t base, uint32_t qpn, uint32_t credit,
                             uint32_t* init_qpn, uint8_t rnd_local[16], uint8_t rnd_peer[16],
                             char peer_ip[64]) {
  make_rnd16(rnd_local);
  char my_hex[33];
  rnd_hex(rnd_local, my_hex);
  char buf[224];
  for (int attempt = 0; attempt < 15; attempt++) {
    sockaddr_in from {};
    socklen_t fl = sizeof from;
    ssize_t n = recvfrom(fd, buf, sizeof buf - 1, 0, reinterpret_cast<sockaddr*>(&from), &fl);
    if (n <= 0) continue;
    buf[n] = 0;
    uint32_t peer_qpn = 0;
    char peer_hex[33] = {0};
    int got = std::sscanf(buf, "SRHELLO qpn=%u rnd=%32s", &peer_qpn, peer_hex);
    if (got >= 1) {
      if (got >= 2) parse_rnd_hex(peer_hex, rnd_peer);
      else std::memset(rnd_peer, 0, 16);
      char reply[256];
      int m = std::snprintf(reply, sizeof reply,
                            "SRWELCOME qpn=%u rkey=%u base=%llu credit=%u rnd=%s", qpn, rkey,
                            static_cast<unsigned long long>(base), credit, my_hex);
      sendto(fd, reply, size_t(m), 0, reinterpret_cast<sockaddr*>(&from), fl);
      *init_qpn = peer_qpn;
      inet_ntop(AF_INET, &from.sin_addr, peer_ip, 64);
      return true;
    }
  }
  return false;
}

inline bool initiator_handshake(const char* peer_ip, uint16_t peer_port, uint32_t my_qpn,
                                uint32_t* tgt_qpn, uint32_t* rkey, uint64_t* base,
                                uint32_t* credit, uint8_t rnd_local[16], uint8_t rnd_peer[16]) {
  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return false;
  timeval tv {2, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

  sockaddr_in to {};
  to.sin_family = AF_INET;
  to.sin_port = htons(uint16_t(peer_port + 100));
  inet_pton(AF_INET, peer_ip, &to.sin_addr);

  make_rnd16(rnd_local);
  char my_hex[33];
  rnd_hex(rnd_local, my_hex);
  char msg[112];
  int m = std::snprintf(msg, sizeof msg, "SRHELLO qpn=%u rnd=%s", my_qpn, my_hex);
  char buf[256];
  for (int attempt = 0; attempt < 10; attempt++) {
    sendto(fd, msg, size_t(m), 0, reinterpret_cast<sockaddr*>(&to), sizeof to);
    ssize_t n = recvfrom(fd, buf, sizeof buf - 1, 0, nullptr, nullptr);
    if (n <= 0) continue;
    buf[n] = 0;
    unsigned tq = 0, rk = 0, cr = 16;
    unsigned long long b = 0;
    char peer_hex[33] = {0};
    int got = std::sscanf(buf, "SRWELCOME qpn=%u rkey=%u base=%llu credit=%u rnd=%32s", &tq, &rk,
                          &b, &cr, peer_hex);
    if (got >= 3) {
      *tgt_qpn = tq;
      *rkey = rk;
      *base = b;
      *credit = (got >= 4) ? cr : 16;
      if (got >= 5) parse_rnd_hex(peer_hex, rnd_peer);
      else std::memset(rnd_peer, 0, 16);
      close(fd);
      return true;
    }
  }
  close(fd);
  return false;
}

}  // namespace cm
