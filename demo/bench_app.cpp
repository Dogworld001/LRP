#include "bench_app.hpp"

#include "cm_net.hpp"
#include "net_log.hpp"
#include "rdma/verbs.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace rdma;
using Clock = std::chrono::steady_clock;

namespace {

constexpr uint64_t kOffCounter = 0;
constexpr uint64_t kRecvBase = 4096;
constexpr uint64_t kRecvStride = 256;
constexpr uint64_t kOffData = 8192;
constexpr uint64_t kOffLat = 12288;
constexpr uint64_t kOffThr = 16384;
constexpr int kWin = 32;

struct Opts {
  std::string role;
  std::string ip = "0.0.0.0";
  uint16_t port = 18515;
  std::string peer_ip = "127.0.0.1";
  uint16_t peer_port = 18515;
  int lat_n = 300;
  int tp_kb = 64;
  int mem_kb = 128;
  int duration = 60;
  uint32_t rate_kbps = 0;
  std::string psk;
  std::string log_ip = "255.255.255.255";
};

int64_t now_us() {
  return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch())
      .count();
}

void usage() {
  netlog::say(
      "用法:\n"
      "  rdma_bench server --ip IP --port P --peer-ip IP --peer-port P [选项]\n"
      "  rdma_bench client --ip IP --port P --peer-ip IP --peer-port P [选项]\n"
      "选项: --lat-n N(延迟次数,默认300) --tp-kb N(吞吐KB数,默认64)\n"
      "      --mem-kb N(target注册内存KB,默认128) --duration N(server运行秒数)\n");
}

bool parse(int argc, char** argv, Opts& o) {
  if (argc < 2) return false;
  o.role = argv[1];
  for (int i = 2; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&](const char* what) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "缺少参数 %s\n", what);
        return nullptr;
      }
      return argv[++i];
    };
    if (a == "--ip") {
      const char* v = next("--ip");
      if (!v) return false;
      o.ip = v;
    } else if (a == "--port") {
      const char* v = next("--port");
      if (!v) return false;
      o.port = uint16_t(std::atoi(v));
    } else if (a == "--peer-ip") {
      const char* v = next("--peer-ip");
      if (!v) return false;
      o.peer_ip = v;
    } else if (a == "--peer-port") {
      const char* v = next("--peer-port");
      if (!v) return false;
      o.peer_port = uint16_t(std::atoi(v));
    } else if (a == "--lat-n") {
      const char* v = next("--lat-n");
      if (!v) return false;
      o.lat_n = std::atoi(v);
    } else if (a == "--tp-kb") {
      const char* v = next("--tp-kb");
      if (!v) return false;
      o.tp_kb = std::atoi(v);
    } else if (a == "--mem-kb") {
      const char* v = next("--mem-kb");
      if (!v) return false;
      o.mem_kb = std::atoi(v);
    } else if (a == "--duration") {
      const char* v = next("--duration");
      if (!v) return false;
      o.duration = std::atoi(v);
    } else if (a == "--rate-kbps") {
      const char* v = next("--rate-kbps");
      if (!v) return false;
      o.rate_kbps = uint32_t(std::atoi(v));
    } else if (a == "--psk") {
      const char* v = next("--psk");
      if (!v) return false;
      o.psk = v;
    } else if (a == "--log-ip") {
      const char* v = next("--log-ip");
      if (!v) return false;
      o.log_ip = v;
    } else {
      std::fprintf(stderr, "未知参数 %s\n", a.c_str());
      return false;
    }
  }
  return o.role == "server" || o.role == "client";
}

bool wait_cqe_busy(Context& ctx, CompletionQueue& cq, Cqe* out, int timeout_ms = 3000) {
  int64_t end = now_us() + int64_t(timeout_ms) * 1000;
  while (now_us() < end) {
    ctx.progress();
    if (cq.poll(out, 1) > 0) return true;
  }
  return false;
}

double percentile(std::vector<double> v, double p) {
  std::sort(v.begin(), v.end());
  return v[size_t(p / 100.0 * double(v.size() - 1))];
}

struct LatResult {
  double avg = 0, p50 = 0, p99 = 0;
};

LatResult summarize(std::vector<double>& s) {
  LatResult r;
  if (s.empty()) return r;
  double sum = 0;
  for (double v : s) sum += v;
  r.avg = sum / double(s.size());
  r.p50 = percentile(s, 50);
  r.p99 = percentile(s, 99);
  return r;
}

int run_server(const Opts& o) {
  auto ctx = Context::open(o.ip, o.port);
  if (!ctx) {
    std::fprintf(stderr, "[server] 无法绑定 %s:%u\n", o.ip.c_str(), o.port);
    return 1;
  }
  if (!o.psk.empty()) ctx->set_psk(o.psk.data(), o.psk.size());
  auto* pd = ctx->alloc_pd();
  auto* cq = ctx->create_cq(512);
  uint64_t* arena = cm::aligned_u64_alloc(size_t(o.mem_kb) * 128);
  auto* mr = pd->reg_mr(arena, size_t(o.mem_kb) * 128 * sizeof(uint64_t),
                        AccessRemoteWrite | AccessRemoteRead | AccessRemoteAtomic);
  auto* qp = ctx->create_qp(pd, cq, QpType::Rc);
  if (!mr || !qp) return 1;

  int cfd = cm::ctrl_bind(uint16_t(o.port + 100));
  if (cfd < 0) return 1;
  netlog::say("[server] fw v0.4: RDMA 目标就绪: %s:%u, QPN=%u, 注册内存 %d KB\n", o.ip.c_str(), o.port,
              qp->qpn(), o.mem_kb);
  netlog::say("[server] 等待客户端...\n");

  auto* base = reinterpret_cast<uint8_t*>(arena);
  for (int i = 0; i < 8; i++) {
    RecvWr rv;
    rv.wr_id = uint64_t(1000 + i);
    rv.sge = {uint64_t(uintptr_t(base + kRecvBase + i * kRecvStride)), 240, mr->lkey()};
    qp->post_recv(rv);
  }

  uint32_t init_qpn = 0;
  uint8_t rnd_local[16], rnd_peer[16];
  char peer_ip_learned[64] = {0};
  if (!cm::target_handshake(cfd, mr->rkey(), uint64_t(uintptr_t(arena)), qp->qpn(),
                            qp->recv_credits(), &init_qpn, rnd_local, rnd_peer,
                            peer_ip_learned)) {
    std::fprintf(stderr, "[server] CM 握手超时\n");
    return 1;
  }
  close(cfd);
  qp->derive_session_key(rnd_local, rnd_peer, 16);
  std::string peer_ip = peer_ip_learned[0] ? peer_ip_learned : o.peer_ip;
  netlog::say("[server] 从握手学习对端地址: %s:%u", peer_ip.c_str(), o.peer_port);
  if (qp->connect(peer_ip, o.peer_port, init_qpn) != 0) return 1;
  netlog::say("[server] 客户端已接入, 开始服务\n");

  int64_t end = now_us() + int64_t(o.duration) * 1000000;
  Cqe c;
  while (now_us() < end) {
    ctx->progress();
    while (cq->poll(&c, 1) > 0) {
      if (c.opcode == WrOpcode::Send && c.status == WcStatus::Success) {
        size_t slot = size_t(c.wr_id - 1000);
        RecvWr rv;
        rv.wr_id = c.wr_id;
        rv.sge = {uint64_t(uintptr_t(base + kRecvBase + slot * kRecvStride)), 240, mr->lkey()};
        qp->post_recv(rv);
      }
    }
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  netlog::say("[server] 结束\n");
  return 0;
}

double lat_one(Context& ctx, CompletionQueue& cq, QueuePair& qp, const SendWr& wr) {
  int64_t t0 = now_us();
  if (qp.post_send(wr) != 0) return -1;
  Cqe c;
  if (!wait_cqe_busy(ctx, cq, &c) || c.status != WcStatus::Success) return -1;
  return double(now_us() - t0);
}

int run_client(const Opts& o) {
  auto ctx = Context::open(o.ip, o.port);
  if (!ctx) return 1;
  if (!o.psk.empty()) ctx->set_psk(o.psk.data(), o.psk.size());
  auto* pd = ctx->alloc_pd();
  auto* cq = ctx->create_cq(512);
  uint64_t* local = cm::aligned_u64_alloc(8 * 1024);
  auto* mr = pd->reg_mr(local, 8 * 1024 * sizeof(uint64_t), AccessLocalWrite);
  auto* qp = ctx->create_qp(pd, cq, QpType::Rc);
  if (!mr || !qp) return 1;

  uint32_t tgt_qpn = 0, rkey = 0, credit = 16;
  uint64_t remote_base = 0;
  netlog::say("[client] fw v0.4: CM 握手 %s:%u ...\n", o.peer_ip.c_str(), o.peer_port);
  uint8_t rnd_local[16], rnd_peer[16];
  if (!cm::initiator_handshake(o.peer_ip.c_str(), o.peer_port, qp->qpn(), &tgt_qpn, &rkey,
                               &remote_base, &credit, rnd_local, rnd_peer)) {
    std::fprintf(stderr, "[client] 握手失败\n");
    return 1;
  }
  qp->derive_session_key(rnd_local, rnd_peer, 16);
  if (qp->connect(o.peer_ip, o.peer_port, tgt_qpn, credit) != 0) return 1;
  if (o.rate_kbps > 0) qp->set_rate_limit(o.rate_kbps);
  netlog::say("[client] 握手完成: 对端 QPN=%u, rkey=%u\n", tgt_qpn, rkey);

  auto* lbase = reinterpret_cast<uint8_t*>(local);
  const uint32_t lkey = mr->lkey();

  std::vector<double> lw, ls, lr, la;
  lw.reserve(size_t(o.lat_n));
  ls.reserve(size_t(o.lat_n));
  lr.reserve(size_t(o.lat_n));
  la.reserve(size_t(o.lat_n));

  netlog::say("[client] 8B 往返延迟测试 (%d 次 x 4 种操作)...\n", o.lat_n);
  for (int i = 0; i < o.lat_n; i++) {
    SendWr wr;
    wr.wr_id = uint64_t(i);
    wr.opcode = WrOpcode::RdmaWrite;
    wr.sge = {uint64_t(uintptr_t(lbase)), 8, lkey};
    wr.remote_addr = remote_base + kOffLat;
    wr.rkey = rkey;
    double t = lat_one(*ctx, *cq, *qp, wr);
    if (t < 0) break;
    lw.push_back(t);
  }
  for (int i = 0; i < o.lat_n; i++) {
    SendWr wr;
    wr.wr_id = uint64_t(10000 + i);
    wr.opcode = WrOpcode::Send;
    wr.sge = {uint64_t(uintptr_t(lbase + 16)), 8, lkey};
    double t = lat_one(*ctx, *cq, *qp, wr);
    if (t < 0) break;
    ls.push_back(t);
  }
  for (int i = 0; i < o.lat_n; i++) {
    SendWr wr;
    wr.wr_id = uint64_t(20000 + i);
    wr.opcode = WrOpcode::RdmaRead;
    wr.sge = {uint64_t(uintptr_t(lbase + 24)), 8, lkey};
    wr.remote_addr = remote_base + kOffData;
    wr.rkey = rkey;
    double t = lat_one(*ctx, *cq, *qp, wr);
    if (t < 0) break;
    lr.push_back(t);
  }
  for (int i = 0; i < o.lat_n; i++) {
    SendWr wr;
    wr.wr_id = uint64_t(30000 + i);
    wr.opcode = WrOpcode::AtomicFaa;
    wr.sge = {uint64_t(uintptr_t(lbase + 32)), 8, lkey};
    wr.remote_addr = remote_base + kOffCounter;
    wr.rkey = rkey;
    wr.swap = 1;
    double t = lat_one(*ctx, *cq, *qp, wr);
    if (t < 0) break;
    la.push_back(t);
  }

  netlog::say("[client] 1KB Write 吞吐测试 (window=%d, 共 %d KB)...\n", kWin, o.tp_kb);
  int sent = 0, inflight = 0;
  Cqe c;
  int64_t t0 = now_us();
  bool ok = true;
  while (sent < o.tp_kb) {
    while (inflight < kWin && sent < o.tp_kb) {
      SendWr wr;
      wr.wr_id = uint64_t(60000 + sent);
      wr.opcode = WrOpcode::RdmaWrite;
      wr.sge = {uint64_t(uintptr_t(lbase + 4096)), 1024, lkey};
      wr.remote_addr = remote_base + kOffThr + uint64_t(sent) * 1024;
      wr.rkey = rkey;
      if (qp->post_send(wr) != 0) break;
      ++inflight;
      ++sent;
    }
    ctx->progress();
    while (inflight > 0 && cq->poll(&c, 1) > 0) --inflight;
    if (c.status != WcStatus::Success) ok = false;
  }
  while (inflight > 0) {
    ctx->progress();
    if (cq->poll(&c, 1) > 0) --inflight;
    std::this_thread::sleep_for(std::chrono::microseconds(50));
  }
  double secs = double(now_us() - t0) / 1e6;
  double mbps = (double(o.tp_kb) / 1024.0) / secs;

  LatResult W = summarize(lw), S = summarize(ls), R = summarize(lr), A = summarize(la);
  netlog::say("\n================ 无线 RDMA 测量结果 ================\n");
  netlog::say("%-24s %10s %10s %10s\n", "项目(8B 往返)", "平均", "P50", "P99");
  netlog::say("%-24s %8.1f us %8.1f us %8.1f us\n", "RDMA Write", W.avg, W.p50, W.p99);
  netlog::say("%-24s %8.1f us %8.1f us %8.1f us\n", "Send", S.avg, S.p50, S.p99);
  netlog::say("%-24s %8.1f us %8.1f us %8.1f us\n", "RDMA Read", R.avg, R.p50, R.p99);
  netlog::say("%-24s %8.1f us %8.1f us %8.1f us\n", "Atomic FAA", A.avg, A.p50, A.p99);
  netlog::say("-----------------------------------------------------\n");
  netlog::say("%-24s %8.1f MB/s (窗口 %d, 成功率 %s)\n", "1KB Write 吞吐", mbps, kWin,
              ok ? "100%" : "有错误");
  netlog::say("=====================================================\n");
  return 0;
}

}  // namespace

int rdma_bench_main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  Opts o;
  if (!parse(argc, argv, o)) {
    usage();
    return 2;
  }
  netlog::init(o.log_ip.c_str());
  return o.role == "server" ? run_server(o) : run_client(o);
}
