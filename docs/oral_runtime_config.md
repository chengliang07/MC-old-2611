# 一丢口语 线上配置与回滚

> 最后更新：2026-11-25 14:30
> relay 源码在**另一个仓库**：`01-word-memory-web/supabase/functions/oral-coach-relay-probe/`
> 项目：`oemsbxavqkmtuyasxsba`（word-memory-prod，生产）

## 一、当前生效配置

| 项 | 值 | 位置 |
|---|---|---|
| 模型 | `qwen-audio-3.1-realtime-plus` | secret `QWEN_REALTIME_MODEL` |
| 音色 | `beth_v3.1` | secret `ORAL_COACH_VOICE` |
| 允许音色白名单 | `beth_v3.1,annie_v3.1,hannah,sherry,emily_v3.1,daniel` | secret `ORAL_COACH_ALLOWED_VOICES` |
| 地域 | `cn`（北京） | secret `BAILIAN_REALTIME_REGION` |
| 语速 | — | `ORAL_COACH_SPEED=1.25` **实际无效**（模型忽略 `audio.output.speed`，可删） |
| 调试 token | 已设 | secret `ORAL_COACH_PROBE_TOKEN` |

代码默认值（secret 缺失时生效）：模型 `qwen3.8-omni-flash-realtime`、音色 `Tina`、地域 `cn`。

> 音色名是**模型相关**的：omni 用 `Tina/Serena/Mia/Raymond/Zane`，
> audio-3.1 用 CosyVoice 那套（`long*`/`loong*`/`hannah`/`daniel`/`beth_v3.1`…）。
> 传错音色会在 `response.create` 阶段报 `invalid_value` 并列出全部可用音色。

## 二、回滚到旧模型（一条命令）

```bash
node -e "..."   # 见下，或直接用 scripts/supabase_secrets.js
```

用仓库里的脚本更稳（读取 `.gates/supabase.pat` 里的 PAT）：

```bash
node scripts/supabase_secrets.js set QWEN_REALTIME_MODEL qwen3.8-omni-flash-realtime
node scripts/supabase_secrets.js set ORAL_COACH_VOICE Tina
node scripts/supabase_secrets.js set ORAL_COACH_ALLOWED_VOICES Tina,Serena,Mia,Raymond,Zane
```

改 secret 后 Supabase 会自动重启函数，**约 10 秒生效，设备无需重刷**。
（设备侧无需任何改动，模型和音色都是 relay 侧配置。）

## 三、换音色 / 换模型的流程

1. 先试音色，别直接改生产：

```bash
# 生成同句样本，直接对比音色和语速
node scripts/relay_probe.js 20 --voice <候选> --text "read" \
  --say-text "The quick brown fox jumps over the lazy dog every single morning." \
  --wav .gates/voice_samples/<候选>.wav
```

2. 检查是否 24 kHz（防止采样率错配导致变速）：

```bash
python scripts/f0_probe.py .gates/voice_samples/<候选>.wav
# 女声 ~200 Hz、男声 ~120 Hz 为正常；>300 Hz 说明被 1.5× 加速
```

3. 确认后写 secret，再用 `scripts/relay_probe.js` 不带 `--voice` 验一次默认值生效。

## 四、部署 relay

```bash
cd E:/09-Soft_DEV/01-word-memory-web
set SUPABASE_ACCESS_TOKEN=<PAT>
npx supabase@3.4.0 functions deploy oral-coach-relay-probe \
  --remote=oemsbxavqkmtuyasxsba --use-api --yes
```

注意：Windows PowerShell 里 `npx` 会把正常输出写成 stderr，`$LASTEXITCODE=1`
不一定是失败，要看日志里有没有 `Deployed Functions on project ...`。

## 五、故障速查

| 现象 | 先查 |
|---|---|
| 界面弹错误原文 | 看 `relay error:` 里的 `code: message`，模型未开通/额度/音色非法都会原样透出 |
| AI 不出声 | `relay.ready` 的 `voice=` 是否是当前模型的合法音色 |
| 说两句就"等待唤醒" | 端侧 `response_says_goodbye()`（已收紧为仅短句道别才结束） |
| 读得飞快、一句吞完 | relay 是否又加了"积压丢弃"；用 `--say-text` 量 wps，>3.5 就是丢片 |
| 字幕与语音不同序 | 两条文本通道是否被混发（必须逐字转写优先） |
| 打断后自问自答 | 端侧上行静音窗（当前 300 ms）是否被改小 |
