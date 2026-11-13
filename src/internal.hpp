#pragma once
#include "rdma/pkt.hpp"
#include "rdma/verbs.hpp"

#include <sys/socket.h>
#include <netinet/in.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>

namespace rdma {

struct PdImpl;
struct QpImpl;

struct MrImpl {
  PdImpl* pd = nullptr;
  void* addr = nullptr;
  size_t length = 0;
  uint32_t access = 0;
  uint32_t key = 0;
};

struct PdImpl {
  ContextImpl* ctx = nullptr;
};

struct CqImpl {
  ContextImpl* ctx = nullptr;
  std::deque<Cqe> queue;
  uint32_t depth = kDefaultCqDepth;
  bool overflow = false;
};

struct PendingWr {
  uint32_t psn = 0;
  SendWr wr;
};

struct Outstanding {
  bool used = false;
  bool sent = false;
  uint32_t psn = 0;
  SendWr wr;
  std::vector<uint8_t> pkt;
  int retries = 0;
  int32_t backoff_ms = 0;
  int64_t sent_at_ms = 0;
};

struct CachedResponse {
  bool used = false;
  std::vector<uint8_t> pkt;
};

struct QpImpl {
  ContextImpl* ctx = nullptr;
  PdImpl* pd = nullptr;
  CqImpl* cq = nullptr;
  QpType type = QpType::Rc;
  uint32_t qpn = 0;
  uint32_t remote_qpn = 0;
  QpState state = QpState::Init;
  sockaddr_in peer {};
  bool peer_set = false;

  uint32_t tx_psn = 1;
  uint32_t expected_psn = 1;

  uint32_t peer_credit = 16;
  uint32_t adv_credit = 0;

  bool has_session = false;
  uint8_t session_key[32] = {0};
  uint8_t blk_key[32] = {0};
  uint16_t blk_gen_cached = 0xFFFF;
  bool blk_valid = false;
  uint64_t frames_sent = 0;

  bool ack_cached = false;
  uint32_t ack_msn = 0;
  uint16_t ack_credit = 0;
  std::vector<uint8_t> ack_pkt;

  double srtt_us = 0;
  double rttvar_us = 0;
  uint32_t rto_ms = kRtoMs;

  uint64_t conn_epoch = 1;
  uint32_t last_boundary = 0;
  std::deque<PendingWr> pending_repost;

  uint32_t rate_max_bps = 0;
  uint32_t rate_bps = 0;
  int64_t tokens = 0;
  int64_t token_ts = 0;

  Outstanding out[kSendWindow];
  CachedResponse resp_cache[kSendWindow];
  std::deque<RecvWr> rq;
};

enum class ChanType : uint8_t { Udp, Tcp };

struct ContextImpl {
  int fd = -1;
  int listen_fd = -1;
  ChanType chan = ChanType::Udp;
  std::vector<uint8_t> rxbuf;

  uint8_t psk[32] = {0};
  bool sec = false;

  std::vector<std::unique_ptr<PdImpl>> pds;
  std::vector<std::unique_ptr<MrImpl>> mrs;
  std::vector<std::unique_ptr<CqImpl>> cqs;
  std::vector<std::unique_ptr<QpImpl>> qps;

  std::vector<std::unique_ptr<ProtectionDomain>> pd_handles;
  std::vector<std::unique_ptr<MemoryRegion>> mr_handles;
  std::vector<std::unique_ptr<CompletionQueue>> cq_handles;
  std::vector<std::unique_ptr<QueuePair>> qp_handles;

  std::unordered_map<uint32_t, QpImpl*> qp_index;
  std::unordered_map<uint32_t, MrImpl*> mr_index;

  uint32_t next_qpn = 1;
  uint32_t next_key = 1;
};

int64_t now_ms();
int chan_xmit(ContextImpl* ctx, const sockaddr_in& peer, const std::vector<uint8_t>& pkt);
void handle_rx(ContextImpl* ctx, const uint8_t* data, size_t len);
int handle_housekeeping(ContextImpl* ctx);
void send_credit_update(QpImpl* qp);
void cc_on_loss(QpImpl* qp);
void cc_on_progress(QpImpl* qp);

QpImpl* find_qp(ContextImpl* ctx, uint32_t qpn);
MrImpl* find_mr(ContextImpl* ctx, uint32_t key);
bool mr_covers(const MrImpl* mr, uint64_t addr, uint32_t len);
bool cq_post(CqImpl* cq, const Cqe& cqe);

}  // namespace rdma
