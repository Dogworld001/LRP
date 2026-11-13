#!/usr/bin/env python3
"""受信任设备管理工具:向对等节点下发 SRCTL 指令(限速/测量/查询)。

用法:
  python3 scripts/ctl.py <节点IP> rate <kbps>      # 限速(0=不限)
  python3 scripts/ctl.py <节点IP> bench [lat] [kb] # 性能测量(默认 100 次 / 32KB)
  python3 scripts/ctl.py <节点IP> stats            # 立即报表
选项: --port <数据端口,默认18515> --key <管理密钥,默认 lrp-admin>
"""
import socket
import sys

args = sys.argv[1:]
if len(args) < 2:
    print(__doc__)
    sys.exit(1)
ip = args[0]
cmd = args[1]
port = 18515
key = "lrp-admin"
rest = []
i = 2
while i < len(args):
    if args[i] == "--port":
        port = int(args[i + 1]); i += 2
    elif args[i] == "--key":
        key = args[i + 1]; i += 2
    else:
        rest.append(args[i]); i += 1

if cmd == "rate":
    if not rest:
        print("rate 需要数值(kbps)"); sys.exit(1)
    msg = f"SRCTL key={key} cmd=rate val={rest[0]}"
elif cmd == "bench":
    lat = rest[0] if len(rest) > 0 else "100"
    kb = rest[1] if len(rest) > 1 else "32"
    msg = f"SRCTL key={key} cmd=bench val={lat} val2={kb}"
elif cmd == "stats":
    msg = f"SRCTL key={key} cmd=stats"
else:
    print(f"未知指令: {cmd}"); sys.exit(1)

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.sendto(msg.encode(), (ip, port + 100))
print(f"已下发 → {ip}:{port + 100}: {msg}")
print("(节点确认与结果经 WiFi 回传,请开 scripts/collect_udp.py 查看)")
