"""Summarise 一丢口语 trace logs produced by ORAL_TRACE in oral_screen.cc.

Usage:
    python analyze_oral_trace.py oral_cap_xxx.log
"""

import io
import re
import sys

LINE = re.compile(
    r"^(\d\d:\d\d:\d\d\.\d+) \[\+\s*([\d.]+)\] \[gap=\s*(\d+)\] ([IWE]) "
    r"\((\d+)\) ([^:]+): (.*)$"
)


def parse(path):
    rows = []
    for line in io.open(path, encoding="utf-8", errors="replace"):
        m = LINE.match(line.rstrip("\n"))
        if not m:
            continue
        rows.append(
            {
                "ts": m.group(1),
                "up": int(m.group(5)),
                "tag": m.group(6),
                "msg": m.group(7),
            }
        )
    start = 0
    for i in range(1, len(rows)):
        if rows[i]["up"] < rows[i - 1]["up"] - 2000:
            start = i
    return rows[start:]


def kv(msg):
    out = {}
    for tok in msg.split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            try:
                out[k] = int(v)
            except ValueError:
                out[k] = v
    return out


def sec(v):
    return "%7.2f s" % (v / 1000.0) if v else "      -"


def main(path):
    rows = parse(path)
    events = [r for r in rows if "[T]" in r["msg"]]
    ticks = [r for r in events if "tick " in r["msg"]]
    marks = [r for r in events if " ev=" in r["msg"]]
    warns = [r for r in events if "tick " not in r["msg"] and " ev=" not in r["msg"]]

    print("=" * 96)
    print("boot span %s -> %s   lines=%d  ticks=%d  events=%d"
          % (rows[0]["ts"], rows[-1]["ts"], len(rows), len(ticks), len(marks)))
    print("=" * 96)

    print("\n--- 轮次时间轴 ---")
    print("%-10s %-24s %10s %10s  %s"
          % ("时刻", "事件", "距rstart", "距rdone", "细节"))
    turn = {}
    for r in marks:
        m = kv(r["msg"])
        ev = str(m.get("ev", "?"))
        extra = ""
        if ev == "rstart":
            turn = {"rstart": r["up"]}
            extra = "上一轮done距今 %s ms, 上一轮未闭合=%s" % (
                m.get("prev_done_ago", 0), m.get("open", 0))
        elif ev == "first_audio":
            extra = "首包 +%s ms" % m.get("dt", 0)
        elif ev == "rdone":
            turn["rdone"] = r["up"]
            extra = ("队列 %s块/%sKB 仍在播=%s | 本轮收 %s包/%sB 最大间隔%sms "
                     "丢%sB 欠载%s次/%sms"
                     % (m.get("qn", 0), m.get("qkb", 0), m.get("pending", 0),
                        m.get("rxn", 0), m.get("rxb", 0), m.get("rxmax", 0),
                        m.get("drop", 0), m.get("gapn", 0), m.get("gapms", 0)))
        elif ev == "resume":
            turn["resume"] = r["up"]
        elif ev == "resume_timeout8s":
            extra = "8s 兜底超时"
        elif ev == "mic_on":
            extra = "开麦"
        elif ev == "ready":
            extra = "relay ready"
        since_r = sec(r["up"] - turn["rstart"]) if "rstart" in turn else ""
        since_d = sec(r["up"] - turn["rdone"]) if "rdone" in turn else ""
        print("%-10s %-24s %10s %10s  %s" % (r["ts"], ev, since_r, since_d, extra))

    hdr = ("%-10s %6s %8s %6s %6s %5s %6s %10s %10s"
           % ("时刻", "收包", "收字节", "慢包", "最大间隔", "丢弃",
              "队列KB", "上行max us", "欠载次/ms"))
    print("\n--- 逐秒（只列有异常的秒） ---")
    print(hdr)
    bad = 0
    for r in ticks:
        m = kv(r["msg"])
        if not (m.get("drop") or m.get("gapn") or m.get("rxslow")):
            continue
        bad += 1
        print("%-10s %6s %8s %6s %6s %5s %6s %10s %10s"
              % (r["ts"], m.get("rxn", 0), m.get("rxb", 0), m.get("rxslow", 0),
                 m.get("rxmax", 0), m.get("drop", 0), m.get("qkb", 0),
                 m.get("txmax", 0),
                 "%s/%s" % (m.get("gapn", 0), m.get("gapms", 0))))
    if bad == 0:
        print("（无）")

    print("\n--- 逐秒：队列与堆（有播放的秒） ---")
    print("%-10s %6s %6s %6s %10s %10s"
          % ("时刻", "收包", "队列块", "队列KB", "内部KB", "PSRAM KB"))
    for r in ticks:
        m = kv(r["msg"])
        if not m.get("rxn") and not m.get("qn"):
            continue
        print("%-10s %6s %6s %6s %10s %10s"
              % (r["ts"], m.get("rxn", 0), m.get("qn", 0), m.get("qkb", 0),
                 m.get("heapint", 0), m.get("heapps", 0)))

    tot = {"drop": 0, "gapn": 0, "gapms": 0, "rxslow": 0, "rxmax": 0,
           "txmax": 0, "txslow": 0, "gapmax": 0}
    for r in ticks:
        m = kv(r["msg"])
        for k in ("drop", "gapn", "gapms"):
            tot[k] += m.get(k, 0)
        tot["rxslow"] += m.get("rxslow", 0)
        tot["txslow"] += m.get("txslow", 0)
        for k in ("rxmax", "txmax", "gapmax"):
            tot[k] = max(tot[k], m.get(k, 0))

    print("\n--- 上行 Send 阻塞（>20ms 每次一条） ---")
    txs = sorted(int(kv(w["msg"]).get("us", 0)) for w in warns
                 if "tx_slow" in w["msg"])
    if txs:
        n = len(txs)
        print("次数 %d, 中位 %d ms, P90 %d ms, 最大 %d ms"
              % (n, txs[n // 2] // 1000, txs[int(n * 0.9)] // 1000,
                 txs[-1] // 1000))
        over100 = sum(1 for v in txs if v >= 100000)
        print("其中 >=100ms 的次数: %d（采集周期就是 100ms）" % over100)
    else:
        print("（无）")

    print("\n--- 下行到达间隔 >120ms ---")
    rxs = [int(kv(w["msg"]).get("dt", 0)) for w in warns if "rx_slow" in w["msg"]]
    if rxs:
        print("次数 %d, 最大 %d ms; 明细: %s"
              % (len(rxs), max(rxs), rxs[:20]))
    else:
        print("（无）")

    print("\n--- 播放欠载结束事件 ---")
    gaps = [int(kv(w["msg"]).get("dur", 0)) for w in warns if "gap_end" in w["msg"]]
    if gaps:
        print("次数 %d, 明细: %s ms" % (len(gaps), gaps))
    else:
        print("（无）")

    print("\n--- 总计 ---")
    print("下行慢包(>=120ms) %d 次, 最大到达间隔 %d ms"
          % (tot["rxslow"], tot["rxmax"]))
    print("丢弃旧音频 %d B, 播放欠载 %d 次 / 共 %d ms (单次最长 %d ms)"
          % (tot["drop"], tot["gapn"], tot["gapms"], tot["gapmax"]))
    print("上行 Send 最大 %d us, 慢发送 %d 次" % (tot["txmax"], tot["txslow"]))

    print("\n--- 判定 ---")
    if tot["drop"] or tot["gapn"]:
        print("端侧缓冲被击穿：既有丢包也有欠载 -> 先改 jitter buffer + 接收优先级。")
    elif tot["rxslow"] or tot["rxmax"] > 150:
        print("下行到达抖动大但没击穿缓冲 -> 带宽/接收任务优先级是主因，先压带宽。")
    else:
        print("音频流本身平稳 -> 卡顿来自轮次切换与首包延迟，不是音质问题。")


if __name__ == "__main__":
    main(sys.argv[1])
