#include "peer_app.hpp"

#include "cm_net.hpp"
#include "net_log.hpp"
#include "rdma/verbs.hpp"

#include <arpa/inet.h>
#include <errno.h>
#if !defined(ESP_PLATFORM)
#include <ifaddrs.h>
#if defined(__APPLE__)
#include <net/if.h>
#include <net/if_dl.h>
#elif defined(__linux__)
#include <netpacket/packet.h>
#endif
#endif
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
#include <unordered_map>
#include <vector>

#if defined(ESP_PLATFORM)
#include <esp_mac.h>
#endif

using namespace rdma;
using Clock = std::chrono::steady_clock;

namespace {

constexpr uint64_t kOffCounter = 0;
constexpr uint64_t kOffHeartbeat = 8;   // 心跳落地槽(仅由对端写入)
constexpr uint64_t kOffScratchFaa = 24;  // FAA 原值本地暂存
constexpr uint64_t kOffScratchRead = 32; // Read 结果本地暂存
constexpr uint64_t kOffHbBuild = 48;     // 心跳组帧缓冲
constexpr uint64_t kRecvBase = 4096;     // Send 接收区(每对端 1KB)
constexpr uint64_t kOffBench = 512;      // 管理面测量本地缓冲(1KB 源 + 8B 结果)
constexpr uint64_t kOffBenchRes = 1536;
constexpr uint64_t kRemoteLat = 2048;    // 测量远端 8B 目标
constexpr uint64_t kRemoteThr = 8192;    // 测量远端 1KB 吞吐区
constexpr const char* kMcastGroup = "239.255.77.77";

uint32_t fnv1a(const uint8_t* p, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) {
    h ^= p[i];
    h *= 16777619u;
  }
  return h;
}

int64_t now_ms();

// 节点身份:从硬件 MAC(+端口)自动派生,免配置、免预分配标识符;--node-id 可人工覆盖(测试用)
uint32_t derive_node_id(uint16_t port) {
  uint8_t mac[8] = {0};
  size_t mac_len = 0;
#if defined(ESP_PLATFORM)
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) mac_len = 6;
#else
  struct ifaddrs* ifs = nullptr;
  if (getifaddrs(&ifs) == 0) {
    for (struct ifaddrs* i = ifs; i != nullptr; i = i->ifa_next) {
      if (!i->ifa_addr || (i->ifa_flags & IFF_LOOPBACK)) continue;
#if defined(__APPLE__)
      if (i->ifa_addr->sa_family == AF_LINK) {
        auto* sdl = reinterpret_cast<sockaddr_dl*>(i->ifa_addr);
        if (sdl->sdl_alen == 6) {
          std::memcpy(mac, LLADDR(sdl), 6);
          mac_len = 6;
          break;
        }
      }
#elif defined(__linux__)
      if (i->ifa_addr->sa_family == AF_PACKET) {
        auto* sll = reinterpret_cast<sockaddr_ll*>(i->ifa_addr);
        if (sll->sll_halen == 6) {
          std::memcpy(mac, sll->sll_addr, 6);
          mac_len = 6;
          break;
        }
      }
#endif
    }
    freeifaddrs(ifs);
  }
#endif
  if (mac_len == 6) {
    uint8_t seed[8];
    std::memcpy(seed, mac, 6);
    seed[6] = uint8_t(port >> 8);
    seed[7] = uint8_t(port);
    return fnv1a(seed, 8) & 0xFFFFFFu;
  }
  uint64_t mix = uint64_t(now_ms()) ^ (uint64_t(uintptr_t(&mac)) << 16) ^ port;
  return fnv1a(reinterpret_cast<const uint8_t*>(&mix), sizeof mix) & 0xFFFFFFu;
}

struct Options {
  std::string ip = "0.0.0.0";
  uint16_t port = 18515;
  uint32_t node_id = 0;  // 0=从硬件 MAC 自动派生
  int duration = 60;
  std::string psk;
  std::string admin_key = "lrp-admin";
  std::string log_ip = "255.255.255.255";
  std::vector<std::pair<std::string, uint16_t>> seeds;
};

struct Inflight {
  int64_t ts = 0;
  uint32_t peer_idx = 0;
  int kind = 0;  // 0=faa 1=write 2=read
};

struct Peer {
  uint32_t node_id = 0;
  std::string ip;
  uint16_t data_port = 0;
  QueuePair* qp = nullptr;
  uint32_t remote_qpn = 0;
  uint32_t rkey = 0;
  uint64_t base = 0;
  uint32_t credit = 16;
  bool connected = false;
  int64_t last_seen = 0;
  int64_t next_faa = 0;
  int64_t next_wr = 0;
  int64_t next_rd = 0;
  uint64_t ops = 0;
  double rtt_sum = 0;
  uint64_t rtt_n = 0;
  uint64_t remote_counter = 0;
  uint16_t seq = 0;
  uint64_t peer_epoch = 0;
  int64_t recover_at = 0;
  int recover_tries = 0;
};

int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch())
      .count();
}

void usage() {
  netlog::say(
      "用法: rdma_peer [--psk 口令] [--seed ip:port]...\n"
      "对等零配置模式: 同一协议设备任意时刻上电即自动入网,无需预配置或预分配标识符。\n"
      "日志回传: --log-ip(默认广播,电脑端 scripts/collect_udp.py 接收)\n"
      "管理指令: 受信任设备经 SRCTL 下发(--admin-key 配密钥);电脑端 scripts/ctl.py\n"
      "节点身份从硬件 MAC 自动派生;发现走组播+广播(可选 --seed 兜底)。\n"
      "端口约定: 数据 P(默认18515), 控制/发现 P+100。\n"
      "可选项(仅调试): --ip --port --node-id --duration\n");
}

bool parse(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; i++) {
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
    } else if (a == "--node-id") {
      const char* v = next("--node-id");
      if (!v) return false;
      o.node_id = uint32_t(std::atoi(v));
    } else if (a == "--duration") {
      const char* v = next("--duration");
      if (!v) return false;
      o.duration = std::atoi(v);
    } else if (a == "--psk") {
      const char* v = next("--psk");
      if (!v) return false;
      o.psk = v;
    } else if (a == "--admin-key") {
      const char* v = next("--admin-key");
      if (!v) return false;
      o.admin_key = v;
    } else if (a == "--log-ip") {
      const char* v = next("--log-ip");
      if (!v) return false;
      o.log_ip = v;
    } else if (a == "--seed") {
      const char* v = next("--seed");
      if (!v) return false;
      std::string s = v;
      size_t colon = s.rfind(':');
      if (colon == std::string::npos) return false;
      o.seeds.emplace_back(s.substr(0, colon), uint16_t(std::atoi(s.c_str() + colon + 1)));
    } else {
      std::fprintf(stderr, "未知参数 %s\n", a.c_str());
      return false;
    }
  }
  return true;
}

struct App {
  Options o;
  uint8_t my_rnd[16] = {0};
  char my_rnd_hex[33] = {0};
  std::unique_ptr<Context> ctx;
  ProtectionDomain* pd = nullptr;
  CompletionQueue* cq = nullptr;
  MemoryRegion* mr = nullptr;
  uint64_t* arena = nullptr;
  int ctrl_fd = -1;
  std::vector<std::unique_ptr<Peer>> peers;
  std::unordered_map<uint64_t, Inflight> inflight;
  uint64_t seq_global = 0;
  int64_t next_report = 0;

  Peer* find_peer(uint32_t node_id) {
    for (auto& p : peers) {
      if (p->node_id == node_id) return p.get();
    }
    return nullptr;
  }

  Peer* ensure_peer(uint32_t node_id, const std::string& ip, uint16_t data_port) {
    Peer* p = find_peer(node_id);
    if (p) return p;
    auto np = std::unique_ptr<Peer>(new Peer());
    np->node_id = node_id;
    np->ip = ip;
    np->data_port = data_port;
    np->qp = ctx->create_qp(pd, cq, QpType::Rc);
    if (!np->qp) return nullptr;
    p = np.get();
    size_t idx = peers.size();
    peers.push_back(std::move(np));
    // 投递接收项:支持节点间 Send(每个对端独立接收区)
    auto* base = reinterpret_cast<uint8_t*>(arena);
    for (int i = 0; i < 4; i++) {
      RecvWr rv;
      rv.wr_id = uint64_t(1000 + idx * 16 + i);
      rv.sge = {uint64_t(uintptr_t(base + kRecvBase + idx * 1024 + i * 240)), 240, mr->lkey()};
      p->qp->post_recv(rv);
    }
    netlog::say("[node %u] 发现新节点 %u @ %s:%u (本端 QPN=%u)", o.node_id, node_id, ip.c_str(),
                data_port, p->qp->qpn());
    return p;
  }

  void establish(Peer* p, uint64_t peer_epoch, unsigned peer_boundary, const char* rnd_hex) {
    bool peer_rebooted = (p->peer_epoch != 0 && peer_epoch != p->peer_epoch);
    if (peer_rebooted) {
      // 对端重启(epoch 变化):本端开新序号空间与之对齐,在途操作按边界分类续传
      netlog::say("[node %u] 节点 %u 重启(epoch 变化),序号重建+续传", o.node_id, p->node_id);
      p->qp->reset();
      p->connected = false;
    }
    if (p->connected && p->qp->state() != QpState::Err) return;
    if (peer_rebooted) {
      p->qp->apply_resume_boundary(peer_boundary);
    } else if (p->qp->state() == QpState::Err) {
      p->qp->recover_rearm();  // 闪断:序号续用,在途帧按原 psn 重传
    }
    if (p->qp->connect(p->ip, p->data_port, p->remote_qpn, p->credit) != 0) return;
    p->connected = true;
    netlog::say("[node %u] 会话参数: 节点 %u base=0x%llx rkey=%u qpn=%u",
                o.node_id, p->node_id, static_cast<unsigned long long>(p->base), p->rkey,
                p->remote_qpn);
    p->peer_epoch = peer_epoch;
    p->recover_tries = 0;
    uint8_t peer_rnd[16];
    if (rnd_hex && rnd_hex[0]) cm::parse_rnd_hex(rnd_hex, peer_rnd);
    else std::memset(peer_rnd, 0, 16);
    p->qp->derive_session_key(my_rnd, peer_rnd, 16);
    netlog::say("[node %u] 与节点 %u 建立对等连接 (QPN %u<->%u, epoch=%llu)\n", o.node_id,
                p->node_id, p->qp->qpn(), p->remote_qpn,
                static_cast<unsigned long long>(peer_epoch));
  }

  void send_ctrl(const std::string& msg, const sockaddr_in& to) {
    sendto(ctrl_fd, msg.data(), msg.size(), 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
  }

  void broadcast_discover() {
    char msg[160];
    std::snprintf(msg, sizeof msg, "SRDISCOVER node=%u data_port=%u rnd=%s\n", o.node_id, o.port, my_rnd_hex);
    sockaddr_in to {};
    to.sin_family = AF_INET;
    to.sin_port = htons(uint16_t(o.port + 100));
    // 1) 组播(自组网主通道,局域网即插即用)
    inet_pton(AF_INET, kMcastGroup, &to.sin_addr);
    sendto(ctrl_fd, msg, std::strlen(msg), 0, reinterpret_cast<sockaddr*>(&to), sizeof to);
    // 2) 广播兜底(部分网络过滤组播)
    to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(ctrl_fd, msg, std::strlen(msg), 0, reinterpret_cast<sockaddr*>(&to), sizeof to);
    // 3) 已知节点种子兜底(跨网段/隔离 AP 场景)
    for (auto& s : o.seeds) {
      to.sin_port = htons(uint16_t(s.second + 100));
      inet_pton(AF_INET, s.first.c_str(), &to.sin_addr);
      sendto(ctrl_fd, msg, std::strlen(msg), 0, reinterpret_cast<sockaddr*>(&to), sizeof to);
    }
  }

  void handle_ctrl(char* buf, size_t n, const sockaddr_in& from) {
    buf[n < 255 ? n : 255] = 0;
    unsigned node = 0, dp = 0, qpn = 0, credit = 16, rk = 0;
    unsigned long long base = 0;
    char ip[64];
    inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);

    if (std::strncmp(buf, "SRCTL", 5) == 0) {
      char key[64] = {0};
      char cmd[16] = {0};
      unsigned v1 = 0, v2 = 0;
      int n = std::sscanf(buf, "SRCTL key=%63s cmd=%15s val=%u val2=%u", key, cmd, &v1, &v2);
      if (n < 2 || o.admin_key != key) {
        netlog::say("[node %u][ctl] 已拒绝未授权管理指令", o.node_id);
        return;
      }
      if (std::strcmp(cmd, "rate") == 0 && n >= 3) {
        for (auto& pp : peers) pp->qp->set_rate_limit(v1);
        netlog::say("[node %u][ctl] 限速设置: %u kbps(全部对端;0=不限)", o.node_id, v1);
      } else if (std::strcmp(cmd, "bench") == 0 && n >= 4) {
        int ran = 0;
        for (auto& pp : peers) {
          if (pp->connected) {
            run_bench(*pp, int(v1), int(v2));
            ++ran;
          }
        }
        if (ran == 0) netlog::say("[node %u][ctl] 无已连接对端,bench 跳过", o.node_id);
      } else if (std::strcmp(cmd, "stats") == 0) {
        next_report = 1;
      } else {
        netlog::say("[node %u][ctl] 未知指令: %s", o.node_id, cmd);
      }
      return;
    }
    char rnd_hex[33] = {0};
    if (std::sscanf(buf, "SRDISCOVER node=%u data_port=%u rnd=%32s", &node, &dp, rnd_hex) >= 2) {
      if (node == o.node_id) return;
      Peer* p = ensure_peer(node, ip, uint16_t(dp));
      if (!p) return;
      p->last_seen = now_ms();
      char msg[256];
      std::snprintf(msg, sizeof msg,
                    "SROFFER node=%u data_port=%u qpn=%u rkey=%u base=%llu credit=%u rnd=%s"
                    " epoch=%llu boundary=%u\n",
                    o.node_id, o.port, p->qp->qpn(), mr->rkey(),
                    static_cast<unsigned long long>(uint64_t(uintptr_t(arena))),
                    p->qp->recv_credits(), my_rnd_hex,
                    static_cast<unsigned long long>(p->qp->conn_epoch()),
                    p->qp->resume_boundary());
      send_ctrl(msg, from);
      return;
    }
    unsigned long long peer_epoch = 0;
    unsigned peer_boundary = 0;
    if (std::sscanf(buf,
                    "SROFFER node=%u data_port=%u qpn=%u rkey=%u base=%llu credit=%u rnd=%32s"
                    " epoch=%llu boundary=%u",
                    &node, &dp, &qpn, &rk, &base, &credit, rnd_hex, &peer_epoch,
                    &peer_boundary) >= 5) {
      if (node == o.node_id) return;
      Peer* p = ensure_peer(node, ip, uint16_t(dp));
      if (!p) return;
      p->last_seen = now_ms();
      p->remote_qpn = qpn;
      p->rkey = rk;
      p->base = base;
      p->credit = credit ? credit : 16;
      establish(p, peer_epoch, peer_boundary, rnd_hex);
      char msg[256];
      std::snprintf(msg, sizeof msg,
                    "SRJOIN node=%u data_port=%u qpn=%u rkey=%u base=%llu credit=%u rnd=%s"
                    " epoch=%llu boundary=%u\n",
                    o.node_id, o.port, p->qp->qpn(), mr->rkey(),
                    static_cast<unsigned long long>(uint64_t(uintptr_t(arena))),
                    p->qp->recv_credits(), my_rnd_hex,
                    static_cast<unsigned long long>(p->qp->conn_epoch()),
                    p->qp->resume_boundary());
      send_ctrl(msg, from);
      return;
    }
    if (std::sscanf(buf,
                    "SRJOIN node=%u data_port=%u qpn=%u rkey=%u base=%llu credit=%u rnd=%32s"
                    " epoch=%llu boundary=%u",
                    &node, &dp, &qpn, &rk, &base, &credit, rnd_hex, &peer_epoch,
                    &peer_boundary) >= 5) {
      if (node == o.node_id) return;
      Peer* p = ensure_peer(node, ip, uint16_t(dp));
      if (!p) return;
      p->last_seen = now_ms();
      p->remote_qpn = qpn;
      p->rkey = rk;
      p->base = base;
      p->credit = credit ? credit : 16;
      establish(p, peer_epoch, peer_boundary, rnd_hex);
    }
  }

  void post_to_peer(size_t idx, int kind) {
    Peer& p = *peers[idx];
    SendWr wr;
    uint16_t seq = ++p.seq;
    wr.wr_id = (uint64_t(idx) << 32) | (uint64_t(kind) << 16) | seq;
    wr.rkey = p.rkey;
    if (kind == 0) {
      wr.opcode = WrOpcode::AtomicFaa;
      wr.sge = {uint64_t(uintptr_t(arena)) + kOffScratchFaa, 8, mr->lkey()};
      wr.remote_addr = p.base + kOffCounter;
      wr.swap = 1;
    } else if (kind == 1) {
      wr.opcode = WrOpcode::RdmaWrite;
      auto* hb = reinterpret_cast<uint8_t*>(arena) + kOffHbBuild;
      int n = std::snprintf(reinterpret_cast<char*>(hb), 96, "heartbeat node=%u seq=%u", o.node_id,
                            seq);
      wr.sge = {uint64_t(uintptr_t(hb)), uint32_t(n), mr->lkey()};
      wr.remote_addr = p.base + kOffHeartbeat;
    } else {
      wr.opcode = WrOpcode::RdmaRead;
      wr.sge = {uint64_t(uintptr_t(arena)) + kOffScratchRead, 8, mr->lkey()};
      wr.remote_addr = p.base + kOffCounter;
    }
    int prc = p.qp->post_send(wr);
    if (prc == 0) {
      inflight[wr.wr_id] = {now_ms(), uint32_t(idx), kind};
    } else if (prc == -EINVAL) {
      p.connected = false;  // QP 真异常才转重连;EAGAIN=窗口满,等完成即可
      if (now_ms() >= p.recover_at) {
        p.recover_at = now_ms() + 3000;
        netlog::say("[node %u] 节点 %u 发送失败(rc=%d),转入重连", o.node_id, p.node_id, prc);
      }
    }
  }

  void on_cqe(const Cqe& c, int64_t now) {
    if (c.wr_id >= 1000 && c.wr_id < 1100) {
      // 接收项完成:立即补投,维持对端 Send 信用
      size_t idx = size_t(c.wr_id - 1000) / 16;
      int slot = int((c.wr_id - 1000) % 16);
      if (idx < peers.size() && c.status == WcStatus::Success) {
        RecvWr rv;
        rv.wr_id = c.wr_id;
        rv.sge = {uint64_t(uintptr_t(reinterpret_cast<uint8_t*>(arena) + kRecvBase + idx * 1024 +
                                     size_t(slot % 4) * 240)),
                  240, mr->lkey()};
        peers[idx]->qp->post_recv(rv);
      }
      return;
    }
    auto it = inflight.find(c.wr_id);
    if (it == inflight.end()) return;
    uint32_t peer_idx = it->second.peer_idx;
    int kind = it->second.kind;
    double rtt = double(now_ms() - it->second.ts) * 1000.0;
    inflight.erase(it);
    Peer& p = *peers[peer_idx];
    if (c.status != WcStatus::Success) {
      if (now >= p.recover_at) {
        netlog::say("[node %u] 节点 %u 操作失败(status=%u),自动重传中", o.node_id, p.node_id,
                    unsigned(c.status));
        ++p.recover_tries;
        if (p.recover_tries >= 3) {
          netlog::say("[node %u] 节点 %u 恢复失败,等待重新发现(序号保留)", o.node_id, p.node_id);
          p.connected = false;
        } else {
          p.qp->recover_rearm();
          char msg[128];
          std::snprintf(msg, sizeof msg,
                        "SRRESUME node=%u data_port=%u rnd=%s epoch=%llu boundary=%u\n",
                        o.node_id, o.port, my_rnd_hex,
                        static_cast<unsigned long long>(p.qp->conn_epoch()),
                        p.qp->resume_boundary());
          sockaddr_in to {};
          to.sin_family = AF_INET;
          to.sin_port = htons(uint16_t(p.data_port + 100));
          inet_pton(AF_INET, p.ip.c_str(), &to.sin_addr);
          send_ctrl(msg, to);
        }
        p.recover_at = now + 3000;
      }
      return;
    }
    p.recover_tries = 0;
    ++p.ops;
    p.rtt_sum += rtt;
    ++p.rtt_n;
    if (kind == 2) {
      std::memcpy(&p.remote_counter, reinterpret_cast<uint8_t*>(arena) + kOffScratchRead, 8);
    }
  }

  static double pct(std::vector<double> v, double q) {
    std::sort(v.begin(), v.end());
    return v[size_t(q / 100.0 * double(v.size() - 1))];
  }

  // 管理面测量:对指定对端跑 8B 四操作延迟 + 1KB 写吞吐(结果经 WiFi 回传)
  void run_bench(Peer& p, int lat_n, int tp_kb) {
    if (!p.connected || p.qp->state() != QpState::Rts) {
      netlog::say("[node %u][bench] 节点 %u 未连接,跳过", o.node_id, p.node_id);
      return;
    }
    auto* base = reinterpret_cast<uint8_t*>(arena);
    netlog::say("[node %u][bench] ===== 开始测量 → 节点 %u (lat=%d, tp=%dKB) =====", o.node_id,
                p.node_id, lat_n, tp_kb);
    struct Row { const char* name; WrOpcode op; };
    Row rows[4] = {{"Write", WrOpcode::RdmaWrite}, {"Send", WrOpcode::Send},
                   {"Read", WrOpcode::RdmaRead}, {"Atomic", WrOpcode::AtomicFaa}};
    for (int k = 0; k < 4; k++) {
      std::vector<double> sm;
      int done_cnt = 0;
      for (int i = 0; i < lat_n; i++) {
        SendWr wr;
        wr.wr_id = 0xB0000000ull | (uint64_t(k) << 16) | uint64_t(i);
        wr.opcode = rows[k].op;
        wr.rkey = p.rkey;
        if (rows[k].op == WrOpcode::AtomicFaa) {
          wr.sge = {uint64_t(uintptr_t(base + kOffBenchRes)), 8, mr->lkey()};
          wr.remote_addr = p.base + kOffCounter;
          wr.swap = 1;
        } else if (rows[k].op == WrOpcode::RdmaRead) {
          wr.sge = {uint64_t(uintptr_t(base + kOffBenchRes)), 8, mr->lkey()};
          wr.remote_addr = p.base + kRemoteLat;
        } else if (rows[k].op == WrOpcode::Send) {
          wr.sge = {uint64_t(uintptr_t(base + kOffBench)), 8, mr->lkey()};
        } else {
          wr.sge = {uint64_t(uintptr_t(base + kOffBench)), 8, mr->lkey()};
          wr.remote_addr = p.base + kRemoteLat;
        }
        int64_t t0 = now_ms();
        if (p.qp->post_send(wr) != 0) break;
        bool ok = false;
        int64_t deadline = t0 + 3000;
        while (now_ms() < deadline) {
          ctx->progress();
          Cqe c;
          while (cq->poll(&c, 1) > 0) {
            if ((c.wr_id >> 28) == 0xB) {
              if (c.wr_id == wr.wr_id && c.status == WcStatus::Success) ok = true;
            } else {
              on_cqe(c, now_ms());
            }
          }
          if (ok) break;
        }
        if (!ok) break;
        sm.push_back(double(now_ms() - t0) * 1000.0);
        ++done_cnt;
      }
      if (done_cnt == 0) {
        netlog::say("[node %u][bench] %-8s 测量失败", o.node_id, rows[k].name);
        continue;
      }
      double sum = 0;
      for (double v : sm) sum += v;
      netlog::say("[node %u][bench] %-8s n=%d avg=%.1fus P50=%.1fus P99=%.1fus", o.node_id,
                  rows[k].name, done_cnt, sum / sm.size(), pct(sm, 50), pct(sm, 99));
    }
    // 1KB 写吞吐(窗口 32)
    int max_kb = int((64 * 1024 - kRemoteThr) / 1024);
    if (tp_kb > max_kb) tp_kb = max_kb;
    int sent = 0, inflight_n = 0;
    int64_t t0 = now_ms();
    while (sent < tp_kb) {
      while (inflight_n < 32 && sent < tp_kb) {
        SendWr wr;
        wr.wr_id = 0xB1000000ull | uint64_t(sent);
        wr.opcode = WrOpcode::RdmaWrite;
        wr.sge = {uint64_t(uintptr_t(base + kOffBench)), 1024, mr->lkey()};
        wr.remote_addr = p.base + kRemoteThr + uint64_t(sent) * 1024;
        wr.rkey = p.rkey;
        if (p.qp->post_send(wr) != 0) break;
        ++inflight_n;
        ++sent;
      }
      ctx->progress();
      Cqe c;
      while (cq->poll(&c, 1) > 0) {
        if ((c.wr_id >> 28) == 0xB) {
          if (inflight_n > 0 && (c.wr_id >> 24) == 0xB1) --inflight_n;
        } else on_cqe(c, now_ms());
      }
    }
    int64_t dl = now_ms() + 3000;
    while (inflight_n > 0 && now_ms() < dl) {
      ctx->progress();
      Cqe c;
      while (cq->poll(&c, 1) > 0) {
        if ((c.wr_id >> 28) == 0xB) {
          if (inflight_n > 0 && (c.wr_id >> 24) == 0xB1) --inflight_n;
        } else on_cqe(c, now_ms());
      }
    }
    double secs = double(now_ms() - t0) / 1000.0;
    if (secs > 0)
      netlog::say("[node %u][bench] 1KB Write 吞吐: %.2f MB/s (窗口32, %d KB)", o.node_id,
                  (double(tp_kb) / 1024.0) / secs, tp_kb);
    netlog::say("[node %u][bench] ===== 测量结束 =====", o.node_id);
  }

  void run() {
    auto end = now_ms() + int64_t(o.duration) * 1000;
    int64_t next_discover = 0;
    if (next_report == 0) next_report = now_ms() + 10000;

    while (now_ms() < end) {
      int64_t now = now_ms();
      if (now >= next_discover) {
        broadcast_discover();
        next_discover = now + 2000;
      }

      char buf[256];
      for (;;) {
        sockaddr_in from {};
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(ctrl_fd, buf, sizeof buf - 1, MSG_DONTWAIT,
                             reinterpret_cast<sockaddr*>(&from), &fl);
        if (n <= 0) break;
        handle_ctrl(buf, size_t(n), from);
      }

      ctx->progress();

      Cqe c;
      while (cq->poll(&c, 1) > 0) on_cqe(c, now);

      for (size_t i = 0; i < peers.size(); i++) {
        Peer& p = *peers[i];
        if (!p.connected) continue;
        if (now >= p.next_faa) {
          post_to_peer(i, 0);
          p.next_faa = now + 500;
        }
        if (now >= p.next_wr) {
          post_to_peer(i, 1);
          p.next_wr = now + 3000;
        }
        if (now >= p.next_rd) {
          post_to_peer(i, 2);
          p.next_rd = now + 5000;
        }
      }

      for (size_t i = 0; i < peers.size(); i++) {
        Peer& p = *peers[i];
        if (p.connected && now - p.last_seen > 15000) {
          netlog::say("[node %u] 节点 %u 心跳超时,断开并等待重新发现\n", o.node_id, p.node_id);
          p.qp->reset();
          p.connected = false;
          p.recover_at = now + 2000;
          p.recover_tries = 0;
        }
      }

      if (now >= next_report) {
        netlog::say("\n[node %u] === 对等节点状态 (%zu 个) ===\n", o.node_id, peers.size());
        for (auto& pp : peers) {
          Peer& p = *pp;
          double avg = p.rtt_n ? p.rtt_sum / double(p.rtt_n) : 0;
          netlog::say("  节点%-3u %-15s ops=%-6llu rtt_avg=%7.1fus 远端计数=%llu %s\n", p.node_id,
                      p.ip.c_str(), static_cast<unsigned long long>(p.ops), avg,
                      static_cast<unsigned long long>(p.remote_counter),
                      p.connected ? "" : "(未连接)");
        }
        next_report = now + 10000;
      }
      std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    netlog::say("[node %u] 结束\n", o.node_id);
  }
};

}  // namespace

int rdma_peer_main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  Options o;
  if (!parse(argc, argv, o)) {
    usage();
    return 2;
  }

  netlog::init(o.log_ip.c_str());
  App app;
  cm::make_rnd16(app.my_rnd);
  cm::rnd_hex(app.my_rnd, app.my_rnd_hex);
  if (o.node_id == 0) o.node_id = derive_node_id(o.port);
  app.o = o;
  app.ctx = Context::open(o.ip, o.port);
  if (!app.ctx) {
    std::fprintf(stderr, "[node %u] 无法绑定 %s:%u\n", o.node_id, o.ip.c_str(), o.port);
    return 1;
  }
  if (!o.psk.empty()) app.ctx->set_psk(o.psk.data(), o.psk.size());
  app.pd = app.ctx->alloc_pd();
  app.cq = app.ctx->create_cq(512);
  app.arena = cm::aligned_u64_alloc(8 * 1024);
  app.mr = app.pd->reg_mr(app.arena, 8 * 1024 * sizeof(uint64_t),
                          AccessRemoteWrite | AccessRemoteRead | AccessRemoteAtomic);

  app.ctrl_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  int yes = 1;
  setsockopt(app.ctrl_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
  setsockopt(app.ctrl_fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof yes);
  timeval tv {0, 20000};
  setsockopt(app.ctrl_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  sockaddr_in a {};
  a.sin_family = AF_INET;
  a.sin_port = htons(uint16_t(o.port + 100));
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(app.ctrl_fd, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0) {
    std::fprintf(stderr, "[node %u] 控制端口 %u 绑定失败: %s\n", o.node_id, o.port + 100,
                 std::strerror(errno));
    return 1;
  }

  netlog::say("[node %u] 对等节点启动(fw v0.4): 数据 %s:%u, 控制 %s:%u, 注册内存 64KB, rkey=%u\n",
              o.node_id, o.ip.c_str(), o.port, o.ip.c_str(), o.port + 100, app.mr->rkey());
  netlog::say("[node %u] 身份=0x%06x(硬件 MAC 派生), 开始发现对等节点...\n", o.node_id,
              o.node_id);

  ip_mreq mreq {};
  inet_pton(AF_INET, kMcastGroup, &mreq.imr_multiaddr);
  mreq.imr_interface.s_addr = htonl(INADDR_ANY);
  if (setsockopt(app.ctrl_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof mreq) < 0) {
    std::fprintf(stderr, "[node %u] 组播加入失败(将依赖广播/种子): %s\n", o.node_id,
                 std::strerror(errno));
  }

  app.run();
  close(app.ctrl_fd);
  return 0;
}
