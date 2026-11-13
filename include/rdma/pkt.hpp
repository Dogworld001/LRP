#pragma once
#include "types.hpp"
#include <cstdint>
#include <vector>

namespace rdma {

// LRP-1 线格式(自研协议,详见 PROTOCOL.md)
constexpr size_t kHdrLen = 10;
constexpr size_t kWextLen = 16;
constexpr size_t kAextLen = 6;
constexpr size_t kAtomExtLen = 28;
constexpr size_t kIcrcLen = 4;
constexpr uint8_t kProtoVer = 1;

inline void put_u8(std::vector<uint8_t>& b, uint8_t v) { b.push_back(v); }
inline void put_u16(std::vector<uint8_t>& b, uint16_t v) {
  b.push_back(uint8_t(v >> 8));
  b.push_back(uint8_t(v));
}
inline void put_u24(std::vector<uint8_t>& b, uint32_t v) {
  b.push_back(uint8_t(v >> 16));
  put_u16(b, uint16_t(v & 0xFFFF));
}
inline void put_u32(std::vector<uint8_t>& b, uint32_t v) {
  put_u16(b, uint16_t(v >> 16));
  put_u16(b, uint16_t(v));
}
inline void put_u64(std::vector<uint8_t>& b, uint64_t v) {
  put_u32(b, uint32_t(v >> 32));
  put_u32(b, uint32_t(v));
}
inline void put_bytes(std::vector<uint8_t>& b, const void* p, size_t n) {
  const uint8_t* s = static_cast<const uint8_t*>(p);
  b.insert(b.end(), s, s + n);
}

struct Reader {
  const uint8_t* p = nullptr;
  size_t n = 0;
  size_t off = 0;

  bool need(size_t k) const { return off + k <= n; }
  size_t remaining() const { return n - off; }
  uint8_t u8() { return p[off++]; }
  uint16_t u16() {
    uint16_t v = uint16_t((uint16_t(p[off]) << 8) | p[off + 1]);
    off += 2;
    return v;
  }
  uint32_t u24() {
    uint32_t v = (uint32_t(p[off]) << 16) | (uint32_t(p[off + 1]) << 8) | p[off + 2];
    off += 3;
    return v;
  }
  uint32_t u32() {
    uint32_t hi = u16();
    uint32_t lo = u16();
    return (hi << 16) | lo;
  }
  uint64_t u64() {
    uint64_t hi = u32();
    uint64_t lo = u32();
    return (hi << 32) | lo;
  }
};

struct FrameHdr {
  uint8_t op = 0;
  uint8_t flags = 0;
  uint16_t conn = 0;
  uint32_t qpn = 0;
  uint32_t seq = 0;
};

inline void put_hdr(std::vector<uint8_t>& b, const FrameHdr& h) {
  put_u8(b, uint8_t((kProtoVer << 4) | (h.op & 0x0F)));
  put_u8(b, h.flags);
  put_u16(b, h.conn);
  put_u24(b, h.qpn);
  put_u24(b, h.seq);
}

inline FrameHdr get_hdr(Reader& r) {
  FrameHdr h;
  uint8_t ver_op = r.u8();
  (void)ver_op;
  h.op = uint8_t(ver_op & 0x0F);
  h.flags = r.u8();
  h.conn = r.u16();
  h.qpn = r.u24();
  h.seq = r.u24();
  return h;
}

inline void put_wext(std::vector<uint8_t>& b, uint64_t va, uint32_t key, uint32_t len) {
  put_u64(b, va);
  put_u32(b, key);
  put_u32(b, len);
}

inline void put_ack_ext(std::vector<uint8_t>& b, uint8_t status, uint32_t msn, uint16_t credit) {
  put_u8(b, status);
  put_u24(b, msn & kPsnMask);
  put_u16(b, credit);
}

inline void put_atom_ext(std::vector<uint8_t>& b, uint64_t va, uint32_t key, uint64_t arg1,
                         uint64_t arg2) {
  put_u64(b, va);
  put_u32(b, key);
  put_u64(b, arg1);
  put_u64(b, arg2);
}

inline uint32_t crc32c(const uint8_t* data, size_t n) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    crc ^= data[i];
    for (int k = 0; k < 8; k++) {
      crc = (crc >> 1) ^ ((crc & 1u) ? 0x82F63B78u : 0u);
    }
  }
  return ~crc;
}

}  // namespace rdma
