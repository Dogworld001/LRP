#!/usr/bin/env python3
"""生成《无线 RDMA 实机验证记录表》PDF —— A4 三页,可直接打印手写填写。"""
from reportlab.lib import colors
from reportlab.lib.pagesizes import A4
from reportlab.lib.units import mm
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.cidfonts import UnicodeCIDFont
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.lib.styles import ParagraphStyle
from reportlab.platypus import (Paragraph, SimpleDocTemplate, Spacer, Table,
                                TableStyle, PageBreak)

OUT = "RDMA实机验证记录表.pdf"


def setup_fonts():
    try:
        pdfmetrics.registerFont(TTFont("CN", "/System/Library/Fonts/STHeiti Light.ttc",
                                       subfontIndex=0))
        pdfmetrics.registerFont(TTFont("CNB", "/System/Library/Fonts/STHeiti Medium.ttc",
                                       subfontIndex=0))
        return "CN", "CNB"
    except Exception:
        pdfmetrics.registerFont(UnicodeCIDFont("STSong-Light"))
        return "STSong-Light", "STSong-Light"


CN, CNB = setup_fonts()

st_title = ParagraphStyle("t", fontName=CNB, fontSize=17, leading=22, spaceAfter=2)
st_sub = ParagraphStyle("s", fontName=CN, fontSize=9.5, leading=13,
                        textColor=colors.HexColor("#444444"), spaceAfter=6)
st_h2 = ParagraphStyle("h2", fontName=CNB, fontSize=11.5, leading=15, spaceBefore=8,
                       spaceAfter=3, textColor=colors.HexColor("#1F2937"))
st_cell = ParagraphStyle("c", fontName=CN, fontSize=9, leading=12)
st_cellb = ParagraphStyle("cb", fontName=CNB, fontSize=9, leading=12)
st_note = ParagraphStyle("n", fontName=CN, fontSize=8.5, leading=11,
                         textColor=colors.HexColor("#555555"))

GRAY = colors.HexColor("#EDEDED")
LINE = colors.HexColor("#9A9A9A")
W = 180 * mm


def P(t, s=st_cell):
    return Paragraph(t, s)


def grid_table(data, col_widths, row_heights=None, header=True, font_size=9):
    t = Table(data, colWidths=col_widths, rowHeights=row_heights, hAlign="LEFT")
    style = [
        ("GRID", (0, 0), (-1, -1), 0.6, LINE),
        ("VALIGN", (0, 0), (-1, -1), "MIDDLE"),
        ("LEFTPADDING", (0, 0), (-1, -1), 4),
        ("RIGHTPADDING", (0, 0), (-1, -1), 4),
    ]
    if header:
        style += [("BACKGROUND", (0, 0), (-1, 0), GRAY)]
    t.setStyle(TableStyle(style))
    return t


def footer(canvas, doc):
    canvas.saveState()
    canvas.setFont(CN, 8)
    canvas.setFillColor(colors.HexColor("#666666"))
    canvas.drawString(15 * mm, 10 * mm, "无线 RDMA(LRP) 实机验证记录表 · SSID: dwrdma_test")
    canvas.drawRightString(195 * mm, 10 * mm, f"第 {doc.page} 页 / 共 3 页")
    canvas.restoreState()


doc = SimpleDocTemplate(OUT, pagesize=A4, leftMargin=15 * mm, rightMargin=15 * mm,
                        topMargin=14 * mm, bottomMargin=16 * mm,
                        title="无线 RDMA 实机验证记录表", author="LRP")
story = []

# ================= 第 1 页 =================
story.append(P("无线 RDMA 实机验证记录表", st_title))
story.append(P("LRP 协议 · 2×ESP32-S3 对等组网 · 打印后手写记录 · 日志留存:串口 monitor_dual 或 WiFi collect_udp(远距推荐)", st_sub))

story.append(P("一、基本信息", st_h2))
story.append(grid_table(
    [[P("实验日期", st_cellb), P("", ), P("实验人", st_cellb), P(""),
      P("固件构建(日期/角色)", st_cellb), P("")],
     [P("场地/天气", st_cellb), P(""), P("路由器/信道", st_cellb), P(""),
      P("供电方式", st_cellb), P("[  ]充电宝 [  ]USB")],
     [P("日志回传方式", st_cellb), P("[  ]串口  [  ]WiFi(远距)"),
      P("实测距离 A↔B (m)", st_cellb), P(""),
      P("距路由器 A/B (m)", st_cellb), P("")]],
    [22 * mm, 38 * mm, 22 * mm, 33 * mm, 30 * mm, 35 * mm],
    row_heights=[11 * mm, 11 * mm, 11 * mm], header=False))

story.append(P("二、环境与节点元数据(开机串口会打印 node-id 与 IP)", st_h2))
story.append(grid_table(
    [[P("项目", st_cellb), P("板 A", st_cellb), P("板 B", st_cellb), P("板 C(如有)", st_cellb)],
     [P("角色(peer/bench)")], [P("node-id (0x…)")], [P("IP 地址")],
     [P("RSSI (dBm)")], [P("距路由器(m)/遮挡")]],
    [46 * mm, 44 * mm, 44 * mm, 46 * mm],
    row_heights=[8 * mm] + [8.5 * mm] * 5))

story.append(P("三、性能矩阵(bench 串口输出抄录;轮次 2 填 PSK 口令,轮次 3 填限速值)", st_h2))
story.append(grid_table(
    [[P("指标", st_cellb), P("轮次 1: 明文<br/>距离___m", st_cellb),
      P("轮次 2: PSK=____<br/>距离___m", st_cellb), P("轮次 3: 限速____kbps<br/>距离___m", st_cellb)],
     [P("8B 往返 Write (avg/P50/P99 µs)")],
     [P("8B 往返 Send (avg/P50/P99 µs)")],
     [P("8B 往返 Read (avg/P50/P99 µs)")],
     [P("8B 往返 Atomic (avg/P50/P99 µs)")],
     [P("1KB Write 吞吐 (MB/s)")],
     [P("传输成功率 (%)")],
     [P("备注")]],
    [56 * mm, 41 * mm, 41 * mm, 42 * mm],
    row_heights=[8 * mm] + [10.5 * mm] * 7))
story.append(P("安全开销结论:延迟增幅 ____%  吞吐降幅 ____%   限速贴合度:实测 ____ vs 设定 ____   远距结论(丢包/恢复现象): __________", st_note))

story.append(PageBreak())

# ================= 第 2 页 =================
story.append(P("四、零配置组网与对等互访(peer 模式,串口时间戳为准)", st_h2))
story.append(grid_table(
    [[P("观察项", st_cellb), P("记录", st_cellb), P("判据", st_cellb), P("通过", st_cellb)],
     [P("板 A 上电 → 入网耗时")], [P("板 B 上电 → 入网耗时")],
     [P("板 C 迟到 30s 上电 → 全网互见")],
     [P("心跳剔除(关机 __s 后剔除)")],
     [P("重新上电 → 重入网耗时")],
     [P("node-id 是否自动生成(免配置)")]],
    [58 * mm, 52 * mm, 46 * mm, 24 * mm],
    row_heights=[8 * mm] + [9.5 * mm] * 6))
story.append(P("判据:入网 < 10s;心跳剔除 ≈15s;重入网 < 20s;「免配置」须为是。", st_note))

story.append(P("稳态对等互访记录(抄录串口 10s 状态表:ops / rtt_avg / 远端计数)", st_h2))
story.append(grid_table(
    [[P("时刻", st_cellb), P("对端", st_cellb), P("ops", st_cellb),
      P("rtt_avg (µs)", st_cellb), P("远端计数(应持续增长)", st_cellb)],
     [P("")], [P("")], [P("")], [P("")]],
    [26 * mm, 26 * mm, 26 * mm, 36 * mm, 66 * mm],
    row_heights=[8 * mm] + [9 * mm] * 4, header=True))

story.append(P("五、可靠性与恢复场景(每行填操作条件/现象/恢复耗时)", st_h2))
story.append(grid_table(
    [[P("场景", st_cellb), P("操作条件", st_cellb), P("观察现象", st_cellb),
      P("恢复耗时", st_cellb), P("通过", st_cellb)],
     [P("远距离/丢包(距离__m/遮挡/干扰)")],
     [P("闪断(断离 AP ____s 再回)")],
     [P("重启续传(重启一块板)")],
     [P("心跳剔除与重新入网)")],
     [P("多节点混合故障(自拟)")]],
    [46 * mm, 38 * mm, 52 * mm, 24 * mm, 20 * mm],
    row_heights=[8 * mm] + [12 * mm] * 5))
story.append(P("重启续传必查:epoch 变化日志出现;「写」被判定完成;原子操作未重复执行(远端计数不多加)。", st_note))

story.append(PageBreak())

# ================= 第 3 页 =================
story.append(P("六、验收判据核对表", st_h2))
crit = [
    "四操作(Write/Send/Read/Atomic)全部成功",
    "对等组网成功,身份自动生成、无需预配置",
    "8B 往返延迟 < 10 ms(空口主导)",
    "加密开销:小帧延迟增幅 < 50%(吞吐降幅已记录)",
    "限速贴合:实测吞吐 = 设定值 ±10%",
    "闪断自愈 < 3 s,无永久性链路报废",
    "重启续传:无重复原子执行,读写语义符合规格",
    "心跳剔除 + 重新入网 < 20 s",
]
rows = [[P("判据", st_cellb), P("实测结果", st_cellb), P("通过", st_cellb)]]
for c in crit:
    rows.append([P(c), P(""), P("[  ]")])
story.append(grid_table(rows, [106 * mm, 52 * mm, 22 * mm],
                        row_heights=[8 * mm] + [10 * mm] * len(crit)))

story.append(P("七、问题 / 异常 / 有趣现象记录(现象、复现步骤、初步判断)", st_h2))
blank = [[P("")] for _ in range(6)]
story.append(grid_table(blank, [180 * mm], row_heights=[9.5 * mm] * 6, header=False))

story.append(P("八、结论与签署", st_h2))
story.append(grid_table(
    [[P("总体结论", st_cellb),
      P("[  ] 通过      [  ] 有条件通过      [  ] 不通过")],
     [P("遗留事项 / 下一步", st_cellb), P("")],
     [P("记录人 / 日期", st_cellb), P("")]],
    [40 * mm, 140 * mm], row_heights=[10 * mm, 14 * mm, 10 * mm], header=False))
story.append(Spacer(1, 4 * mm))
story.append(P("数据留存:WiFi 回传 python3 scripts/collect_udp.py 自动存 logs/collect_*.log(远距免连线);<br/>串口联调 monitor_dual.sh 存 logs/boardX_*.log;摆位拍照标注距离/遮挡。", st_note))

doc.build(story, onFirstPage=footer, onLaterPages=footer)
print("generated:", OUT)
