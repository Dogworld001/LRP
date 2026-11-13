// ESP32 RDMA 节点固件(四角色):WiFi UDP 上运行软 RDMA 协议栈。
// 角色与网络参数在 idf.py menuconfig → "RDMA Node 配置" 中设置:
//   target / initiator     —— 功能演示(Write/Send/Read/Atomic/延迟/吞吐)
//   bench-server / bench-client —— RDMA 测量套件(8B 延迟 + 1KB 吞吐)

#include "bench_app.hpp"
#include "demo_app.hpp"
#include "peer_app.hpp"

#include <cstdio>
#include <cstring>

#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <nvs_flash.h>

static const char* TAG = "rdma_node";
static EventGroupHandle_t s_wifi_events;
static constexpr int kWifiGotIp = BIT0;

static void wifi_event_handler(void*, esp_event_base_t base, int32_t id, void*) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    esp_wifi_connect();
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    xEventGroupSetBits(s_wifi_events, kWifiGotIp);
  }
}

static void wifi_init_and_wait() {
  s_wifi_events = xEventGroupCreate();
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler,
                                             nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler,
                                             nullptr));

  wifi_config_t wc = {};
  std::strncpy(reinterpret_cast<char*>(wc.sta.ssid), CONFIG_RDMA_WIFI_SSID, sizeof wc.sta.ssid);
  std::strncpy(reinterpret_cast<char*>(wc.sta.password), CONFIG_RDMA_WIFI_PASS,
               sizeof wc.sta.password);
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
  ESP_ERROR_CHECK(esp_wifi_start());

  EventBits_t bits =
      xEventGroupWaitBits(s_wifi_events, kWifiGotIp, pdFALSE, pdTRUE, pdMS_TO_TICKS(30000));
  if (bits & kWifiGotIp) {
    esp_netif_ip_info_t ip {};
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) esp_netif_get_ip_info(netif, &ip);
    esp_wifi_set_ps(WIFI_PS_NONE);  // 关 WiFi 省电,降低收发延迟抖动
    ESP_LOGI(TAG, "WiFi 已连接 (%s), 本机 IP: " IPSTR, CONFIG_RDMA_WIFI_SSID, IP2STR(&ip.ip));
  } else {
    ESP_LOGE(TAG, "WiFi 连接超时,继续以 0.0.0.0 运行(无法跨设备通信)");
  }
}

static void run_lrp(void*) {
  char self_port[8], peer_port[8], mem_kb[8], iters[8], thr_ops[8], rate_s[8];
  std::snprintf(self_port, sizeof self_port, "%d", CONFIG_RDMA_SELF_PORT);
  std::snprintf(peer_port, sizeof peer_port, "%d", CONFIG_RDMA_PEER_PORT);
  std::snprintf(mem_kb, sizeof mem_kb, "%d", CONFIG_RDMA_MEM_KB);
  std::snprintf(iters, sizeof iters, "%d", CONFIG_RDMA_ITERS);
  std::snprintf(thr_ops, sizeof thr_ops, "%d", CONFIG_RDMA_THR_OPS);
  std::snprintf(rate_s, sizeof rate_s, "%d", CONFIG_RDMA_RATE_KBPS);

  const char* role = "target";
  bool bench = false;
  bool peer = false;
#if defined(CONFIG_RDMA_ROLE_INITIATOR)
  role = "initiator";
#elif defined(CONFIG_RDMA_ROLE_BENCH_SERVER)
  role = "server";
  bench = true;
#elif defined(CONFIG_RDMA_ROLE_BENCH_CLIENT)
  role = "client";
  bench = true;
#elif defined(CONFIG_RDMA_ROLE_PEER)
  role = "peer";
  peer = true;
#endif

  if (peer) {
    char* pargv[18];
    int j = 0;
    pargv[j++] = const_cast<char*>("rdma_peer");
    pargv[j++] = const_cast<char*>("--port");
    pargv[j++] = self_port;
    pargv[j++] = const_cast<char*>("--duration");
    pargv[j++] = const_cast<char*>("86400");
    if (sizeof(CONFIG_RDMA_PSK) > 1) {
      pargv[j++] = const_cast<char*>("--psk");
      pargv[j++] = const_cast<char*>(CONFIG_RDMA_PSK);
    }
    pargv[j++] = const_cast<char*>("--log-ip");
    pargv[j++] = const_cast<char*>(CONFIG_RDMA_LOG_IP);
    pargv[j++] = const_cast<char*>("--admin-key");
    pargv[j++] = const_cast<char*>(CONFIG_RDMA_ADMIN_KEY);
    ESP_LOGI(TAG, "启动 peer 零配置对等节点: 数据端口 %d, 身份自动派生", CONFIG_RDMA_SELF_PORT);
    rdma_peer_main(j, pargv);
    return;
  }

  char* argv[24];
  int i = 0;
  argv[i++] = const_cast<char*>(bench ? "rdma_bench" : "rdma_demo");
  argv[i++] = const_cast<char*>(role);
  argv[i++] = const_cast<char*>("--ip");
  argv[i++] = const_cast<char*>("0.0.0.0");
  argv[i++] = const_cast<char*>("--port");
  argv[i++] = self_port;
  argv[i++] = const_cast<char*>("--peer-ip");
  argv[i++] = const_cast<char*>(CONFIG_RDMA_PEER_IP);
  argv[i++] = const_cast<char*>("--peer-port");
  argv[i++] = peer_port;

  bool is_target_like =
#if defined(CONFIG_RDMA_ROLE_INITIATOR)
      false;
#else
      true;
#endif
#if defined(CONFIG_RDMA_ROLE_BENCH_CLIENT)
  is_target_like = false;
#endif

  if (bench) {
    if (is_target_like) {
      argv[i++] = const_cast<char*>("--mem-kb");
      argv[i++] = mem_kb;
      argv[i++] = const_cast<char*>("--duration");
      argv[i++] = const_cast<char*>("86400");
    } else {
      argv[i++] = const_cast<char*>("--lat-n");
      argv[i++] = iters;
      argv[i++] = const_cast<char*>("--tp-kb");
      argv[i++] = thr_ops;
    }
    if (sizeof(CONFIG_RDMA_PSK) > 1) {
      argv[i++] = const_cast<char*>("--psk");
      argv[i++] = const_cast<char*>(CONFIG_RDMA_PSK);
    }
    if (CONFIG_RDMA_RATE_KBPS > 0 && !is_target_like) {
      argv[i++] = const_cast<char*>("--rate-kbps");
      argv[i++] = rate_s;
    }
  } else if (is_target_like) {
    argv[i++] = const_cast<char*>("--mem-kb");
    argv[i++] = mem_kb;
    argv[i++] = const_cast<char*>("--duration");
    argv[i++] = const_cast<char*>("86400");
  } else {
    argv[i++] = const_cast<char*>("--iters");
    argv[i++] = iters;
    argv[i++] = const_cast<char*>("--thr-ops");
    argv[i++] = thr_ops;
  }

  if (bench) {
    argv[i++] = const_cast<char*>("--log-ip");
    argv[i++] = const_cast<char*>(CONFIG_RDMA_LOG_IP);
  }
  ESP_LOGI(TAG, "启动 %s 角色=%s: 本机 %d -> %s:%d", bench ? "bench" : "demo", role,
           CONFIG_RDMA_SELF_PORT, CONFIG_RDMA_PEER_IP, CONFIG_RDMA_PEER_PORT);
  if (bench) {
    rdma_bench_main(i, argv);
  } else {
    rdma_demo_main(i, argv);
  }
  vTaskDelete(nullptr);
}

extern "C" void app_main(void) {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
  }
  wifi_init_and_wait();
  // 双核分工:WiFi/lwIP 固定核 0,LRP 协议栈(收发/加解密/重传)固定核 1
  xTaskCreatePinnedToCore(run_lrp, "lrp", 16384, nullptr, 5, nullptr, 1);
}
