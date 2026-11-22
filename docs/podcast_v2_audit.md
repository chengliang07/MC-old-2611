# Podcast V2 设备端播放链路审计

日期：2026-11-22

## 1. 范围与约束

- 后台仍是唯一内容入口：一丢教育 Web、RSS 导入、MP3 上传、审核发布均不变。
- 设备只访问现有 Vercel API，不直连 Supabase，不解析 RSS。
- 保持 `PodcastItem`、`PodcastPlayInfo`、NVS 中的 `api_base` / `device_token`、列表缓存和现有 UI 不变。
- 不改电台、小智语音链路。
- 旧播放器源码保留，切换期间不删除，便于回退。

## 2. 现有链路

`podcast_screen.cc`

- `PodcastClient::FetchPodcasts` 拉列表。
- 点击节目后 `PodcastClient::ResolvePlayUrl` 获取 `/play` 返回的临时地址。
- `PodcastPlayer::Start(url)` 启动播放。

`podcast_player.cc`（旧实现）

- `PodcastFetchTask` 用 `esp_http_client` 拉流，写入 96 × 16KB（约 1.5MB PSRAM）环形缓冲。
- `PodcastReadCallback` 是阻塞式回调，缓冲区空时最多等待 500ms。
- `PodcastOutCallback` 做声道下混、倍速重采样并输出到 `AudioCodec`。
- 断流时最多 8 次 Range 续传。
- 进度按“基准毫秒 + 墙钟 × 倍速”估算。

## 3. 已确认的问题

1. 阻塞式读回调位于 GMF pipeline 的输入端口，缓冲不足时会同时卡住解码和 PCM 输出，表现为每几秒一次卡顿。
2. 1.5MB 环形缓冲常驻 PSRAM，叠加 TLS、语音会话和 LVGL 后内存压力大，长会话后容易出现聆听无响应。
3. 进度按墙钟估算，网络停顿或重连后会与声音脱节。
4. 播放地址请求和音频打开都在点击后串行发生，首播等待时间偏长。

## 4. GMF 能力与限制（已读源码）

组件版本：

- `esp_audio_simple_player 0.9.6~1`
- `gmf_io 0.8.1`

可用能力：

- `http_io_cfg_t.event_handle` 在 `HTTP_STREAM_PRE_REQUEST` 时可直接操作 `esp_http_client`，可补 `Authorization`、`User-Agent` 和 `Range`。
- `esp_gmf_io_seek()` 会先 `esp_gmf_io_set_pos()`，HTTP IO 的 `_http_open()` 再据此发送 `Range: bytes=N-`，seek 可用。
- `esp_gmf_io_get_speed_stats()` 可拿到 IO 实际读取字节数；`esp_gmf_io_get_db_filled_size()` 可拿到尚未消费的缓冲字节。
- 读取失败时 IO 最多自动重连 5 次，成功读取后计数清零。

限制：

- 默认注册的 `io_http` 不带证书回调和鉴权回调。
- 同 tag 注册时池按链表第一个匹配，直接再注册一个 `io_http` 不会替换默认实例。
- HTTP IO 只跟随 301/302，不处理 307/308。
- 重连时 IO 框架不会自动更新 `pos`，必须由事件回调根据实际读取字节补 `Range`。
- 没有直接暴露播放时长或解码位置，进度仍需按字节比例换算。

## 5. V2 方案

1. 使用 `esp_audio_simple_player_run_to_end()` 直接播放 `/play` 返回的 HTTPS URL，输入不再走 `raw://`。
2. 注册独立 tag 的 HTTP IO（`io_http_podcast`），评分高于默认 `io_http`，确保 pipeline 选中带鉴权和证书配置的实例。
3. `HTTP_STREAM_PRE_REQUEST` 中补：
   - `Authorization: Bearer <device_token>`（仅播客域名，发音 URL 不加）。
   - `User-Agent`（发音 URL 需要）。
   - `Range: bytes=<seek_offset + 已消费字节>-`；已消费位置取
     `esp_gmf_io_get_info().pos`，不能用网络已读字节，否则重连会跳过缓冲内未播放的音频。
4. 开启 IO speed monitor，仅用于进度显示；断流续传位置以 IO 的 `pos` 为准。
5. 保留 `PodcastOutCallback` 的下混、倍速和 `AudioCodec` 输出；删除阻塞式 `PodcastReadCallback`、`PodcastFetchTask`、`StreamState`、`RingBlock`。
6. 暂停/恢复继续走独立控制任务；`stop` 也放到后台任务执行，避免管线退出（最长等 HTTP 读超时）
   阻塞 LVGL 线程。后台 stop 与播放器销毁用生命周期锁串行化，防止 use-after-free。
7. Seek 继续采用“停旧播放器 + 用新偏移重建播放器”，由 `Range` 从目标字节开始。

## 6. 等价能力

- 播放/暂停/恢复/停止：`esp_audio_simple_player_run_to_end` + `pause` / `resume` / `stop`。
- 进度：`duration × (seek_offset + 已消费字节) / 总字节`；总字节从 `Content-Range` 或 `Content-Length` 获取。
- 倍速：保留 PCM 重采样输出回调。
- 断流续传：事件回调用 `esp_gmf_io_get_info().pos` 回填 Range（已消费位置）。
- 返回来源屏：不改 `podcast_screen.cc` 的 `return_screen` 逻辑。

## 7. 风险与回退

- 代理若忽略 Range 返回 200，seek 会从头播放；首版接受，后续由后台代理保证 Range 透传。
- 外部音源经 302 跳转时，`Authorization` 可能随跳转带到第三方；仅影响外部 RSS 源，Supabase 节目不受影响。后续后台统一走 `/stream` 代理可消除。
- 20KB IO 缓冲小于旧 1.5MB，极端弱网下抗抖动能力下降；先以默认值验证，必要时只增大 IO 缓冲，不恢复自建环形缓冲。
- `stop` 最长需等一次 HTTP 读超时（8s）才返回，但已在后台任务执行，不阻塞 UI；若极端情况下
  15s 仍未退出，保留播放器不销毁以避免 use-after-free，代价是泄漏一个管线实例。
- 回退点：`main/display/screen/podcast_screen/podcast_player.cc` 保留旧实现；将 `main/CMakeLists.txt` 的源文件切回旧文件即可。
