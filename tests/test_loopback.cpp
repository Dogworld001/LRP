#include "rdma/aead.hpp"
#include "rdma/verbs.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <thread>
#include <vector>

using namespace rdma;

static int g_failures = 0;

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                \
      ++g_failures;                                                              \
    }                                                                            \
  } while (0)

static bool pump_until(Context& a, Context& b, const std::function<bool()>& done,
                       int timeout_ms = 3000) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    a.progress();
    b.progress();
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  return false;
}

struct Fixture {
  std::unique_ptr<Context> a, b;
  ProtectionDomain* pda = nullptr;
  ProtectionDomain* pdb = nullptr;
  CompletionQueue* cqa = nullptr;
  CompletionQueue* cqb = nullptr;
  QueuePair* qpa = nullptr;
  QueuePair* qpb = nullptr;
  MemoryRegion* mra = nullptr;
  MemoryRegion* mrb = nullptr;
  alignas(8) char src[1024] {};
  alignas(8) char dst[1024] {};

  bool setup(uint32_t b_access, bool tcp = false, const char* psk = nullptr,
             uint32_t peer_credit = 16) {
    if (tcp) {
      a = Context::open_tcp_listen("127.0.0.1", 0);
      if (!a) return false;
      b = Context::open_tcp_connect("127.0.0.1", a->port());
    } else {
      a = Context::open("127.0.0.1", 0);
      b = Context::open("127.0.0.1", 0);
    }
    if (!a || !b) return false;
    if (psk) {
      a->set_psk(psk, std::strlen(psk));
      b->set_psk(psk, std::strlen(psk));
    }
    pda = a->alloc_pd();
    pdb = b->alloc_pd();
    cqa = a->create_cq(256);
    cqb = b->create_cq(256);
    mra = pda->reg_mr(src, sizeof src, AccessLocalWrite);
    mrb = pdb->reg_mr(dst, sizeof dst, b_access);
    qpa = a->create_qp(pda, cqa, QpType::Rc);
    qpb = b->create_qp(pdb, cqb, QpType::Rc);
    if (!mra || !mrb || !qpa || !qpb) return false;
    if (qpa->connect("127.0.0.1", b->port(), qpb->qpn(), peer_credit) != 0) return false;
    if (qpb->connect("127.0.0.1", a->port(), qpa->qpn(), peer_credit) != 0) return false;
    return true;
  }
};

static void test_rdma_write() {
  std::printf("test_rdma_write\n");
  Fixture f;
  CHECK(f.setup(AccessRemoteWrite | AccessRemoteRead | AccessRemoteAtomic));
  std::memset(f.src, 0xAB, 128);
  std::memset(f.dst, 0, 128);

  SendWr wr;
  wr.wr_id = 101;
  wr.opcode = WrOpcode::RdmaWrite;
  wr.sge = {uint64_t(uintptr_t(f.src)), 128, f.mra->lkey()};
  wr.remote_addr = uint64_t(uintptr_t(f.dst));
  wr.rkey = f.mrb->rkey();
  CHECK(f.qpa->post_send(wr) == 0);

  Cqe c;
  bool ok = pump_until(*f.a, *f.b, [&] { return f.cqa->poll(&c, 1) > 0; });
  CHECK(ok);
  CHECK(c.wr_id == 101);
  CHECK(c.status == WcStatus::Success);
  CHECK(c.byte_len == 128);
  CHECK(std::memcmp(f.src, f.dst, 128) == 0);

  Cqe none;
  CHECK(f.cqb->poll(&none, 1) == 0);
}

static void test_send_recv() {
  std::printf("test_send_recv\n");
  Fixture f;
  CHECK(f.setup(AccessRemoteWrite));

  RecvWr rv;
  rv.wr_id = 3;
  rv.sge = {uint64_t(uintptr_t(f.dst)), 256, f.mrb->lkey()};
  CHECK(f.qpb->post_recv(rv) == 0);

  std::memset(f.src, 0, sizeof f.src);
  std::strcpy(f.src, "hello rdma");
  SendWr wr;
  wr.wr_id = 5;
  wr.opcode = WrOpcode::Send;
  wr.sge = {uint64_t(uintptr_t(f.src)), 32, f.mra->lkey()};
  CHECK(f.qpa->post_send(wr) == 0);

  Cqe cs, cr;
  bool ok = pump_until(*f.a, *f.b,
                       [&] { return f.cqa->poll(&cs, 1) > 0 && f.cqb->poll(&cr, 1) > 0; });
  CHECK(ok);
  CHECK(cs.wr_id == 5 && cs.status == WcStatus::Success && cs.byte_len == 32);
  CHECK(cr.wr_id == 3 && cr.status == WcStatus::Success && cr.byte_len == 32);
  CHECK(std::memcmp(f.src, f.dst, 32) == 0);
}

static void test_rdma_read() {
  std::printf("test_rdma_read\n");
  Fixture f;
  CHECK(f.setup(AccessRemoteRead | AccessRemoteWrite));
  for (int i = 0; i < 64; i++) f.dst[i] = char(i + 1);
  std::memset(f.src, 0, 64);

  SendWr wr;
  wr.wr_id = 201;
  wr.opcode = WrOpcode::RdmaRead;
  wr.sge = {uint64_t(uintptr_t(f.src)), 64, f.mra->lkey()};
  wr.remote_addr = uint64_t(uintptr_t(f.dst));
  wr.rkey = f.mrb->rkey();
  CHECK(f.qpa->post_send(wr) == 0);

  Cqe c;
  bool ok = pump_until(*f.a, *f.b, [&] { return f.cqa->poll(&c, 1) > 0; });
  CHECK(ok);
  CHECK(c.wr_id == 201 && c.status == WcStatus::Success && c.byte_len == 64);
  CHECK(std::memcmp(f.src, f.dst, 64) == 0);
}

static void test_atomic_cas() {
  std::printf("test_atomic_cas\n");
  Fixture f;
  CHECK(f.setup(AccessRemoteAtomic | AccessRemoteWrite));

  auto* slot = reinterpret_cast<uint64_t*>(f.dst);
  auto* orig_slot = reinterpret_cast<uint64_t*>(f.src);
  *slot = 5;

  SendWr wr;
  wr.wr_id = 301;
  wr.opcode = WrOpcode::AtomicCas;
  wr.sge = {uint64_t(uintptr_t(f.src)), 8, f.mra->lkey()};
  wr.remote_addr = uint64_t(uintptr_t(f.dst));
  wr.rkey = f.mrb->rkey();
  wr.compare = 5;
  wr.swap = 7;
  CHECK(f.qpa->post_send(wr) == 0);

  Cqe c;
  bool ok = pump_until(*f.a, *f.b, [&] { return f.cqa->poll(&c, 1) > 0; });
  CHECK(ok);
  CHECK(c.status == WcStatus::Success);
  CHECK(*orig_slot == 5);
  CHECK(*slot == 7);

  wr.wr_id = 302;
  wr.compare = 5;
  wr.swap = 9;
  *orig_slot = 0;
  CHECK(f.qpa->post_send(wr) == 0);
  ok = pump_until(*f.a, *f.b, [&] { return f.cqa->poll(&c, 1) > 0; });
  CHECK(ok);
  CHECK(*orig_slot == 7);
  CHECK(*slot == 7);
}

static void test_atomic_faa() {
  std::printf("test_atomic_faa\n");
  Fixture f;
  CHECK(f.setup(AccessRemoteAtomic | AccessRemoteWrite));

  auto* slot = reinterpret_cast<uint64_t*>(f.dst);
  auto* orig_slot = reinterpret_cast<uint64_t*>(f.src);
  *slot = 10;

  SendWr wr;
  wr.wr_id = 401;
  wr.opcode = WrOpcode::AtomicFaa;
  wr.sge = {uint64_t(uintptr_t(f.src)), 8, f.mra->lkey()};
  wr.remote_addr = uint64_t(uintptr_t(f.dst));
  wr.rkey = f.mrb->rkey();
  wr.swap = 3;
  CHECK(f.qpa->post_send(wr) == 0);

  Cqe c;
  bool ok = pump_until(*f.a, *f.b, [&] { return f.cqa->poll(&c, 1) > 0; });
  CHECK(ok);
  CHECK(*orig_slot == 10);
  CHECK(*slot == 13);
}

static void test_nak_invalid_then_alive() {
  std::printf("test_nak_invalid_then_alive\n");
  Fixture f;
  CHECK(f.setup(AccessLocalWrite));

  std::memset(f.src, 0x11, 64);
  SendWr wr;
  wr.wr_id = 501;
  wr.opcode = WrOpcode::RdmaWrite;
  wr.sge = {uint64_t(uintptr_t(f.src)), 64, f.mra->lkey()};
  wr.remote_addr = uint64_t(uintptr_t(f.dst));
  wr.rkey = f.mrb->rkey();
  CHECK(f.qpa->post_send(wr) == 0);

  Cqe c;
  bool ok = pump_until(*f.a, *f.b, [&] { return f.cqa->poll(&c, 1) > 0; });
  CHECK(ok);
  CHECK(c.wr_id == 501 && c.status == WcStatus::RemAccessErr);

  RecvWr rv;
  rv.wr_id = 601;
  rv.sge = {uint64_t(uintptr_t(f.dst)), 64, f.mrb->lkey()};
  CHECK(f.qpb->post_recv(rv) == 0);
  SendWr ws;
  ws.wr_id = 602;
  ws.opcode = WrOpcode::Send;
  ws.sge = {uint64_t(uintptr_t(f.src)), 16, f.mra->lkey()};
  CHECK(f.qpa->post_send(ws) == 0);

  Cqe cs, cr;
  ok = pump_until(*f.a, *f.b,
                  [&] { return f.cqa->poll(&cs, 1) > 0 && f.cqb->poll(&cr, 1) > 0; });
  CHECK(ok);
  CHECK(cs.wr_id == 602 && cs.status == WcStatus::Success);
  CHECK(cr.wr_id == 601 && cr.status == WcStatus::Success);
}

static void test_rnr_retry() {
  std::printf("test_rnr_retry\n");
  Fixture f;
  CHECK(f.setup(AccessRemoteWrite));

  std::strcpy(f.src, "rnr payload");
  SendWr wr;
  wr.wr_id = 701;
  wr.opcode = WrOpcode::Send;
  wr.sge = {uint64_t(uintptr_t(f.src)), 16, f.mra->lkey()};
  CHECK(f.qpa->post_send(wr) == 0);

  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  f.a->progress();
  f.b->progress();

  RecvWr rv;
  rv.wr_id = 702;
  rv.sge = {uint64_t(uintptr_t(f.dst)), 32, f.mrb->lkey()};
  CHECK(f.qpb->post_recv(rv) == 0);

  Cqe cs, cr;
  bool ok = pump_until(*f.a, *f.b,
                       [&] { return f.cqa->poll(&cs, 1) > 0 && f.cqb->poll(&cr, 1) > 0; });
  CHECK(ok);
  CHECK(cs.wr_id == 701 && cs.status == WcStatus::Success);
  CHECK(cr.wr_id == 702 && cr.status == WcStatus::Success);
  CHECK(std::memcmp(f.src, f.dst, 16) == 0);
}

static void test_tcp_channel() {
  std::printf("test_tcp_channel\n");
  Fixture f;
  CHECK(f.setup(AccessRemoteWrite | AccessRemoteRead | AccessRemoteAtomic, true));

  for (int i = 0; i < 100; i++) {
    f.a->progress();
    f.b->progress();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  std::memset(f.src, 0x5A, 128);
  std::memset(f.dst, 0, 128);
  SendWr wr;
  wr.wr_id = 801;
  wr.opcode = WrOpcode::RdmaWrite;
  wr.sge = {uint64_t(uintptr_t(f.src)), 128, f.mra->lkey()};
  wr.remote_addr = uint64_t(uintptr_t(f.dst));
  wr.rkey = f.mrb->rkey();
  CHECK(f.qpa->post_send(wr) == 0);
  Cqe c;
  bool ok = pump_until(*f.a, *f.b, [&] { return f.cqa->poll(&c, 1) > 0; });
  CHECK(ok && c.wr_id == 801 && c.status == WcStatus::Success);
  CHECK(std::memcmp(f.src, f.dst, 128) == 0);

  RecvWr rv;
  rv.wr_id = 802;
  rv.sge = {uint64_t(uintptr_t(f.dst)), 256, f.mrb->lkey()};
  CHECK(f.qpb->post_recv(rv) == 0);
  wr.wr_id = 803;
  wr.opcode = WrOpcode::Send;
  wr.sge = {uint64_t(uintptr_t(f.src)), 64, f.mra->lkey()};
  CHECK(f.qpa->post_send(wr) == 0);
  Cqe cs, cr;
  ok = pump_until(*f.a, *f.b,
                  [&] { return f.cqa->poll(&cs, 1) > 0 && f.cqb->poll(&cr, 1) > 0; });
  CHECK(ok && cs.wr_id == 803 && cr.wr_id == 802 && cr.byte_len == 64);

  std::memset(f.src, 0, 64);
  wr.wr_id = 804;
  wr.opcode = WrOpcode::RdmaRead;
  wr.sge = {uint64_t(uintptr_t(f.src)), 64, f.mra->lkey()};
  CHECK(f.qpa->post_send(wr) == 0);
  ok = pump_until(*f.a, *f.b, [&] { return f.cqa->poll(&c, 1) > 0; });
  CHECK(ok && c.wr_id == 804 && c.status == WcStatus::Success);
  CHECK(std::memcmp(f.src, f.dst, 64) == 0);

  auto* slot = reinterpret_cast<uint64_t*>(f.dst);
  auto* orig_slot = reinterpret_cast<uint64_t*>(f.src);
  *slot = 5;
  wr.wr_id = 805;
  wr.opcode = WrOpcode::AtomicCas;
  wr.sge = {uint64_t(uintptr_t(f.src)), 8, f.mra->lkey()};
  wr.remote_addr = uint64_t(uintptr_t(f.dst));
  wr.rkey = f.mrb->rkey();
  wr.compare = 5;
  wr.swap = 7;
  CHECK(f.qpa->post_send(wr) == 0);
  ok = pump_until(*f.a, *f.b, [&] { return f.cqa->poll(&c, 1) > 0; });
  CHECK(ok && *orig_slot == 5 && *slot == 7);
}

static std::vector<uint8_t> hex2bin(const char* s) {
  std::vector<uint8_t> v;
  unsigned b;
  while (*s) {
    if (*s == ' ' || *s == ':') {
      ++s;
      continue;
    }
    std::sscanf(s, "%2x", &b);
    v.push_back(uint8_t(b));
    s += 2;
  }
  return v;
}

static void test_aead_kat() {
  std::printf("test_aead_kat\n");
  using namespace rdma::aead;
  {
    auto k = hex2bin("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    auto p = hex2bin("00112233445566778899aabbccddeeff");
    uint8_t c[16];
    aes_encrypt_block(k.data(), 32, p.data(), c);
    auto w = hex2bin("8ea2b7ca516745bfeafc49904b496089");
    CHECK(std::memcmp(c, w.data(), 16) == 0);
  }
  {
    auto k = hex2bin("000102030405060708090a0b0c0d0e0f");
    auto p = hex2bin("00112233445566778899aabbccddeeff");
    uint8_t c[16];
    aes_encrypt_block(k.data(), 16, p.data(), c);
    auto w = hex2bin("69c4e0d86a7b0430d8cdb78070b4c55a");
    CHECK(std::memcmp(c, w.data(), 16) == 0);
  }
  {
    auto k = hex2bin("2b7e151628aed2a6abf7158809cf4f3c");
    auto m = hex2bin("6bc1bee22e409f96e93d7e117393172a");
    uint8_t t[16];
    cmac(k.data(), 16, m.data(), 16, t);
    auto w = hex2bin("070a16b46b4d4144f79bdd9dd04a287c");
    CHECK(std::memcmp(t, w.data(), 16) == 0);
  }
  {
    uint8_t k[32] = {0}, iv[12] = {0}, tag[16];
    seal(k, iv, nullptr, 0, nullptr, 0, nullptr, tag);
    auto wt = hex2bin("530f8afbc74536b9a963b4f1c4cb738b");
    CHECK(std::memcmp(tag, wt.data(), 16) == 0);
  }
  {
    uint8_t k[32] = {0}, iv[12] = {0}, pt[16] = {0}, ct[16], tag[16];
    seal(k, iv, nullptr, 0, pt, 16, ct, tag);
    auto wc = hex2bin("cea7403d4d606b6e074ec5d3baf39d18");
    auto wt = hex2bin("d0d1c8a799996bf0265b98b5d48ab919");
    CHECK(std::memcmp(ct, wc.data(), 16) == 0);
    CHECK(std::memcmp(tag, wt.data(), 16) == 0);
    uint8_t back[16];
    CHECK(open(k, iv, nullptr, 0, ct, 16, tag, back));
    CHECK(std::memcmp(back, pt, 16) == 0);
    tag[0] ^= 1;
    CHECK(!open(k, iv, nullptr, 0, ct, 16, tag, back));
  }
  {
    uint8_t root[32];
    for (int i = 0; i < 32; i++) root[i] = uint8_t(i);
    uint8_t o1[32], o2[32], o3[32];
    uint8_t ctx[16] = {1, 2, 3};
    kdf(root, 32, "LRP-sec-2 session", ctx, 16, o1, 32);
    kdf(root, 32, "LRP-sec-2 session", ctx, 16, o2, 32);
    kdf(root, 32, "LRP-sec-2 blk", ctx, 16, o3, 32);
    CHECK(std::memcmp(o1, o2, 32) == 0);
    CHECK(std::memcmp(o1, o3, 32) != 0);
  }
}

static void test_security() {
  std::printf("test_security\n");
  Fixture f;
  const char* psk = "lrp-sec-1 psk material 32 bytes!!";
  CHECK(f.setup(AccessRemoteWrite | AccessRemoteRead | AccessRemoteAtomic, false, psk));

  std::memset(f.src, 0x77, 64);
  std::memset(f.dst, 0, 64);
  SendWr wr;
  wr.wr_id = 1;
  wr.opcode = WrOpcode::RdmaWrite;
  wr.sge = {uint64_t(uintptr_t(f.src)), 64, f.mra->lkey()};
  wr.remote_addr = uint64_t(uintptr_t(f.dst));
  wr.rkey = f.mrb->rkey();
  CHECK(f.qpa->post_send(wr) == 0);
  Cqe c;
  bool ok = pump_until(*f.a, *f.b, [&] { return f.cqa->poll(&c, 1) > 0; });
  CHECK(ok && c.status == WcStatus::Success);
  CHECK(std::memcmp(f.src, f.dst, 64) == 0);

  // 错密钥:帧被 AEAD 静默丢弃——不完成、不误报、链路保持可用(新语义:持续重传)
  f.a->set_psk("wrong-key-material-should-fail..", 32);
  std::memset(f.src, 0x11, 64);
  wr.wr_id = 2;
  CHECK(f.qpa->post_send(wr) == 0);
  ok = pump_until(*f.a, *f.b, [&] { return f.cqa->poll(&c, 1) > 0; }, 1200);
  CHECK(!ok);
  CHECK(f.qpa->state() == QpState::Rts);
  // 恢复正确密钥后,新 QP 立即可用(节点未受损;旧 QP 序号被未认证帧占住,属已知限制)
  f.a->set_psk(psk, std::strlen(psk));
  QueuePair* qa2 = f.a->create_qp(f.pda, f.cqa, QpType::Rc);
  QueuePair* qb2 = f.b->create_qp(f.pdb, f.cqb, QpType::Rc);
  CHECK(qa2 && qb2);
  CHECK(qa2->connect("127.0.0.1", f.b->port(), qb2->qpn()) == 0);
  CHECK(qb2->connect("127.0.0.1", f.a->port(), qa2->qpn()) == 0);
  wr.wr_id = 3;
  CHECK(qa2->post_send(wr) == 0);
  ok = pump_until(*f.a, *f.b, [&] { return f.cqa->poll(&c, 1) > 0; });
  CHECK(ok && c.wr_id == 3 && c.status == WcStatus::Success);
}

static void test_credit_flow() {
  std::printf("test_credit_flow\n");
  Fixture f;
  CHECK(f.setup(AccessRemoteWrite, false, nullptr, 0));

  std::strcpy(f.src, "credit test");
  SendWr wr;
  wr.wr_id = 901;
  wr.opcode = WrOpcode::Send;
  wr.sge = {uint64_t(uintptr_t(f.src)), 16, f.mra->lkey()};
  CHECK(f.qpa->post_send(wr) == 0);

  Cqe c;
  bool done = pump_until(*f.a, *f.b, [&] { return f.cqa->poll(&c, 1) > 0; }, 300);
  CHECK(!done);

  RecvWr rv;
  rv.wr_id = 902;
  rv.sge = {uint64_t(uintptr_t(f.dst)), 32, f.mrb->lkey()};
  CHECK(f.qpb->post_recv(rv) == 0);

  Cqe cs, cr;
  bool ok = pump_until(*f.a, *f.b,
                       [&] { return f.cqa->poll(&cs, 1) > 0 && f.cqb->poll(&cr, 1) > 0; });
  CHECK(ok && cs.wr_id == 901 && cr.wr_id == 902);
  CHECK(std::memcmp(f.src, f.dst, 16) == 0);
}

static void test_rate_limit() {
  std::printf("test_rate_limit\n");
  Fixture f;
  CHECK(f.setup(AccessRemoteWrite));
  CHECK(f.qpa->set_rate_limit(256) == 0);

  const int ops = 32;
  std::memset(f.src, 0x5A, 128);
  auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < ops; i++) {
    SendWr wr;
    wr.wr_id = uint64_t(i);
    wr.opcode = WrOpcode::RdmaWrite;
    wr.sge = {uint64_t(uintptr_t(f.src)), 1024, f.mra->lkey()};
    wr.remote_addr = uint64_t(uintptr_t(f.dst));
    wr.rkey = f.mrb->rkey();
    CHECK(f.qpa->post_send(wr) == 0);
  }
  int done = 0;
  Cqe c;
  while (done < ops && pump_until(*f.a, *f.b, [&] { return f.cqa->poll(&c, 1) > 0; }, 2000)) {
    if (c.status == WcStatus::Success) ++done;
  }
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  CHECK(done == ops);
  CHECK(secs > 0.4);
}

static void test_reset_resume() {
  std::printf("test_reset_resume\n");
  Fixture f;
  CHECK(f.setup(AccessRemoteWrite | AccessRemoteRead | AccessRemoteAtomic));
  CHECK(f.qpa->set_rate_limit(1) == 0);  // 极低速:帧全部挂起不上线,隔离网络缓冲干扰

  SendWr wr;
  wr.wr_id = 1;
  wr.opcode = WrOpcode::RdmaWrite;
  wr.sge = {uint64_t(uintptr_t(f.src)), 64, f.mra->lkey()};
  wr.remote_addr = uint64_t(uintptr_t(f.dst));
  wr.rkey = f.mrb->rkey();
  CHECK(f.qpa->post_send(wr) == 0);
  wr.wr_id = 2;
  CHECK(f.qpa->post_send(wr) == 0);
  wr.wr_id = 3;
  wr.opcode = WrOpcode::AtomicFaa;
  wr.sge = {uint64_t(uintptr_t(f.src)), 8, f.mra->lkey()};
  wr.swap = 1;
  CHECK(f.qpa->post_send(wr) == 0);
  wr.wr_id = 4;
  wr.opcode = WrOpcode::RdmaRead;
  wr.sge = {uint64_t(uintptr_t(f.src)), 64, f.mra->lkey()};
  CHECK(f.qpa->post_send(wr) == 0);

  CHECK(f.qpa->reset() == 0);
  CHECK(f.qpa->apply_resume_boundary(3) == 0);

  Cqe c;
  int n_ok = 0, n_err = 0, err_id = 0;
  while (f.cqa->poll(&c, 1) > 0) {
    if (c.status == WcStatus::Success) {
      ++n_ok;
    } else {
      ++n_err;
      err_id = int(c.wr_id);
    }
  }
  CHECK(n_ok == 2);
  CHECK(n_err == 1 && err_id == 3);  // 原子操作结果不确定 → 由应用决定是否重试

  f.qpb->reset();
  CHECK(f.qpa->connect("127.0.0.1", f.b->port(), f.qpb->qpn()) == 0);
  CHECK(f.qpb->connect("127.0.0.1", f.a->port(), f.qpa->qpn()) == 0);
  CHECK(f.qpa->set_rate_limit(0) == 0);
  bool ok = pump_until(*f.a, *f.b, [&] { return f.cqa->poll(&c, 1) > 0; });
  CHECK(ok && c.wr_id == 4 && c.status == WcStatus::Success);  // 读幂等重投成功
}

static void test_rto_adaptive() {
  std::printf("test_rto_adaptive\n");
  Fixture f;
  CHECK(f.setup(AccessRemoteWrite));
  for (int i = 0; i < 20; i++) {
    SendWr wr;
    wr.wr_id = uint64_t(i);
    wr.opcode = WrOpcode::RdmaWrite;
    wr.sge = {uint64_t(uintptr_t(f.src)), 8, f.mra->lkey()};
    wr.remote_addr = uint64_t(uintptr_t(f.dst));
    wr.rkey = f.mrb->rkey();
    CHECK(f.qpa->post_send(wr) == 0);
    Cqe c;
    CHECK(pump_until(*f.a, *f.b, [&] { return f.cqa->poll(&c, 1) > 0; }));
    CHECK(c.status == WcStatus::Success);
  }
  CHECK(f.qpa->rto_ms() >= 20 && f.qpa->rto_ms() <= 100);
}

int main() {
  test_rdma_write();
  test_send_recv();
  test_rdma_read();
  test_atomic_cas();
  test_atomic_faa();
  test_nak_invalid_then_alive();
  test_rnr_retry();
  test_tcp_channel();
  test_aead_kat();
  test_security();
  test_credit_flow();
  test_rate_limit();
  test_reset_resume();
  test_rto_adaptive();

  if (g_failures == 0) {
    std::printf("all tests passed\n");
    return 0;
  }
  std::printf("%d check(s) failed\n", g_failures);
  return 1;
}
