#include "cm_net.hpp"
#include "demo_app.hpp"

#include "rdma/verbs.hpp"

#include <arpa/inet.h>
#include <errno.h>
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

#if defined(ESP_PLATFORM)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
static void sleep_us(int us) {
  if (us >= 1000) {
    vTaskDelay(pdMS_TO_TICKS(us / 1000));
  } else {
    vTaskDelay(1);
  }
}
#else
static void sleep_us(int us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); }
#endif

namespace {

struct Options {
  std::string role;
  std::string ip = "0.0.0.0";
  uint16_t port = 18515;
  std::string peer_ip = "127.0.0.1";
  uint16_t peer_port = 18515;
  int iters = 2000;
  int thr_ops = 1000;
  int mem_kb = 4096;
  int duration = 30;
  bool verbose = false;
};

// 目标设备注册内存布局(相对基址偏移),需与 MCU 小内存配置兼容
constexpr uint64_t kOffCounter = 0;
constexpr uint64_t kRecvBase = 4096;
constexpr uint64_t kRecvStride = 256;
constexpr uint64_t kOffData = 8192;
constexpr uint64_t kOffLat = 12288;
constexpr uint64_t kOffThr = 16384;

void usage() {
  std::printf(
      "用法:\n"
      "  rdma_demo target    [--ip IP] [--port P] --peer-ip IP --peer-port P [选项]\n"
      "  rdma_demo initiator [--ip IP] [--port P] --peer-ip IP --peer-port P [选项]\n"
      "选项:\n"
      "  --ip / --port       本机绑定地址(默认 0.0.0.0:18515)\n"
      "  --peer-ip / --peer-port  对端设备地址\n"
      "  --iters N           延迟测试次数(默认 2000, 仅 initiator)\n"
      "  --thr-ops N         吞吐测试 1KB Write 个数(默认 1000, 仅 initiator)\n"
      "  --mem-kb N          目标注册内存大小 KB(默认 4096; MCU 用 128, 仅 target)\n"
      "  --duration N        target 运行秒数(默认 30)\n"
      "  --verbose           打印每个完成事件\n"
      "说明:\n"
      "  target 监听数据端口 P 与控制端口 P+100; initiator 通过控制通道\n"
      "  完成 CM 握手(交换 QPN / rkey / 内存基址)后执行 RDMA 操作。\n");
}

bool parse(int argc, char** argv, Options& o) {
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
    } else if (a == "--iters") {
      const char* v = next("--iters");
      if (!v) return false;
      o.iters = std::atoi(v);
    } else if (a == "--thr-ops") {
      const char* v = next("--thr-ops");
      if (!v) return false;
      o.thr_ops = std::atoi(v);
    } else if (a == "--mem-kb") {
      const char* v = next("--mem-kb");
      if (!v) return false;
      o.mem_kb = std::atoi(v);
    } else if (a == "--duration") {
      const char* v = next("--duration");
      if (!v) return false;
      o.duration = std::atoi(v);
    } else if (a == "--verbose") {
      o.verbose = true;
    } else {
      std::fprintf(stderr, "未知参数 %s\n", a.c_str());
      return false;
    }
  }
  return o.role == "target" || o.role == "initiator";
}

bool wait_cqe(Context& ctx, CompletionQueue& cq, Cqe* out, int timeout_ms = 5000) {
  auto end = Clock::now() + std::chrono::milliseconds(timeout_ms);
  while (Clock::now() < end) {
    ctx.progress();
    if (cq.poll(out, 1) > 0) return true;
    sleep_us(100);
  }
  return false;
}

double percentile_us(std::vector<double> samples, double p) {
  std::sort(samples.begin(), samples.end());
  size_t i = size_t(p / 100.0 * double(samples.size() - 1));
  return samples[i];
}

int run_target(const Options& o) {
  if (o.mem_kb < 64) {
    std::fprintf(stderr, "[B] --mem-kb 至少 64\n");
    return 1;
  }
  auto ctx = Context::open(o.ip, o.port);
  if (!ctx) {
    std::fprintf(stderr, "[B] 无法绑定 %s:%u\n", o.ip.c_str(), o.port);
    return 1;
  }
  auto* pd = ctx->alloc_pd();
  auto* cq = ctx->create_cq(512);
  uint64_t* arena = cm::aligned_u64_alloc(size_t(o.mem_kb) * 128);
  const uint32_t access = AccessRemoteWrite | AccessRemoteRead | AccessRemoteAtomic;
  auto* mr = pd->reg_mr(arena, size_t(o.mem_kb) * 128 * sizeof(uint64_t), access);
  auto* qp = ctx->create_qp(pd, cq, QpType::Rc);
  if (!mr || !qp) {
    std::fprintf(stderr, "[B] 资源创建失败\n");
    return 1;
  }

  int cfd = cm::ctrl_bind(uint16_t(o.port + 100));
  if (cfd < 0) return 1;
  std::printf("[B] 目标设备启动: 数据 %s:%u, 控制 %s:%u, QPN=%u, 注册内存 %d KB (rkey=%u)\n",
              o.ip.c_str(), o.port, o.ip.c_str(), o.port + 100, qp->qpn(), o.mem_kb, mr->rkey());
  std::printf("[B] 等待设备A接入...\n");

  auto* base = reinterpret_cast<uint8_t*>(arena);
  for (int i = 0; i < 8; i++) {
    RecvWr rv;
    rv.wr_id = uint64_t(1000 + i);
    rv.sge = {uint64_t(uintptr_t(base + kRecvBase + i * kRecvStride)), 240, mr->lkey()};
    if (qp->post_recv(rv) != 0) {
      std::fprintf(stderr, "[B] 接收队列投递失败\n");
      return 1;
    }
  }

  uint32_t init_qpn = 0;
  uint8_t rnd_local[16], rnd_peer[16];
  char peer_ip_learned[64] = {0};
  if (!cm::target_handshake(cfd, mr->rkey(), uint64_t(uintptr_t(arena)), qp->qpn(),
                        qp->recv_credits(), &init_qpn, rnd_local, rnd_peer, peer_ip_learned)) {
    std::fprintf(stderr, "[B] CM 握手超时\n");
    close(cfd);
    return 1;
  }
  close(cfd);
  qp->derive_session_key(rnd_local, rnd_peer, 16);
  std::string peer_ip = peer_ip_learned[0] ? peer_ip_learned : o.peer_ip;
  if (qp->connect(peer_ip, o.peer_port, init_qpn) != 0) {
    std::fprintf(stderr, "[B] QP 连接失败\n");
    return 1;
  }
  std::printf("[B] 设备A已接入 (对端 QPN=%u), 开始服务\n", init_qpn);

  auto end = Clock::now() + std::chrono::seconds(o.duration);
  Cqe c;
  while (Clock::now() < end) {
    ctx->progress();
    while (cq->poll(&c, 1) > 0) {
      if (c.opcode == WrOpcode::Send && c.status == WcStatus::Success) {
        size_t slot = size_t(c.wr_id - 1000);
        const char* msg = reinterpret_cast<const char*>(base + kRecvBase + slot * kRecvStride);
        std::printf("[B] 收到 Send 消息 (%u 字节): %s\n", c.byte_len, msg);
        RecvWr rv;
        rv.wr_id = c.wr_id;
        rv.sge = {uint64_t(uintptr_t(base + kRecvBase + slot * kRecvStride)), 240, mr->lkey()};
        qp->post_recv(rv);
      } else if (o.verbose) {
        std::printf("[B] CQE wr_id=%" PRIu64 " status=%u\n", c.wr_id, unsigned(c.status));
      }
    }
    sleep_us(200);
  }
  std::printf("[B] 目标设备结束运行\n");
  return 0;
}

int run_initiator(const Options& o) {
  auto ctx = Context::open(o.ip, o.port);
  if (!ctx) {
    std::fprintf(stderr, "[A] 无法绑定 %s:%u\n", o.ip.c_str(), o.port);
    return 1;
  }
  auto* pd = ctx->alloc_pd();
  auto* cq = ctx->create_cq(512);
  // 本地缓冲最大使用偏移约 42KB,64KB 足够(MCU 内存友好)
  uint64_t* local = cm::aligned_u64_alloc(8 * 1024);
  auto* mr = pd->reg_mr(local, 8 * 1024 * sizeof(uint64_t), AccessLocalWrite);
  auto* qp = ctx->create_qp(pd, cq, QpType::Rc);
  if (!mr || !qp) {
    std::fprintf(stderr, "[A] 资源创建失败\n");
    return 1;
  }

  uint32_t tgt_qpn = 0, rkey = 0, credit = 16;
  uint64_t remote_base = 0;
  std::printf("[A] 正在通过控制通道连接设备B (%s:%u)...\n", o.peer_ip.c_str(), o.peer_port);
  uint8_t rnd_local[16], rnd_peer[16];
  if (!cm::initiator_handshake(o.peer_ip.c_str(), o.peer_port, qp->qpn(), &tgt_qpn, &rkey,
                              &remote_base, &credit, rnd_local, rnd_peer)) {
    std::fprintf(stderr, "[A] CM 握手失败, 目标设备未响应\n");
    return 1;
  }
  qp->derive_session_key(rnd_local, rnd_peer, 16);
  if (qp->connect(o.peer_ip, o.peer_port, tgt_qpn, credit) != 0) {
    std::fprintf(stderr, "[A] QP 连接失败\n");
    return 1;
  }
  std::printf("[A] 握手完成: 对端 QPN=%u, rkey=%u, 内存基址=0x%" PRIx64 "\n", tgt_qpn, rkey,
              remote_base);
  std::printf("[A] === RDMA 双设备演示开始 ===\n");

  auto* lbase = reinterpret_cast<uint8_t*>(local);
  const uint32_t lkey = mr->lkey();

  SendWr wr;
  Cqe c;

  std::vector<uint8_t> pattern(1024);
  for (size_t i = 0; i < pattern.size(); i++) pattern[i] = uint8_t(i * 7 + 3);
  std::memcpy(lbase, pattern.data(), pattern.size());
  wr = SendWr();
  wr.wr_id = 1;
  wr.opcode = WrOpcode::RdmaWrite;
  wr.sge = {uint64_t(uintptr_t(lbase)), 1024, lkey};
  wr.remote_addr = remote_base + kOffData;
  wr.rkey = rkey;
  if (qp->post_send(wr) != 0 || !wait_cqe(*ctx, *cq, &c) || c.status != WcStatus::Success) {
    std::fprintf(stderr, "[A] 操作1 RDMA Write 失败\n");
    return 1;
  }
  std::printf("[A] 操作1 RDMA Write 1KB 完成 (wr_id=%" PRIu64 ", %u 字节)\n", c.wr_id, c.byte_len);

  const char* msg = "你好,这是设备A通过 RDMA Send 发来的消息";
  std::snprintf(reinterpret_cast<char*>(lbase + 8192), 240, "%s", msg);
  wr = SendWr();
  wr.wr_id = 2;
  wr.opcode = WrOpcode::Send;
  size_t msg_len = std::strlen(reinterpret_cast<char*>(lbase + 8192)) + 1;
  wr.sge = {uint64_t(uintptr_t(lbase + 8192)), uint32_t(msg_len), lkey};
  if (qp->post_send(wr) != 0 || !wait_cqe(*ctx, *cq, &c) || c.status != WcStatus::Success) {
    std::fprintf(stderr, "[A] 操作2 Send 失败\n");
    return 1;
  }
  std::printf("[A] 操作2 Send 消息完成 (%zu 字节)\n", msg_len);

  std::memset(lbase + 16384, 0, 1024);
  wr = SendWr();
  wr.wr_id = 3;
  wr.opcode = WrOpcode::RdmaRead;
  wr.sge = {uint64_t(uintptr_t(lbase + 16384)), 1024, lkey};
  wr.remote_addr = remote_base + kOffData;
  wr.rkey = rkey;
  if (qp->post_send(wr) != 0 || !wait_cqe(*ctx, *cq, &c) || c.status != WcStatus::Success) {
    std::fprintf(stderr, "[A] 操作3 RDMA Read 失败\n");
    return 1;
  }
  bool match = std::memcmp(lbase + 16384, pattern.data(), 1024) == 0;
  std::printf("[A] 操作3 RDMA Read 回读校验 %s\n", match ? "通过" : "失败");
  if (!match) return 1;

  wr = SendWr();
  wr.wr_id = 4;
  wr.opcode = WrOpcode::AtomicFaa;
  wr.sge = {uint64_t(uintptr_t(lbase + 24576)), 8, lkey};
  wr.remote_addr = remote_base + kOffCounter;
  wr.rkey = rkey;
  wr.swap = 1;
  if (qp->post_send(wr) != 0 || !wait_cqe(*ctx, *cq, &c) || c.status != WcStatus::Success) {
    std::fprintf(stderr, "[A] 操作4 Atomic FAA 失败\n");
    return 1;
  }
  wr = SendWr();
  wr.wr_id = 5;
  wr.opcode = WrOpcode::RdmaRead;
  wr.sge = {uint64_t(uintptr_t(lbase + 24584)), 8, lkey};
  wr.remote_addr = remote_base + kOffCounter;
  wr.rkey = rkey;
  if (qp->post_send(wr) != 0 || !wait_cqe(*ctx, *cq, &c) || c.status != WcStatus::Success) {
    std::fprintf(stderr, "[A] 操作4 计数器回读失败\n");
    return 1;
  }
  uint64_t counter = 0;
  std::memcpy(&counter, lbase + 24584, 8);
  std::printf("[A] 操作4 Atomic FAA 计数器 = %" PRIu64 "\n", counter);

  std::vector<double> samples;
  samples.reserve(size_t(o.iters));
  for (int i = 0; i < o.iters; i++) {
    auto t0 = Clock::now();
    wr = SendWr();
    wr.wr_id = uint64_t(100 + i);
    wr.opcode = WrOpcode::RdmaWrite;
    wr.sge = {uint64_t(uintptr_t(lbase + 32768)), 8, lkey};
    wr.remote_addr = remote_base + kOffLat;
    wr.rkey = rkey;
    if (qp->post_send(wr) != 0 || !wait_cqe(*ctx, *cq, &c) || c.status != WcStatus::Success) {
      std::fprintf(stderr, "[A] 操作5 延迟测试中断于第 %d 次\n", i);
      return 1;
    }
    double dt = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
    samples.push_back(dt);
  }
  double sum = 0;
  for (double v : samples) sum += v;
  std::printf(
      "[A] 操作5 延迟测试 %d 次 8B Write: 平均 %.1f us, P50 %.1f us, P99 %.1f us, 最小 %.1f us\n",
      o.iters, sum / samples.size(), percentile_us(samples, 50), percentile_us(samples, 99),
      *std::min_element(samples.begin(), samples.end()));

  const size_t kOps = size_t(o.thr_ops);
  const int kBatch = 32;
  size_t sent = 0;
  int prev_sent = -1;
  int inflight = 0;
  auto t0 = Clock::now();
  while (sent < kOps) {
    while (inflight < kBatch && sent < kOps) {
      wr = SendWr();
      wr.wr_id = uint64_t(50000 + sent);
      wr.opcode = WrOpcode::RdmaWrite;
      wr.sge = {uint64_t(uintptr_t(lbase + 40960)), 1024, lkey};
      wr.remote_addr = remote_base + kOffThr + sent * 1024;
      wr.rkey = rkey;
      if (qp->post_send(wr) != 0) break;
      ++inflight;
      ++sent;
    }
    ctx->progress();
    while (inflight > 0 && cq->poll(&c, 1) > 0) --inflight;
    if (inflight == 0 && sent < kOps && prev_sent == sent) break;
    prev_sent = sent;
    if (inflight > 0) sleep_us(50);
  }
  auto drain_end = Clock::now() + std::chrono::seconds(3);
  while (inflight > 0 && Clock::now() < drain_end) {
    ctx->progress();
    if (cq->poll(&c, 1) > 0) --inflight;
    sleep_us(50);
  }
  double secs = std::chrono::duration<double>(Clock::now() - t0).count();
  double mbps = (double(kOps) * 1024.0 / 1048576.0) / secs;
  std::printf("[A] 操作6 吞吐测试 %zu x 1KB Write: %.1f MB/s (含确认, 用时 %.2f s)\n", kOps, mbps,
              secs);

  std::printf("[A] === 演示结束 ===\n");
  return 0;
}

}  // namespace

int rdma_demo_main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  Options o;
  if (!parse(argc, argv, o)) {
    usage();
    return 2;
  }
  if (o.role == "target") return run_target(o);
  return run_initiator(o);
}
