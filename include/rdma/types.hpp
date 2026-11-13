#pragma once
#include <cstddef>
#include <cstdint>

namespace rdma {

constexpr uint32_t kMaxMsg = 1024;
constexpr uint32_t kSendWindow = 64;
constexpr uint32_t kRecvQueueCap = 64;
constexpr uint32_t kDefaultCqDepth = 256;
constexpr uint32_t kPsnMask = 0xFFFFFFu;
constexpr int kRtoMs = 50;
constexpr int kMaxRetries = 3;

// LRP-1 操作码(自研协议,详见 PROTOCOL.md)
enum class Opcode : uint8_t {
  Send = 0x0,
  RdmaWrite = 0x1,
  RdmaReadReq = 0x2,
  RdmaReadResp = 0x3,
  AtomicCas = 0x4,
  AtomicFaa = 0x5,
  AtomicAck = 0x6,
  Ack = 0x7,
};

enum class Syndrome : uint8_t {
  Ack = 0x00,
  NakInvalid = 0x81,
  NakRnr = 0x82,
};

enum class WcStatus : uint8_t {
  Success = 0,
  LocLenErr,
  RemAccessErr,
  RnrRetryErr,
  GeneralErr,
};

enum class QpState : uint8_t { Reset, Init, Rtr, Rts, Sqd, Err };
enum class QpType : uint8_t { Rc, Uc, Ud };

enum Access : uint32_t {
  AccessLocalWrite = 1u << 0,
  AccessRemoteWrite = 1u << 1,
  AccessRemoteRead = 1u << 2,
  AccessRemoteAtomic = 1u << 3,
};

struct Sge {
  uint64_t addr = 0;
  uint32_t length = 0;
  uint32_t lkey = 0;
};

enum class WrOpcode : uint8_t { Send, RdmaWrite, RdmaRead, AtomicCas, AtomicFaa };

struct SendWr {
  uint64_t wr_id = 0;
  WrOpcode opcode = WrOpcode::Send;
  Sge sge;
  uint64_t remote_addr = 0;
  uint32_t rkey = 0;
  uint64_t compare = 0;
  uint64_t swap = 0;
};

struct RecvWr {
  uint64_t wr_id = 0;
  Sge sge;
};

struct Cqe {
  uint64_t wr_id = 0;
  WcStatus status = WcStatus::Success;
  WrOpcode opcode = WrOpcode::Send;
  uint32_t byte_len = 0;
};

inline int32_t psn_diff(uint32_t a, uint32_t b) {
  int32_t d = int32_t(((a & kPsnMask) - (b & kPsnMask)) & kPsnMask);
  return (d & 0x800000) ? d - 0x1000000 : d;
}

}  // namespace rdma
