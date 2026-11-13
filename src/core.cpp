#include "internal.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <cstring>
#include <vector>

#include "rdma/aead.hpp"

namespace rdma {

int64_t now_ms() {
  timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

namespace {

bool write_all(int fd, const uint8_t* p, size_t n) {
  while (n > 0) {
    ssize_t w = send(fd, p, n, 0);
    if (w <= 0) return false;
    p += w;
    n -= size_t(w);
  }
  return true;
}

}  // namespace

int chan_xmit(ContextImpl* ctx, const sockaddr_in& peer, const std::vector<uint8_t>& pkt) {
  if (ctx->chan == ChanType::Tcp) {
    if (ctx->fd < 0) return -ENOTCONN;
    uint8_t hdr[4] = {
        uint8_t(pkt.size() >> 24), uint8_t(pkt.size() >> 16), uint8_t(pkt.size() >> 8),
        uint8_t(pkt.size())};
    if (!write_all(ctx->fd, hdr, 4)) return -EIO;
    if (!write_all(ctx->fd, pkt.data(), pkt.size())) return -EIO;
    return 0;
  }
  ssize_t n = sendto(ctx->fd, pkt.data(), pkt.size(), 0,
                     reinterpret_cast<const sockaddr*>(&peer), sizeof peer);
  return n < 0 ? -errno : 0;
}

QpImpl* find_qp(ContextImpl* ctx, uint32_t qpn) {
  auto it = ctx->qp_index.find(qpn);
  return it == ctx->qp_index.end() ? nullptr : it->second;
}

MrImpl* find_mr(ContextImpl* ctx, uint32_t key) {
  auto it = ctx->mr_index.find(key);
  return it == ctx->mr_index.end() ? nullptr : it->second;
}

bool mr_covers(const MrImpl* mr, uint64_t addr, uint32_t len) {
  if (!mr) return false;
  uint64_t base = uint64_t(uintptr_t(mr->addr));
  if (addr < base) return false;
  uint64_t off = addr - base;
  return off <= mr->length && len <= mr->length - off;
}

bool cq_post(CqImpl* cq, const Cqe& cqe) {
  if (cq->queue.size() >= cq->depth) {
    cq->overflow = true;
    return false;
  }
  cq->queue.push_back(cqe);
  return true;
}

std::unique_ptr<Context> Context::open(const std::string& ip, uint16_t port) {
  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return nullptr;
  int yes = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

  sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
    close(fd);
    return nullptr;
  }
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
    close(fd);
    return nullptr;
  }

  auto* impl = new ContextImpl();
  impl->fd = fd;
  return std::unique_ptr<Context>(new Context(impl));
}

std::unique_ptr<Context> Context::open_tcp_listen(const std::string& ip, uint16_t port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return nullptr;
  int yes = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

  sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
    close(fd);
    return nullptr;
  }
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0 || listen(fd, 1) < 0) {
    close(fd);
    return nullptr;
  }
  int fl = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, fl | O_NONBLOCK);

  auto* impl = new ContextImpl();
  impl->listen_fd = fd;
  impl->chan = ChanType::Tcp;
  return std::unique_ptr<Context>(new Context(impl));
}

std::unique_ptr<Context> Context::open_tcp_connect(const std::string& ip, uint16_t port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return nullptr;

  sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
    close(fd);
    return nullptr;
  }
  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
    close(fd);
    return nullptr;
  }
  int fl = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, fl | O_NONBLOCK);

  auto* impl = new ContextImpl();
  impl->fd = fd;
  impl->chan = ChanType::Tcp;
  return std::unique_ptr<Context>(new Context(impl));
}

Context::Context(ContextImpl* impl) : impl_(impl) {}

Context::~Context() {
  if (!impl_) return;
  if (impl_->fd >= 0) close(impl_->fd);
  if (impl_->listen_fd >= 0) close(impl_->listen_fd);
  delete impl_;
}

uint16_t Context::port() const {
  int fd = impl_->fd >= 0 ? impl_->fd : impl_->listen_fd;
  if (fd < 0) return 0;
  sockaddr_in addr {};
  socklen_t len = sizeof addr;
  if (getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0) return 0;
  return ntohs(addr.sin_port);
}

int Context::progress() {
  uint8_t buf[4096];
  int events = 0;
  if (impl_->chan == ChanType::Tcp) {
    if (impl_->listen_fd >= 0 && impl_->fd < 0) {
      int c = accept(impl_->listen_fd, nullptr, nullptr);
      if (c >= 0) {
        int fl = fcntl(c, F_GETFL, 0);
        fcntl(c, F_SETFL, fl | O_NONBLOCK);
        impl_->fd = c;
        ++events;
      }
    }
    if (impl_->fd >= 0) {
      ssize_t n;
      while ((n = recv(impl_->fd, buf, sizeof buf, MSG_DONTWAIT)) > 0) {
        impl_->rxbuf.insert(impl_->rxbuf.end(), buf, buf + n);
      }
      while (impl_->rxbuf.size() >= 4) {
        uint32_t flen = (uint32_t(impl_->rxbuf[0]) << 24) | (uint32_t(impl_->rxbuf[1]) << 16) |
                        (uint32_t(impl_->rxbuf[2]) << 8) | impl_->rxbuf[3];
        if (flen == 0 || flen > 65535) {
          impl_->rxbuf.clear();
          break;
        }
        if (impl_->rxbuf.size() < 4 + flen) break;
        handle_rx(impl_, impl_->rxbuf.data() + 4, flen);
        impl_->rxbuf.erase(impl_->rxbuf.begin(), impl_->rxbuf.begin() + 4 + flen);
        ++events;
      }
    }
    events += handle_housekeeping(impl_);
    return events;
  }

  ssize_t n;
  while ((n = recvfrom(impl_->fd, buf, sizeof buf, MSG_DONTWAIT, nullptr, nullptr)) > 0) {
    handle_rx(impl_, buf, size_t(n));
    ++events;
  }
  events += handle_housekeeping(impl_);
  return events;
}

ProtectionDomain* Context::alloc_pd() {
  auto pd = std::unique_ptr<PdImpl>(new PdImpl());
  pd->ctx = impl_;
  impl_->pds.push_back(std::move(pd));
  auto h = std::unique_ptr<ProtectionDomain>(new ProtectionDomain(impl_->pds.back().get()));
  auto* raw = h.get();
  impl_->pd_handles.push_back(std::move(h));
  return raw;
}

CompletionQueue* Context::create_cq(uint32_t depth) {
  auto cq = std::unique_ptr<CqImpl>(new CqImpl());
  cq->ctx = impl_;
  cq->depth = depth ? depth : kDefaultCqDepth;
  impl_->cqs.push_back(std::move(cq));
  auto h = std::unique_ptr<CompletionQueue>(new CompletionQueue(impl_->cqs.back().get()));
  auto* raw = h.get();
  impl_->cq_handles.push_back(std::move(h));
  return raw;
}

QueuePair* Context::create_qp(ProtectionDomain* pd, CompletionQueue* cq, QpType type) {
  if (!pd || !cq) return nullptr;
  auto qp = std::unique_ptr<QpImpl>(new QpImpl());
  qp->ctx = impl_;
  qp->pd = pd->impl();
  qp->cq = cq->impl();
  qp->type = type;
  qp->qpn = impl_->next_qpn++;
  qp->state = QpState::Init;
  // 连接纪元按开机/创建时刻随机化:对端重启后 epoch 必变化,避免序号空间错配
  qp->conn_epoch = (uint64_t(now_ms()) << 24) ^ (uint64_t(uintptr_t(qp.get())) * 0x9e3779b97f4a7c15ull);
  QpImpl* raw_qp = qp.get();
  impl_->qp_index[raw_qp->qpn] = raw_qp;
  impl_->qps.push_back(std::move(qp));
  auto h = std::unique_ptr<QueuePair>(new QueuePair(raw_qp));
  auto* raw = h.get();
  impl_->qp_handles.push_back(std::move(h));
  return raw;
}

MemoryRegion* ProtectionDomain::reg_mr(void* addr, size_t length, uint32_t access) {
  if (!addr || !length || !impl_) return nullptr;
  auto mr = std::unique_ptr<MrImpl>(new MrImpl());
  mr->pd = impl_;
  mr->addr = addr;
  mr->length = length;
  mr->access = access;
  mr->key = impl_->ctx->next_key++;
  MrImpl* raw_mr = mr.get();
  impl_->ctx->mr_index[raw_mr->key] = raw_mr;
  impl_->ctx->mrs.push_back(std::move(mr));
  auto h = std::unique_ptr<MemoryRegion>(new MemoryRegion(raw_mr));
  auto* raw = h.get();
  impl_->ctx->mr_handles.push_back(std::move(h));
  return raw;
}

uint32_t MemoryRegion::lkey() const { return impl_->key; }
uint32_t MemoryRegion::rkey() const { return impl_->key; }
void* MemoryRegion::addr() const { return impl_->addr; }
size_t MemoryRegion::length() const { return impl_->length; }

int CompletionQueue::poll(Cqe* out, int max) {
  int cnt = 0;
  while (cnt < max && !impl_->queue.empty()) {
    out[cnt++] = impl_->queue.front();
    impl_->queue.pop_front();
  }
  return cnt;
}

bool CompletionQueue::overflowed() const { return impl_->overflow; }

uint32_t QueuePair::qpn() const { return impl_->qpn; }
QpState QueuePair::state() const { return impl_->state; }

// 闪断恢复(对端仍存活):不清序号,在途帧按原 psn 立即重传,
// 依靠对端重复抑制/响应缓存完成续传(读写安全,原子不重复执行)
int QueuePair::recover_rearm() {
  QpImpl* qp = impl_;
  if (qp->state == QpState::Err) qp->state = QpState::Rts;
  int64_t due = now_ms() - int64_t(qp->rto_ms) - 1;
  for (uint32_t i = 0; i < kSendWindow; i++) {
    Outstanding& o = qp->out[i];
    if (!o.used) continue;
    o.sent = true;
    o.sent_at_ms = due;
    o.retries = 0;
  }
  return 0;
}

// 重启路径(对端状态丢失):清序号开新连接,在途操作按 SRRESUME 边界分类续传
int QueuePair::reset() {
  QpImpl* qp = impl_;
  qp->last_boundary = (qp->expected_psn - 1) & kPsnMask;
  for (uint32_t i = 0; i < kSendWindow; i++) {
    Outstanding& o = qp->out[i];
    if (!o.used) continue;
    qp->pending_repost.push_back(PendingWr {o.psn, o.wr});
    o.used = false;
    o.sent = false;
  }
  qp->tx_psn = 1;
  qp->expected_psn = 1;
  qp->peer_credit = 16;
  qp->adv_credit = 0;
  qp->ack_cached = false;
  qp->blk_valid = false;
  qp->frames_sent = 0;
  qp->state = QpState::Init;
  return 0;
}

int QueuePair::apply_resume_boundary(uint32_t boundary) {
  QpImpl* qp = impl_;
  std::deque<PendingWr> keep;
  while (!qp->pending_repost.empty()) {
    PendingWr e = qp->pending_repost.front();
    qp->pending_repost.pop_front();
    bool done_at_peer = psn_diff(e.psn, boundary) <= 0;
    if (!done_at_peer) {
      keep.push_back(e);
      continue;
    }
    Cqe c;
    c.wr_id = e.wr.wr_id;
    c.opcode = e.wr.opcode;
    if (e.wr.opcode == WrOpcode::RdmaRead) {
      keep.push_back(e);  // 对端已执行但结果丢失:读幂等,重投
      continue;
    }
    if (e.wr.opcode == WrOpcode::AtomicCas || e.wr.opcode == WrOpcode::AtomicFaa) {
      c.status = WcStatus::GeneralErr;  // 结果不确定,是否重试由应用决定
      c.byte_len = 0;
    } else {
      c.status = WcStatus::Success;
      c.byte_len = e.wr.sge.length;
    }
    cq_post(qp->cq, c);
  }
  qp->pending_repost = keep;
  return 0;
}

uint32_t QueuePair::rto_ms() const { return impl_->rto_ms; }
uint64_t QueuePair::conn_epoch() const { return impl_->conn_epoch; }
uint32_t QueuePair::resume_boundary() const { return (impl_->expected_psn - 1) & kPsnMask; }

int QueuePair::connect(const std::string& ip, uint16_t port, uint32_t remote_qpn,
                       uint32_t peer_credit) {
  if (!impl_ || impl_->state == QpState::Err) return -EINVAL;
  sockaddr_in peer {};
  peer.sin_family = AF_INET;
  peer.sin_port = htons(port);
  if (inet_pton(AF_INET, ip.c_str(), &peer.sin_addr) != 1) return -EINVAL;
  impl_->peer = peer;
  impl_->peer_set = true;
  impl_->remote_qpn = remote_qpn;
  impl_->peer_credit = peer_credit;
  impl_->state = QpState::Rts;
  while (!impl_->pending_repost.empty()) {
    SendWr wr = impl_->pending_repost.front().wr;
    impl_->pending_repost.pop_front();
    post_send(wr);
  }
  return 0;
}

int QueuePair::post_recv(const RecvWr& wr) {
  QpImpl* qp = impl_;
  if (qp->state == QpState::Reset || qp->state == QpState::Err) return -EINVAL;
  if (wr.sge.length > kMaxMsg) return -EINVAL;
  if (qp->rq.size() >= kRecvQueueCap) return -EAGAIN;
  MrImpl* mr = find_mr(qp->ctx, wr.sge.lkey);
  if (!mr_covers(mr, wr.sge.addr, wr.sge.length)) return -EINVAL;
  qp->rq.push_back(wr);
  if (qp->adv_credit == 0 && qp->peer_set) send_credit_update(qp);
  return 0;
}

uint32_t QueuePair::recv_credits() const { return uint32_t(impl_->rq.size()); }

int QueuePair::set_rate_limit(uint32_t kbps) {
  impl_->rate_max_bps = kbps * 128;
  impl_->rate_bps = impl_->rate_max_bps;
  impl_->tokens = 4096;
  impl_->token_ts = now_ms();
  return 0;
}

int QueuePair::derive_session_key(const void* rnd_a, const void* rnd_b, size_t len) {
  if (!rnd_a || !rnd_b || len == 0 || !impl_->ctx->sec) return -EINVAL;
  // 会话密钥 = KDF(PSK, 两端随机会话因子,按序拼接保证双向一致)
  const uint8_t* a = static_cast<const uint8_t*>(rnd_a);
  const uint8_t* b = static_cast<const uint8_t*>(rnd_b);
  int cmp = std::memcmp(a, b, len);
  std::vector<uint8_t> ctx_buf;
  if (cmp <= 0) {
    ctx_buf.assign(a, a + len);
    ctx_buf.insert(ctx_buf.end(), b, b + len);
  } else {
    ctx_buf.assign(b, b + len);
    ctx_buf.insert(ctx_buf.end(), a, a + len);
  }
  rdma::aead::kdf(impl_->ctx->psk, sizeof impl_->ctx->psk, "LRP-sec-2 session", ctx_buf.data(),
                  ctx_buf.size(), impl_->session_key, sizeof impl_->session_key);
  impl_->has_session = true;
  impl_->blk_valid = false;
  return 0;
}

void Context::set_psk(const void* key, size_t len) {
  if (!key || len == 0) {
    impl_->sec = false;
    return;
  }
  std::memset(impl_->psk, 0, sizeof impl_->psk);
  const uint8_t* k = static_cast<const uint8_t*>(key);
  for (size_t i = 0; i < 32; i++) impl_->psk[i] = k[i % len];
  impl_->sec = true;
  for (auto& qpo : impl_->qps) {
    qpo->blk_valid = false;
    qpo->has_session = false;
    qpo->ack_cached = false;
  }
}

}  // namespace rdma
