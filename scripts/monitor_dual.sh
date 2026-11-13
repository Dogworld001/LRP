#!/usr/bin/env bash
# 双串口合并监控:两块 ESP32 同时输出到一个终端,并分别存日志。
# 用法:
#   ./scripts/monitor_dual.sh                      # 列出候选串口设备
#   ./scripts/monitor_dual.sh <板A设备> <板B设备>   # 例如 /dev/tty.usbserial-0001 /dev/tty.usbserial-0002
set -u

if [ $# -lt 2 ]; then
  echo "候选串口设备:"
  ls /dev/tty.usb* /dev/tty.wchusb* 2>/dev/null || echo "  (未发现,请插好 USB 数据线)"
  echo
  echo "用法: $0 <板A设备> <板B设备>"
  echo "示例: $0 /dev/tty.usbserial-0001 /dev/tty.usbserial-0002"
  exit 1
fi

DEV_A=$1
DEV_B=$2
BAUD=115200
[ -e "$DEV_A" ] || { echo "设备不存在: $DEV_A"; exit 1; }
[ -e "$DEV_B" ] || { echo "设备不存在: $DEV_B"; exit 1; }

mkdir -p logs
STAMP=$(date +%m%d_%H%M)
LOG_A="logs/boardA_${STAMP}.log"
LOG_B="logs/boardB_${STAMP}.log"

stty -f "$DEV_A" "$BAUD" raw -echo 2>/dev/null || stty -F "$DEV_A" "$BAUD" raw -echo
stty -f "$DEV_B" "$BAUD" raw -echo 2>/dev/null || stty -F "$DEV_B" "$BAUD" raw -echo

echo "监控中: A=$DEV_A -> $LOG_A ; B=$DEV_B -> $LOG_B (Ctrl+C 退出)"
echo "------------------------------------------------------------"

tail -n +1 -f "$DEV_A" | while IFS= read -r line; do
  printf '[%s] [A] %s\n' "$(date +%H:%M:%S)" "$line"
  printf '[%s] [A] %s\n' "$(date +%H:%M:%S)" "$line" >> "$LOG_A"
done &
PID_A=$!

tail -n +1 -f "$DEV_B" | while IFS= read -r line; do
  printf '[%s] [B] %s\n' "$(date +%H:%M:%S)" "$line"
  printf '[%s] [B] %s\n' "$(date +%H:%M:%S)" "$line" >> "$LOG_B"
done &
PID_B=$!

trap 'kill $PID_A $PID_B 2>/dev/null; echo; echo "日志已保存: $LOG_A / $LOG_B"; exit 0' INT TERM
wait
