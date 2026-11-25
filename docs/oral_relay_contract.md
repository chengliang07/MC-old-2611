# 一丢口语 relay（oral-coach-relay-probe）接口约定

> 状态：2026-11-25 14:30 更新，反映**已实施**的方案。
> 上一版（20 ms 分片 / Opus / `response.done` ≤100 ms）是当时的设想，**没有实施**，
> 不要再照着它改。

## 一、原始问题（已解决，留作背景）

2026-11-25 凌晨实测：下行 15360 B/包、间隔 1–7 s，有效音频速率仅 0.32×，
播放端 2.5 分钟内欠载 27 次。根因不在设备：**relay 攒包 + 上行 Send 阻塞**。

## 二、现在实际怎么跑

### 下行音频：按播放速率投放，不截断

模型是**一口气把整段回复合成完**再发的（14 s 音频可在 2 s 内到齐）。relay 收到后：

```
audioQueue（Uint8Array[]）→ 20 ms 定时泵 → 设备
```

- 投放速率 **1.1× 实时**（`PACE_FACTOR`）。1.0× 时设备每秒饿 2–3 次；1.25× 又浪费。
- 积压上限 **30 s**。**绝不因为积压而丢片** —— 曾经的 1.2 s 上限把长句砍掉 3/4
  （实测 44 词只剩 3.68 s 音频），用户听感是"读得飞快、一句吞完"。
- `response.done` **等队列放空再发**，否则设备会提前恢复聆听、丢掉最后几片。
- `interrupt` 时清空队列。

实测：分片间隔 378–384 ms（≈1.1×），50 词 / 16.7 s = 2.99 词/秒（正常语速）。

### 下行文本：逐字转写优先

模型可能同时发两条文本通道：

| 通道 | 内容 |
|---|---|
| `response.text.delta` | 摘要式文本 |
| `response.audio_transcript.delta` | 与音频严格同序的逐字转写 |

规则：`response.text.delta` 先缓存 200 ms，一旦出现转写通道就整段丢弃、只转发转写。
两路混发会导致**字幕顺序与语音不一致**（已发生过）。

### 上行音频

裸 PCM 16 kHz，100 ms/帧，端侧解耦（`s_tx_chunks` 队列 + 独立 `tx_task`），
relay 侧不做特殊处理。

### 错误与调试

- 上游错误（`code`/`message`/`request_id`）**原样转发**为 `relay.error`，
  端上 `ESP_LOGE("relay error: ...")` 打印原文。`no active response` 视为噪音忽略。
- 调试参数（只影响单次连接）：`?voice=` `?model=` `?probe_text=` `?say_text=` `?speed=`
- `?say_text=` 会让模型逐字复读指定句子、temperature 0，用于**客观测量语速**
  （配合 `scripts/f0_probe.py` 查采样率是否错配）。

## 三、不要做的事

1. **不要为了"平滑"丢音频。** 队列是唯一正确的缓冲手段。
2. **不要用 1.0× 投放。** 设备会持续欠载。
3. **不要混用两条文本通道。**
4. **不要在没有音频排空时就发 `response.done`。**
5. 不要把 `response.created` 的放行条件只写成"有用户转写"：audio-3.1 不产生
   输入转写事件，必须同时接受 `input_audio_buffer.speech_stopped`。

## 四、端侧配套（已实现，供对照）

| 端侧行为 | 值 |
|---|---|
| 下行缓冲上限 | 256 KB / 32 块（≈8 s），正常只用 0.3–1 s |
| 轮次看门狗 | 8 s 无音频则重开麦（兜底，正常不触发） |
| 打断上行静音窗 | 300 ms（覆盖 I2S DMA 余量 + 扬声器尾音，防自问自答） |
| 气泡重排节流 | 80 ms/次（每个 delta 都重排会拖慢音频） |

## 五、验收方法

设备端内置打点（`ORAL_TRACE`，日志 tag `OralScreen: [T]`）：

```
ev=rstart / ev=first_audio dt=…   → 首包 0.5–0.8 s 为正常
tick rxn=… rxb=…                  → 每秒字节数 ≈ 48000（1 s 音频）
rdone … qn=0 qkb=0                → 结束时队列为空
gapn / gapms                      → 应接近 0；持续 >1 次/秒说明投放速率不足
drop=…                            → 必须为 0
```

服务端侧用 `scripts/relay_probe.js`：

```
node scripts/relay_probe.js 25 --text "..." [--voice X] [--wav out.wav]
→ -- audio cadence avg=380ms          （应为 350–400 ms）
→ -- transcript words=N audioSec=M wps=…   （应为 2.5–3.2 词/秒；偏高说明丢片）
```
