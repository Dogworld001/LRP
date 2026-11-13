#!/usr/bin/env python3
"""收集 ESP32 通过 WiFi 回传的日志行(UDP,默认端口 18999,广播/单播均可)。

用法: python3 scripts/collect_udp.py [端口]
配合板端: rdma_bench/rdma_peer --log-ip 255.255.255.255(默认) 或 --log-ip <本机IP>
"""
import datetime
import os
import socket
import sys

port = int(sys.argv[1]) if len(sys.argv) > 1 else 18999

os.makedirs("logs", exist_ok=True)
log_path = os.path.join("logs", f"collect_{datetime.datetime.now():%m%d_%H%M}.log")

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(("", port))

print(f"监听 UDP {port} (广播/单播均可),日志存档: {log_path}")
print("-" * 60)

with open(log_path, "w", encoding="utf-8") as log:
    while True:
        data, addr = sock.recvfrom(4096)
        line = data.decode("utf-8", "replace").rstrip("\n")
        ts = datetime.datetime.now().strftime("%H:%M:%S")
        out = f"[{ts}] [{addr[0]}] {line}"
        print(out, flush=True)
        log.write(out + "\n")
        log.flush()
