#pragma once
#include "types.hpp"
#include <memory>
#include <string>

namespace rdma {

struct ContextImpl;
struct PdImpl;
struct MrImpl;
struct CqImpl;
struct QpImpl;

class MemoryRegion {
 public:
  uint32_t lkey() const;
  uint32_t rkey() const;
  void* addr() const;
  size_t length() const;
  explicit MemoryRegion(MrImpl* impl) : impl_(impl) {}

 private:
  MrImpl* impl_;
};

class ProtectionDomain {
 public:
  MemoryRegion* reg_mr(void* addr, size_t length, uint32_t access);
  explicit ProtectionDomain(PdImpl* impl) : impl_(impl) {}
  PdImpl* impl() const { return impl_; }

 private:
  PdImpl* impl_;
};

class CompletionQueue {
 public:
  int poll(Cqe* out, int max);
  bool overflowed() const;
  explicit CompletionQueue(CqImpl* impl) : impl_(impl) {}
  CqImpl* impl() const { return impl_; }

 private:
  CqImpl* impl_;
};

class QueuePair {
 public:
  uint32_t qpn() const;
  QpState state() const;
  int connect(const std::string& ip, uint16_t port, uint32_t remote_qpn, uint32_t peer_credit = 16);
  int post_send(const SendWr& wr);
  int post_recv(const RecvWr& wr);
  int set_rate_limit(uint32_t kbps);
  uint32_t recv_credits() const;
  int derive_session_key(const void* rnd_a, const void* rnd_b, size_t len);
  int reset();
  int recover_rearm();
  int apply_resume_boundary(uint32_t boundary);
  uint32_t rto_ms() const;
  uint64_t conn_epoch() const;
  uint32_t resume_boundary() const;
  explicit QueuePair(QpImpl* impl) : impl_(impl) {}
  QpImpl* impl() const { return impl_; }

 private:
  QpImpl* impl_;
};

class Context {
 public:
  static std::unique_ptr<Context> open(const std::string& ip, uint16_t port);
  static std::unique_ptr<Context> open_tcp_listen(const std::string& ip, uint16_t port);
  static std::unique_ptr<Context> open_tcp_connect(const std::string& ip, uint16_t port);
  ~Context();

  ProtectionDomain* alloc_pd();
  CompletionQueue* create_cq(uint32_t depth);
  QueuePair* create_qp(ProtectionDomain* pd, CompletionQueue* cq, QpType type);
  int progress();
  uint16_t port() const;
  void set_psk(const void* key, size_t len);

  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;

  ContextImpl* impl() const { return impl_; }

 private:
  explicit Context(ContextImpl* impl);
  ContextImpl* impl_;
};

}  // namespace rdma
