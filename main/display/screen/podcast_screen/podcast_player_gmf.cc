#include "podcast_player.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <strings.h>
#include <vector>

#include "audio_codec.h"
#include "board.h"
#include "esp_audio_simple_player.h"
#include "esp_audio_simple_player_advance.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_gmf_io_http.h"
#include "esp_gmf_obj.h"
#include "esp_gmf_pipeline.h"
#include "esp_gmf_pool.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "media_lib_adapter.h"
#include "podcast_client.h"
#include "system_info.h"
#include "application.h"

#ifdef CONFIG_ESP_AUDIO_SIMPLE_PLAYER_RESAMPLE_EN
#include "esp_gmf_rate_cvt.h"
#endif

namespace {

constexpr const char* TAG = "PodcastPlayer";
constexpr int kOutputSampleRate = 16000;
// 独立 tag：池按 tag 取第一个实例，不能直接复用默认的 "io_http"。
constexpr const char* kPodcastHttpTag = "io_http_podcast";

struct PlayArg {
    uint32_t generation = 0;
    std::string url;
    int64_t range_bytes = 0;
    int64_t base_ms = 0;
    int64_t click_us = 0;
    bool prepare = false;
};

struct PlayerContext {
    AudioCodec* codec = nullptr;
    esp_asp_handle_t player = nullptr;
    uint32_t generation = 0;
};

// 播客自己占着喇叭：进播放任务时接管 AudioService 的 15s 省电检查，
// 任务退出（正常结束/出错/被停止）时自动交还，避免中途被静音且无法恢复。
// 预播（prepare）期间播放任务同样存活着，所以这里不区分 prepare。
struct ExternalPlaybackGuard {
    ExternalPlaybackGuard() {
        Application::GetInstance().GetAudioService().SetExternalPlayback(true);
    }
    ~ExternalPlaybackGuard() {
        Application::GetInstance().GetAudioService().SetExternalPlayback(false);
    }
    ExternalPlaybackGuard(const ExternalPlaybackGuard&) = delete;
    ExternalPlaybackGuard& operator=(const ExternalPlaybackGuard&) = delete;
};

std::mutex s_player_mutex;
std::mutex s_api_mutex;
esp_asp_handle_t s_player = nullptr;
TaskHandle_t s_play_task = nullptr;
std::atomic<uint32_t> s_generation{0};
std::atomic<int> s_pcm_channels{2};
std::atomic<bool> s_output_enabled{false};
std::atomic<bool> s_reported_playing{false};
std::atomic<bool> s_start_gate_done{false};
std::atomic<uint32_t> s_output_generation{0};
PodcastPlayerEventCallback s_event_cb = nullptr;
void* s_event_ctx = nullptr;
std::vector<int16_t> s_pcm_buf;
std::vector<int16_t> s_pcm_out;
bool s_media_lib_ready = false;

// ---- 进度 / 倍速 ----------------------------------------------------------
// 位置优先按“已消费音频字节 / 总字节”换算；总字节未知时退回墙钟估算。
std::atomic<int64_t> s_range_offset{0};
std::atomic<int64_t> s_total_bytes{0};
// 已消费的压缩字节：live 是当前连接的实时值，committed 是之前连接累计值。
// 断流重连时 GMF 的 info.pos 不会随读取推进，只能靠自己记这份账喂 Range，
// 否则每次都从 bytes=0 重播。
std::atomic<int64_t> s_consumed_live{0};
std::atomic<int64_t> s_consumed_committed{0};
std::atomic<int64_t> s_base_ms{0};
std::atomic<int64_t> s_clock_us{0};
std::atomic<int> s_speed_q1000{1000};
std::atomic<int> s_duration_sec{0};
std::atomic<int> s_estimated_duration_sec{0};
std::atomic<bool> s_quick_url{false};
std::atomic<bool> s_headers_parsed{false};
std::atomic<uint32_t> s_first_pcm_generation{0};
std::atomic<uint32_t> s_url_expired_generation{0};
// 预播状态：进入播客页先跑通 HTTP 打开/解码，首帧到达后在输出回调里按住，
// 用户点播时放开即可，省掉点击后 2-5 秒的连接与首包等待。
std::atomic<bool> s_prepare_mode{false};
std::atomic<bool> s_prepare_play{false};
std::atomic<uint32_t> s_prepare_generation{0};
// stop 会阻塞到管线线程退出（最长等 HTTP 读超时），不能在 UI 线程里调用。
// 生命周期锁只保护“正在后台 stop 的播放器指针”，绝不能跨越 stop 调用持有。
std::mutex s_lifecycle_mutex;
std::condition_variable s_stop_cv;  // 后台 stop 完成时唤醒等待销毁的一方
esp_asp_handle_t s_pending_stop_player = nullptr;
std::atomic<esp_gmf_io_handle_t> s_http_io{nullptr};
std::atomic<uint32_t> s_http_io_generation{0};
std::atomic<bool> s_http_capture_allowed{false};
std::mutex s_io_mutex;
std::string s_current_url;

// ---- 全链路耗时诊断 ------------------------------------------------------
std::atomic<uint32_t> s_timing_generation{0};
std::atomic<int64_t> s_timing_gen_start_us{0};
std::atomic<uint32_t> s_timing_click_generation{0};
std::atomic<int64_t> s_timing_click_us{0};

void MarkTimingStart(uint32_t generation, int64_t click_us) {
    s_timing_generation.store(generation, std::memory_order_release);
    s_timing_gen_start_us.store(esp_timer_get_time(),
                                std::memory_order_release);
    s_timing_click_generation.store(generation, std::memory_order_release);
    s_timing_click_us.store(click_us, std::memory_order_release);
}

void TimingLog(uint32_t generation, const char* stage,
               const char* extra = nullptr) {
    const int64_t now = esp_timer_get_time();
    int64_t dt_ms = -1;
    int64_t since_click_ms = -1;
    if (s_timing_generation.load(std::memory_order_acquire) == generation) {
        const int64_t start =
            s_timing_gen_start_us.load(std::memory_order_relaxed);
        if (start > 0) {
            dt_ms = (now - start) / 1000;
        }
    }
    if (s_timing_click_generation.load(std::memory_order_acquire) ==
        generation) {
        const int64_t click =
            s_timing_click_us.load(std::memory_order_relaxed);
        if (click > 0) {
            since_click_ms = (now - click) / 1000;
        }
    }
    ESP_LOGI(TAG, "[timing] gen=%u stage=%s dt=%dms click=%dms%s%s",
             static_cast<unsigned>(generation), stage,
             static_cast<int>(dt_ms),
             static_cast<int>(since_click_ms),
             extra != nullptr ? " " : "", extra != nullptr ? extra : "");
}

void ClearHttpIo(uint32_t generation) {
    std::lock_guard<std::mutex> lock(s_io_mutex);
    // 与 IO 指针一起失效，避免输出回调在销毁后重新写回悬空指针。
    s_http_capture_allowed.store(false, std::memory_order_release);
    if (generation == 0 ||
        s_http_io_generation.load(std::memory_order_relaxed) == generation) {
        s_http_io.store(nullptr, std::memory_order_release);
        s_http_io_generation.store(0, std::memory_order_release);
    }
}

// 中止正在阻塞的读，尽量缩短管线退出时间；调用方不能持有 io 锁。
void AbortAndClearHttpIo() {
    std::lock_guard<std::mutex> lock(s_io_mutex);
    s_http_capture_allowed.store(false, std::memory_order_release);
    esp_gmf_io_handle_t io = s_http_io.load(std::memory_order_acquire);
    if (io != nullptr) {
        esp_gmf_io_abort(io);
    }
    s_http_io.store(nullptr, std::memory_order_release);
    s_http_io_generation.store(0, std::memory_order_release);
}

int EffectiveDurationSeconds() {
    const int meta = s_duration_sec.load(std::memory_order_relaxed);
    if (meta > 0) {
        return meta;
    }
    return s_estimated_duration_sec.load(std::memory_order_relaxed);
}

int64_t ClampPositionMs(int64_t ms) {
    const int duration = EffectiveDurationSeconds();
    if (duration > 0 && ms > static_cast<int64_t>(duration) * 1000) {
        return static_cast<int64_t>(duration) * 1000;
    }
    if (ms < 0) {
        return 0;
    }
    return ms;
}

// 已消费字节（压缩口径）。info.pos 只在 seek 时写入、读取过程不推进，
// 所以必须用 speed monitor 的 total_bytes - 缓冲余量自己算。
int64_t ConsumedAudioBytes(uint32_t generation) {
    std::lock_guard<std::mutex> lock(s_io_mutex);
    if (s_http_io_generation.load(std::memory_order_acquire) != generation) {
        return 0;
    }
    esp_gmf_io_handle_t io = s_http_io.load(std::memory_order_acquire);
    if (io == nullptr) {
        return 0;
    }
    esp_gmf_io_speed_stats_t stats = {};
    if (esp_gmf_io_get_speed_stats(io, &stats) != ESP_GMF_ERR_OK) {
        return 0;
    }
    uint32_t filled = 0;
    if (esp_gmf_io_get_db_filled_size(io, &filled) != ESP_GMF_ERR_OK) {
        filled = 0;
    }
    const int64_t consumed =
        static_cast<int64_t>(stats.total_bytes) - static_cast<int64_t>(filled);
    if (consumed <= 0) {
        return 0;
    }
    // 单调递增采样：给重连时的 Range 用，进度条也读它。
    int64_t prev = s_consumed_live.load(std::memory_order_relaxed);
    if (consumed > prev) {
        s_consumed_live.store(consumed, std::memory_order_relaxed);
    }
    return consumed;
}

// 本集累计已消费字节（含之前各次连接的贡献）。
int64_t TotalConsumedBytes() {
    return s_consumed_committed.load(std::memory_order_relaxed) +
           s_consumed_live.load(std::memory_order_relaxed);
}

int64_t CurrentPositionMs() {
    const int duration = EffectiveDurationSeconds();
    const int64_t total = s_total_bytes.load(std::memory_order_relaxed);
    if (duration > 0 && total > 0) {
        // 先采样一次刷新 s_consumed_live（进度条 500ms 调一次，足够细）。
        ConsumedAudioBytes(s_generation.load(std::memory_order_relaxed));
        int64_t bytes =
            s_range_offset.load(std::memory_order_relaxed) + TotalConsumedBytes();
        if (bytes > total) {
            bytes = total;
        }
        return ClampPositionMs(bytes * duration * 1000 / total);
    }

    int64_t ms = s_base_ms.load(std::memory_order_relaxed);
    const int64_t clock = s_clock_us.load(std::memory_order_relaxed);
    if (clock != 0) {
        const int64_t elapsed_us = esp_timer_get_time() - clock;
        const int q = s_speed_q1000.load(std::memory_order_relaxed);
        ms += elapsed_us * q / 1000 / 1000;
    }
    return ClampPositionMs(ms);
}

void PausePositionClock() {
    if (s_clock_us.load(std::memory_order_acquire) == 0) {
        return;
    }
    const int64_t ms = CurrentPositionMs();
    s_clock_us.store(0, std::memory_order_release);
    s_base_ms.store(ms, std::memory_order_relaxed);
}

void ResetPositionClock(int64_t base_ms) {
    s_base_ms.store(base_ms, std::memory_order_relaxed);
    s_clock_us.store(esp_timer_get_time(), std::memory_order_relaxed);
}

void Emit(PodcastPlayerEvent event, const char* detail) {
    PodcastPlayerEventCallback callback = nullptr;
    void* ctx = nullptr;
    {
        std::lock_guard<std::mutex> lock(s_player_mutex);
        callback = s_event_cb;
        ctx = s_event_ctx;
    }
    if (callback != nullptr) {
        callback(event, detail, ctx);
    }
}

void PostPlayerControl(bool pause) {
    esp_asp_handle_t player = nullptr;
    uint32_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(s_player_mutex);
        player = s_player;
        generation = s_generation.load(std::memory_order_relaxed);
    }
    if (player == nullptr) {
        return;
    }
    struct ControlArg {
        esp_asp_handle_t player;
        uint32_t generation;
        bool pause;
    };
    auto* arg = new ControlArg{player, generation, pause};
    if (xTaskCreate(
            [](void* data) {
                std::unique_ptr<ControlArg> control(
                    static_cast<ControlArg*>(data));
                std::lock_guard<std::mutex> api_lock(s_api_mutex);
                bool valid = false;
                {
                    std::lock_guard<std::mutex> lock(s_player_mutex);
                    valid = s_player == control->player &&
                            s_generation.load(std::memory_order_relaxed) ==
                                control->generation;
                }
                if (!valid) {
                    // 播放器已切换或销毁，旧控制请求直接丢弃。
                } else if (control->pause) {
                    esp_audio_simple_player_pause(control->player);
                } else {
                    esp_audio_simple_player_resume(control->player);
                }
                vTaskDelete(nullptr);
            },
            "podcast_ctl", 3072, arg, 4, nullptr) != pdPASS) {
        delete arg;
        ESP_LOGW(TAG, "create control task failed");
    }
}

void EnsureMediaLibAdapter() {
    if (!s_media_lib_ready) {
        media_lib_add_default_adapter();
        s_media_lib_ready = true;
    }
}

void FinishPlayTask(uint32_t generation = 0) {
    {
        std::lock_guard<std::mutex> lock(s_player_mutex);
        if (s_play_task == xTaskGetCurrentTaskHandle()) {
            s_play_task = nullptr;
            // 预播任务退出（含失败/被切换）时解除按住，避免界面后续状态事件
            // 一直被当成预播而丢弃。
            if (generation != 0 &&
                s_prepare_generation.load(std::memory_order_relaxed) ==
                    generation) {
                s_prepare_mode.store(false, std::memory_order_release);
            }
        }
    }
    vTaskDelete(nullptr);
}

bool WaitForPendingStop(esp_asp_handle_t player) {
    {
        std::lock_guard<std::mutex> lock(s_lifecycle_mutex);
        if (s_pending_stop_player != player) {
            return true;
        }
    }
    for (int i = 0; i < 1500; ++i) {
        {
            std::lock_guard<std::mutex> lock(s_lifecycle_mutex);
            if (s_pending_stop_player != player) {
                return true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

void ApplyRateCvt16k(esp_asp_handle_t player) {
#ifdef CONFIG_ESP_AUDIO_SIMPLE_PLAYER_RESAMPLE_EN
    esp_gmf_pipeline_handle_t pipeline = nullptr;
    esp_gmf_element_handle_t rate_el = nullptr;
    if (esp_audio_simple_player_get_pipeline(player, &pipeline) !=
            ESP_GMF_ERR_OK ||
        pipeline == nullptr) {
        return;
    }
    if (esp_gmf_pipeline_get_el_by_name(pipeline, "aud_rate_cvt", &rate_el) ==
            ESP_GMF_ERR_OK &&
        rate_el != nullptr) {
        esp_gmf_rate_cvt_set_dest_rate(
            rate_el, static_cast<uint32_t>(kOutputSampleRate));
    }
#else
    (void)player;
#endif
}

void ForcePoolRateCvt16k(esp_gmf_pool_handle_t pool) {
#ifdef CONFIG_ESP_AUDIO_SIMPLE_PLAYER_RESAMPLE_EN
    if (pool == nullptr) {
        return;
    }
    const void* it = nullptr;
    esp_gmf_element_handle_t el = nullptr;
    while (esp_gmf_pool_iterate_element(pool, &it, &el) == ESP_GMF_ERR_OK) {
        if (el == nullptr) {
            continue;
        }
        const char* tag = OBJ_GET_TAG(el);
        if (tag == nullptr || strcmp(tag, "aud_rate_cvt") != 0) {
            continue;
        }
        esp_gmf_rate_cvt_set_dest_rate(el,
                                       static_cast<uint32_t>(kOutputSampleRate));
        auto* cfg = static_cast<esp_ae_rate_cvt_cfg_t*>(OBJ_GET_CFG(el));
        if (cfg != nullptr) {
            cfg->dest_rate = static_cast<uint32_t>(kOutputSampleRate);
        }
    }
#else
    (void)pool;
#endif
}

struct StopArg {
    esp_asp_handle_t player = nullptr;
};

void PodcastStopTask(void* data) {
    std::unique_ptr<StopArg> arg(static_cast<StopArg*>(data));
    const int64_t started_us = esp_timer_get_time();
    const uint32_t generation = s_generation.load(std::memory_order_acquire);
    TimingLog(generation, "stop_begin");
    esp_audio_simple_player_stop(arg->player);
    ESP_LOGI(TAG, "[timing] gen=%u stage=stop_done dt=%dms",
             static_cast<unsigned>(generation),
             static_cast<int>((esp_timer_get_time() - started_us) / 1000));
    {
        std::lock_guard<std::mutex> lock(s_lifecycle_mutex);
        if (s_pending_stop_player == arg->player) {
            s_pending_stop_player = nullptr;
            s_stop_cv.notify_all();
        }
    }
    vTaskDelete(nullptr);
}

void DetachCurrentPlayback() {
    esp_asp_handle_t old_player = nullptr;
    {
        std::lock_guard<std::mutex> life_lock(s_lifecycle_mutex);
        std::lock_guard<std::mutex> lock(s_player_mutex);
        s_generation.fetch_add(1, std::memory_order_acq_rel);
        old_player = s_player;
        s_player = nullptr;
        s_output_enabled.store(false, std::memory_order_release);
        s_output_generation.store(0, std::memory_order_release);
        AbortAndClearHttpIo();
        if (old_player != nullptr) {
            s_pending_stop_player = old_player;
        }
    }
    if (old_player == nullptr) {
        return;
    }
    TimingLog(s_generation.load(std::memory_order_relaxed), "detach_old");
    auto* arg = new StopArg{old_player};
    if (xTaskCreate(PodcastStopTask, "podcast_stop", 4096, arg, 4, nullptr) !=
        pdPASS) {
        delete arg;
        // 极少见：任务创建失败时退回同步停止，保证不会有两个声音同时播放。
        esp_audio_simple_player_stop(old_player);
        std::lock_guard<std::mutex> lock(s_lifecycle_mutex);
        if (s_pending_stop_player == old_player) {
            s_pending_stop_player = nullptr;
        }
    }
}

bool IsQuickPronunciationUrl(const std::string& url) {
    return url.find("dict.youdao.com") != std::string::npos ||
           url.find("fanyi.baidu.com") != std::string::npos;
}

esp_gmf_err_t PodcastHttpScore(esp_gmf_io_handle_t /*io*/, const char* url,
                               int* score) {
    if (score == nullptr) {
        return ESP_GMF_ERR_INVALID_ARG;
    }
    *score = ESP_GMF_IO_SCORE_NONE;
    if (url != nullptr &&
        (strncasecmp(url, "http://", 7) == 0 ||
         strncasecmp(url, "https://", 8) == 0)) {
        // 高于默认 io_http 的 STANDARD，确保池选中带鉴权回调的实例。
        *score = ESP_GMF_IO_SCORE_STANDARD + 10;
    }
    return ESP_GMF_ERR_OK;
}

void ParseResponseHeaders(esp_http_client_handle_t client, uint32_t generation) {
    if (client == nullptr || generation != s_generation.load() ||
        s_headers_parsed.exchange(true)) {
        return;
    }
    const int status = esp_http_client_get_status_code(client);
    char timing_extra[32] = {};
    snprintf(timing_extra, sizeof(timing_extra), "status=%d", status);
    TimingLog(generation, "http_headers", timing_extra);
    // Supabase 签名地址过期通常返回 400(InvalidJWT)，权限问题返回 403。
    if ((status == 400 || status == 403) &&
        !s_quick_url.load(std::memory_order_relaxed) &&
        s_url_expired_generation.exchange(generation,
                                          std::memory_order_acq_rel) !=
            generation) {
        // 音频地址（非设备 Token）返回 403，视为签名地址过期，交给页面
        // 失效该节目缓存并重试一次；设备 Token 的 401/403 不会走到这里。
        ESP_LOGW(TAG, "signed url expired, generation=%u",
                 static_cast<unsigned>(generation));
        Emit(PodcastPlayerEvent::UrlExpired, nullptr);
    }
    if (status == 200 && s_range_offset.load(std::memory_order_relaxed) > 0) {
        // 代理忽略 Range 时实际从头播放，进度也必须回到 0，避免声音与进度错位。
        ESP_LOGW(TAG, "server ignored range, restart progress from 0");
        s_range_offset.store(0, std::memory_order_relaxed);
        s_base_ms.store(0, std::memory_order_relaxed);
    }
    char* content_range = nullptr;
    if (esp_http_client_get_header(client, "Content-Range", &content_range) ==
            ESP_OK &&
        content_range != nullptr) {
        const char* slash = strrchr(content_range, '/');
        if (slash != nullptr && slash[1] != '\0' && slash[1] != '*') {
            const long long total = atoll(slash + 1);
            if (total > 0) {
                s_total_bytes.store(total, std::memory_order_relaxed);
            }
        }
    }
    if (s_total_bytes.load(std::memory_order_relaxed) <= 0) {
        const int64_t length = esp_http_client_get_content_length(client);
        if (length > 0 && s_range_offset.load(std::memory_order_relaxed) == 0) {
            s_total_bytes.store(length, std::memory_order_relaxed);
        }
    }
    const int64_t total = s_total_bytes.load(std::memory_order_relaxed);
    ESP_LOGI(TAG, "http headers status=%d total=%d", status,
             static_cast<int>(total));
    if (s_duration_sec.load(std::memory_order_relaxed) <= 0 && total > 0) {
        // 接口没给时长时按 64kbps 估算量程，只用于进度条。
        s_estimated_duration_sec.store(
            static_cast<int>(total * 8 / 64000), std::memory_order_relaxed);
    }
}

int PodcastHttpEvent(http_stream_event_msg_t* msg) {
    if (msg == nullptr || msg->http_client == nullptr) {
        return 0;
    }
    auto* player_ctx = static_cast<PlayerContext*>(msg->user_data);
    if (player_ctx == nullptr ||
        player_ctx->generation !=
            s_generation.load(std::memory_order_acquire)) {
        return 0;
    }
    auto* client = static_cast<esp_http_client_handle_t>(msg->http_client);
    if (msg->event_id == HTTP_STREAM_PRE_REQUEST) {
        TimingLog(player_ctx->generation, "http_request");
        s_headers_parsed.store(false, std::memory_order_release);
        // 新连接开始：把上一连接已消费的量并入累计，Range 才能接着往下要。
        // committed + live 的和在提交前后不变，所以重复进入也安全。
        s_consumed_committed.fetch_add(
            s_consumed_live.exchange(0, std::memory_order_acq_rel),
            std::memory_order_acq_rel);
        // 限制阻塞读时长，停止/切节目时不会被默认 30s 读超时拖住。
        esp_http_client_set_timeout_ms(
            client, s_quick_url.load(std::memory_order_relaxed) ? 5000 : 8000);
        if (s_quick_url.load(std::memory_order_relaxed)) {
            esp_http_client_set_header(
                client, "User-Agent",
                "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                "AppleWebKit/537.36 Chrome/143.0.0.0 Safari/537.36");
            esp_http_client_set_header(client, "Accept", "audio/mpeg,*/*");
            return 0;
        }

        // 播客流同样不能缺 User-Agent：Cloudflare 会把无 UA 请求挡成 403，
        // 在播放器里表现为“播放失败/地址过期”，排查时极易误判成签名问题。
        const std::string user_agent = SystemInfo::GetUserAgent();
        esp_http_client_set_header(client, "User-Agent", user_agent.c_str());

        // 只对自家域名带 Bearer：签名音频地址在第三方域，且 302 之后 GMF
        // 不会再次触发 PRE_REQUEST，头会一路带到跳转目标。
        const std::string token = PodcastClient::DeviceToken();
        std::string url_copy;
        {
            std::lock_guard<std::mutex> lock(s_player_mutex);
            url_copy = s_current_url;
        }
        if (!token.empty() && PodcastClient::IsOwnHost(url_copy)) {
            const std::string authorization = "Bearer " + token;
            esp_http_client_set_header(client, "Authorization",
                                       authorization.c_str());
        }
        const int64_t pos = s_range_offset.load(std::memory_order_relaxed) +
                            s_consumed_committed.load(std::memory_order_relaxed);
        if (pos > 0) {
            char range[48] = {};
            snprintf(range, sizeof(range), "bytes=%d-",
                     static_cast<int>(pos));
            esp_http_client_set_header(client, "Range", range);
            ESP_LOGI(TAG, "http range %s", range);
        }
    } else if (msg->event_id == HTTP_STREAM_ON_RESPONSE) {
        ParseResponseHeaders(client, player_ctx->generation);
        // 3xx 之后可能是第三方域（CDN/签名存储）：把 Bearer 摘掉再跳。
        const int status = esp_http_client_get_status_code(client);
        if (status >= 300 && status < 400) {
            esp_http_client_delete_header(client, "Authorization");
        }
    }
    return 0;
}

bool RegisterPodcastHttpIo(esp_asp_handle_t player, PlayerContext* player_ctx) {
    http_io_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_cfg.dir = ESP_GMF_IO_DIR_READER;
    http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    http_cfg.event_handle = PodcastHttpEvent;
    http_cfg.user_data = player_ctx;
    // 播客是持续码流，扩大环形缓冲以吸收 WiFi/4G 抖动，减少解码欠载卡顿。
    // 短发音不需要大缓冲，避免每次点单词都多占 PSRAM。
    const bool quick = s_quick_url.load(std::memory_order_relaxed);
    http_cfg.io_cfg.buffer_cfg.buffer_size =
        quick ? 64 * 1024 : 512 * 1024;
    http_cfg.io_cfg.buffer_cfg.io_size = quick ? 4 * 1024 : 16 * 1024;
    // 进度/续传要靠 speed monitor 的字节统计：关掉它 get_speed_stats 恒返回 0，
    // 进度条会永远停在 0、重连也会从 0 重播。
    http_cfg.io_cfg.enable_speed_monitor = true;

    esp_gmf_io_handle_t http_io = nullptr;
    if (esp_gmf_io_http_init(&http_cfg, &http_io) != ESP_GMF_ERR_OK ||
        http_io == nullptr) {
        return false;
    }
    if (esp_gmf_obj_set_tag(reinterpret_cast<esp_gmf_obj_handle_t>(http_io),
                            kPodcastHttpTag) != ESP_GMF_ERR_OK) {
        esp_gmf_obj_delete(http_io);
        return false;
    }
    reinterpret_cast<esp_gmf_io_t*>(http_io)->get_score = PodcastHttpScore;
    if (esp_audio_simple_player_register_io(player, http_io) != ESP_GMF_ERR_OK) {
        esp_gmf_obj_delete(http_io);
        return false;
    }
    return true;
}

void CapturePipelineIo(esp_asp_handle_t player, uint32_t generation) {
    std::lock_guard<std::mutex> io_lock(s_io_mutex);
    if (player == nullptr ||
        generation != s_generation.load(std::memory_order_acquire) ||
        !s_http_capture_allowed.load(std::memory_order_acquire) ||
        s_http_io_generation.load(std::memory_order_acquire) == generation) {
        return;
    }
    esp_gmf_pipeline_handle_t pipeline = nullptr;
    esp_gmf_io_handle_t io = nullptr;
    if (esp_audio_simple_player_get_pipeline(player, &pipeline) ==
            ESP_GMF_ERR_OK &&
        pipeline != nullptr &&
        esp_gmf_pipeline_get_in(pipeline, &io) == ESP_GMF_ERR_OK &&
        io != nullptr) {
        s_http_io.store(io, std::memory_order_release);
        s_http_io_generation.store(generation, std::memory_order_release);
        ESP_LOGI(TAG, "io captured, generation=%u",
                 static_cast<unsigned>(generation));
    }
}

int PodcastOutCallback(uint8_t* data, int data_size, void* ctx) {
    auto* player_ctx = static_cast<PlayerContext*>(ctx);
    if (player_ctx == nullptr || player_ctx->codec == nullptr ||
        player_ctx->generation !=
            s_generation.load(std::memory_order_acquire)) {
        return 0;
    }
    if (s_http_io.load(std::memory_order_acquire) == nullptr) {
        CapturePipelineIo(player_ctx->player, player_ctx->generation);
    }
    // 预播：数据已经解到首帧，这里把管线按住，让 HTTP 环形缓冲填满；
    // 用户点播时放开即可直接出声。
    if (s_prepare_mode.load(std::memory_order_acquire) &&
        player_ctx->generation ==
            s_prepare_generation.load(std::memory_order_acquire)) {
        while (s_prepare_mode.load(std::memory_order_acquire) &&
               player_ctx->generation ==
                   s_generation.load(std::memory_order_acquire)) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (player_ctx->generation !=
                s_generation.load(std::memory_order_acquire) ||
            !s_prepare_play.exchange(false, std::memory_order_acq_rel)) {
            return 0;
        }
    }
    if (!s_output_enabled.load(std::memory_order_acquire) ||
        s_output_generation.load(std::memory_order_relaxed) !=
            s_generation.load(std::memory_order_relaxed) ||
        data == nullptr || data_size <= 0) {
        return 0;
    }
    // 播客首帧先让 HTTP 缓冲填一小段，避免代理首包偏慢造成开头连续欠载。
    // 英语发音 URL 跳过，避免增加点按发音的等待。
    if (!s_quick_url.load(std::memory_order_relaxed) &&
        !s_start_gate_done.exchange(true, std::memory_order_acq_rel)) {
        TimingLog(player_ctx->generation, "first_pcm_buffered");
        vTaskDelay(pdMS_TO_TICKS(200));
        if (player_ctx->generation !=
                s_generation.load(std::memory_order_acquire) ||
            !s_output_enabled.load(std::memory_order_acquire)) {
            return 0;
        }
    }
    auto* codec = player_ctx->codec;
    if (codec == nullptr) {
        return 0;
    }

    const int frames = data_size / static_cast<int>(sizeof(int16_t));
    if (frames <= 0) {
        return 0;
    }
    const auto* pcm = reinterpret_cast<const int16_t*>(data);
    const int channels = s_pcm_channels.load(std::memory_order_relaxed);

    if (channels >= 2) {
        const int mono = frames / channels;
        if (mono <= 0) {
            return 0;
        }
        s_pcm_buf.resize(static_cast<size_t>(mono));
        for (int i = 0; i < mono; ++i) {
            int sum = 0;
            for (int ch = 0; ch < channels; ++ch) {
                sum += pcm[i * channels + ch];
            }
            s_pcm_buf[static_cast<size_t>(i)] =
                static_cast<int16_t>(sum / channels);
        }
    } else {
        s_pcm_buf.assign(pcm, pcm + frames);
    }

    const int speed_q = s_speed_q1000.load(std::memory_order_relaxed);
    if (speed_q != 1000 && !s_pcm_buf.empty()) {
        const double ratio = static_cast<double>(speed_q) / 1000.0;
        const size_t out_count = static_cast<size_t>(
            static_cast<double>(s_pcm_buf.size()) / ratio);
        if (out_count > 0) {
            s_pcm_out.resize(out_count);
            double pos = 0.0;
            for (size_t i = 0; i < out_count; ++i) {
                const size_t index = static_cast<size_t>(pos);
                const double frac = pos - static_cast<double>(index);
                const int16_t a = s_pcm_buf[index];
                const int16_t b =
                    index + 1 < s_pcm_buf.size() ? s_pcm_buf[index + 1] : a;
                s_pcm_out[i] = static_cast<int16_t>(
                    static_cast<double>(a) +
                    (static_cast<double>(b) - static_cast<double>(a)) * frac);
                pos += ratio;
            }
            codec->OutputData(s_pcm_out);
        }
    } else {
        codec->OutputData(s_pcm_buf);
    }
    if (s_first_pcm_generation.exchange(player_ctx->generation,
                                       std::memory_order_acq_rel) !=
        player_ctx->generation) {
        TimingLog(player_ctx->generation, "first_pcm_out");
    }
    if (!s_reported_playing.exchange(true, std::memory_order_acq_rel)) {
        Emit(PodcastPlayerEvent::Playing, nullptr);
    }
    return 0;
}

int PodcastEventCallback(esp_asp_event_pkt_t* event, void* ctx) {
    auto* player_ctx = static_cast<PlayerContext*>(ctx);
    if (event == nullptr || player_ctx == nullptr ||
        player_ctx->generation !=
            s_generation.load(std::memory_order_acquire)) {
        return 0;
    }
    if (event->type == ESP_ASP_EVENT_TYPE_MUSIC_INFO &&
        event->payload_size >=
            static_cast<int>(sizeof(esp_asp_music_info_t))) {
        esp_asp_music_info_t info = {};
        std::memcpy(&info, event->payload, sizeof(info));
        if (info.channels > 0) {
            s_pcm_channels.store(info.channels, std::memory_order_relaxed);
        }
        ESP_LOGI(TAG, "music info: rate=%d ch=%d bits=%d", info.sample_rate,
                 info.channels, info.bits);
    } else if (event->type == ESP_ASP_EVENT_TYPE_STATE &&
               event->payload_size == sizeof(esp_asp_state_t)) {
        esp_asp_state_t state = ESP_ASP_STATE_NONE;
        std::memcpy(&state, event->payload, event->payload_size);
        // 预播阶段只做“打开 + 预解码”，不上报播放状态给界面。
        if (s_prepare_mode.load(std::memory_order_acquire)) {
            return 0;
        }
        if (state == ESP_ASP_STATE_RUNNING) {
            // 管线刚运行时先抓住 HTTP IO，切集时 abort 才能立即打断读操作。
            CapturePipelineIo(player_ctx->player, player_ctx->generation);
            if (!s_reported_playing.exchange(true,
                                             std::memory_order_acq_rel)) {
                Emit(PodcastPlayerEvent::Playing, nullptr);
            }
        } else if (state == ESP_ASP_STATE_PAUSED) {
            Emit(PodcastPlayerEvent::Paused, nullptr);
        } else if (state == ESP_ASP_STATE_ERROR) {
            const char* stage = "pipe";
            esp_gmf_pipeline_handle_t pipe = nullptr;
            if (player_ctx->player != nullptr &&
                esp_audio_simple_player_get_pipeline(
                    player_ctx->player, &pipe) ==
                    ESP_GMF_ERR_OK &&
                pipe != nullptr) {
                static const char* const kNames[] = {"aud_dec",
                                                     "aud_rate_cvt"};
                for (const char* name : kNames) {
                    esp_gmf_element_handle_t el = nullptr;
                    if (esp_gmf_pipeline_get_el_by_name(pipe, name, &el) !=
                            ESP_GMF_ERR_OK ||
                        el == nullptr) {
                        continue;
                    }
                    esp_gmf_event_state_t el_state = ESP_GMF_EVENT_STATE_NONE;
                    if (esp_gmf_element_get_state(el, &el_state) ==
                            ESP_GMF_ERR_OK &&
                        el_state == ESP_GMF_EVENT_STATE_ERROR) {
                        stage = name;
                    }
                }
            }
            char detail[48] = {};
            snprintf(detail, sizeof(detail), "播放失败@%s", stage);
            Emit(PodcastPlayerEvent::Error, detail);
        }
    }
    return 0;
}

void PodcastPlayTask(void* arg) {
    auto* play_arg = static_cast<PlayArg*>(arg);
    const uint32_t generation = play_arg->generation;
    const std::string url = play_arg->url;
    const int64_t base_ms = play_arg->base_ms;
    const bool prepare = play_arg->prepare;
    delete play_arg;
    TimingLog(generation, "play_task_start");

    auto is_current = [generation]() {
        return generation == s_generation.load(std::memory_order_acquire);
    };
    auto abandon = [generation](const char* detail) {
        if (detail != nullptr) {
            Emit(PodcastPlayerEvent::Error, detail);
        }
        FinishPlayTask(generation);
    };

    if (!is_current()) {
        abandon(nullptr);
        return;
    }

    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        abandon("音频输出不可用");
        return;
    }

    EnsureMediaLibAdapter();
    s_pcm_channels.store(2, std::memory_order_relaxed);
    s_reported_playing.store(false, std::memory_order_relaxed);
    s_start_gate_done.store(false, std::memory_order_relaxed);

    auto* player_ctx = new PlayerContext{codec, nullptr, generation};
    esp_asp_cfg_t cfg = {
        .in = {.cb = nullptr, .user_ctx = nullptr},
        .out = {.cb = PodcastOutCallback, .user_ctx = player_ctx},
        .task_prio = 5,
        .task_stack = 8 * 1024,
        .prev = nullptr,
        .prev_ctx = nullptr,
    };
    esp_asp_handle_t player = nullptr;
    if (esp_audio_simple_player_new(&cfg, &player) != ESP_GMF_ERR_OK ||
        player == nullptr) {
        delete player_ctx;
        abandon("创建播放器失败");
        return;
    }
    player_ctx->player = player;
    TimingLog(generation, "player_new");

    esp_gmf_pool_handle_t pool = nullptr;
    if (esp_audio_simple_player_get_pool(player, &pool) == ESP_GMF_ERR_OK &&
        pool != nullptr) {
        ForcePoolRateCvt16k(pool);
    }
    ApplyRateCvt16k(player);
    if (!RegisterPodcastHttpIo(player, player_ctx)) {
        esp_audio_simple_player_destroy(player);
        delete player_ctx;
        abandon("创建音频输入失败");
        return;
    }
    esp_audio_simple_player_set_event(player, PodcastEventCallback,
                                      player_ctx);
    TimingLog(generation, "io_ready");

    bool cancelled = false;
    {
        std::lock_guard<std::mutex> api_lock(s_api_mutex);
        {
            std::lock_guard<std::mutex> lock(s_player_mutex);
            cancelled = !is_current();
            if (!cancelled) {
                s_player = player;
            }
        }
        if (cancelled) {
            esp_audio_simple_player_destroy(player);
            delete player_ctx;
        }
    }
    if (cancelled) {
        abandon(nullptr);
        return;
    }

    codec->EnableOutput(true);
    // 从这里到任务退出，喇叭归播客，电源定时器不再关输出。
    ExternalPlaybackGuard output_guard;
    if (!prepare) {
        s_output_generation.store(generation, std::memory_order_relaxed);
        s_output_enabled.store(true, std::memory_order_release);
        ResetPositionClock(base_ms);
    }
    s_http_capture_allowed.store(true, std::memory_order_release);

    if (!is_current()) {
        s_output_enabled.store(false, std::memory_order_release);
        s_http_capture_allowed.store(false, std::memory_order_release);
        if (!WaitForPendingStop(player)) {
            ESP_LOGE(TAG, "stop timeout, keep player %p alive", player);
            abandon(nullptr);
            return;
        }
        {
            std::lock_guard<std::mutex> api_lock(s_api_mutex);
            {
                std::lock_guard<std::mutex> lock(s_player_mutex);
                if (s_player == player) {
                    s_player = nullptr;
                }
                ClearHttpIo(generation);
            }
            esp_audio_simple_player_destroy(player);
            delete player_ctx;
        }
        abandon(nullptr);
        return;
    }

    ESP_LOGI(TAG, "start MP3 playback, offset=%d",
             static_cast<int>(s_range_offset.load(
                 std::memory_order_relaxed)));
    TimingLog(generation, "run_begin");
    const esp_gmf_err_t result =
        esp_audio_simple_player_run_to_end(player, url.c_str(), nullptr);
    char run_extra[32] = {};
    snprintf(run_extra, sizeof(run_extra), "ret=0x%x",
             static_cast<unsigned>(result));
    TimingLog(generation, "run_end", run_extra);
    s_http_capture_allowed.store(false, std::memory_order_release);
    if (s_output_generation.load(std::memory_order_acquire) == generation) {
        s_output_enabled.store(false, std::memory_order_release);
    }

    // 切换节目时 stop 在后台执行，必须等它退出后再销毁播放器，避免 UAF。
    // 关键点：等待用条件变量（不再 10ms 轮询），并且"确认没有 pending stop"
    // 与"销毁"在同一个 lifecycle 临界区里完成 —— 原来两者分属两把锁，
    // DetachCurrentPlayback 能插进中间，造成 stop 与 destroy 并发作用于同一句柄。
    bool still_current = false;
    {
        std::unique_lock<std::mutex> life_lock(s_lifecycle_mutex);
        if (s_pending_stop_player == player) {
            const bool stopped = s_stop_cv.wait_for(
                life_lock, std::chrono::seconds(15),
                [&player]() { return s_pending_stop_player != player; });
            if (!stopped) {
                ESP_LOGE(TAG, "stop timeout, keep player %p alive", player);
                life_lock.unlock();
                FinishPlayTask(generation);
                return;
            }
        }
        {
            std::lock_guard<std::mutex> lock(s_player_mutex);
            if (s_player == player) {
                s_player = nullptr;
            }
            still_current =
                generation == s_generation.load(std::memory_order_relaxed);
            ClearHttpIo(generation);
        }
        esp_audio_simple_player_destroy(player);
        delete player_ctx;
    }

    if (still_current) {
        if (result == ESP_GMF_ERR_OK) {
            Emit(PodcastPlayerEvent::Finished, nullptr);
        } else {
            char detail[48] = {};
            snprintf(detail, sizeof(detail), "播放失败 0x%X",
                     static_cast<unsigned>(result));
            ESP_LOGE(TAG, "play failed, ret=0x%x url_len=%u",
                     static_cast<unsigned>(result),
                     static_cast<unsigned>(url.size()));
            Emit(PodcastPlayerEvent::Error, detail);
        }
    }
    FinishPlayTask(generation);
}

void PodcastStartTask(void* arg) {
    auto* play_arg = static_cast<PlayArg*>(arg);
    const uint32_t generation = play_arg->generation;
    bool ready = false;
    TimingLog(generation, "start_task_wait");
    // 旧管线退出最长要等 HTTP 读超时，给足 12 秒，避免切节目直接报“播放器正忙”。
    for (int i = 0; i < 600; ++i) {
        if (generation != s_generation.load(std::memory_order_acquire)) {
            delete play_arg;
            vTaskDelete(nullptr);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(s_player_mutex);
            if (s_play_task == nullptr) {
                ready = true;
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    TimingLog(generation, ready ? "start_task_ready" : "start_task_timeout");
    bool started = false;
    {
        std::lock_guard<std::mutex> lock(s_player_mutex);
        if (ready &&
            generation == s_generation.load(std::memory_order_acquire)) {
            started = xTaskCreate(PodcastPlayTask, "podcast_play", 8192,
                                  play_arg, 5, &s_play_task) == pdPASS;
            if (!started) {
                s_play_task = nullptr;
            }
        }
    }
    if (!started) {
        delete play_arg;
        if (generation == s_generation.load(std::memory_order_acquire)) {
            Emit(PodcastPlayerEvent::Error, "播放器正忙，请稍后重试");
        }
    }
    vTaskDelete(nullptr);
}

}  // namespace

static bool StartInternal(const std::string& url, std::string& error,
                          int64_t range_bytes, int64_t base_ms,
                          int64_t click_us = 0, bool prepare = false) {
    error.clear();
    if (url.empty()) {
        error = "播放地址为空";
        return false;
    }

    DetachCurrentPlayback();
    bool create_failed = false;
    {
        std::lock_guard<std::mutex> lock(s_player_mutex);
        const uint32_t generation =
            s_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
        s_prepare_play.store(false, std::memory_order_release);
        s_prepare_generation.store(generation, std::memory_order_release);
        s_prepare_mode.store(prepare, std::memory_order_release);
        if (prepare) {
            // 预播已经等过首帧，放开时不要再加首帧缓冲延时。
            s_start_gate_done.store(true, std::memory_order_release);
        }
        MarkTimingStart(generation, click_us);
        auto* arg = new PlayArg{};
        arg->generation = generation;
        arg->url = url;
        arg->range_bytes = range_bytes;
        arg->base_ms = base_ms;
        arg->click_us = click_us;
        arg->prepare = prepare;
        s_current_url = url;
        s_quick_url.store(IsQuickPronunciationUrl(url),
                          std::memory_order_relaxed);
        s_range_offset.store(range_bytes, std::memory_order_relaxed);
        s_consumed_live.store(0, std::memory_order_relaxed);
        s_consumed_committed.store(0, std::memory_order_relaxed);
        s_http_capture_allowed.store(false, std::memory_order_release);
        ClearHttpIo(0);
        s_headers_parsed.store(false, std::memory_order_relaxed);
        if (range_bytes == 0) {
            s_total_bytes.store(0, std::memory_order_relaxed);
            s_estimated_duration_sec.store(0, std::memory_order_relaxed);
        }
        s_base_ms.store(base_ms, std::memory_order_relaxed);
        s_clock_us.store(0, std::memory_order_relaxed);
        if (xTaskCreate(PodcastStartTask, "podcast_start", 4096, arg, 4,
                        nullptr) != pdPASS) {
            delete arg;
            error = "创建播放任务失败";
            create_failed = true;
            s_prepare_mode.store(false, std::memory_order_release);
        }
    }
    return !create_failed;
}

bool PodcastPlayer::Start(const std::string& url, std::string& error,
                          int64_t click_us) {
    return StartInternal(url, error, 0, 0, click_us);
}

namespace {
// 预播最多保留 45 秒：环形缓冲填满后连接会闲着，超过常见 keep-alive
// 时限就可能被服务端关掉，宁可放弃也不要放出一段“播几秒就结束”的音频。
constexpr int kPrepareHoldSeconds = 45;

void PodcastPrepareWatchdog(void* arg) {
    const uint32_t generation =
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    for (int i = 0; i < kPrepareHoldSeconds * 50; ++i) {
        if (!s_prepare_mode.load(std::memory_order_acquire) ||
            s_prepare_generation.load(std::memory_order_acquire) !=
                generation) {
            vTaskDelete(nullptr);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGW(TAG, "prepare timeout, cancel prepared player");
    PodcastPlayer::Stop();
    vTaskDelete(nullptr);
}
}  // namespace

bool PodcastPlayer::Prepare(const std::string& url, std::string& error) {
    error.clear();
    if (url.empty()) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(s_player_mutex);
        if (s_player != nullptr || s_play_task != nullptr ||
            s_prepare_mode.load(std::memory_order_relaxed)) {
            return false;
        }
    }
    if (!StartInternal(url, error, 0, 0, 0, true)) {
        return false;
    }
    const uint32_t generation =
        s_prepare_generation.load(std::memory_order_acquire);
    if (xTaskCreate(
            PodcastPrepareWatchdog, "podcast_prep", 3072,
            reinterpret_cast<void*>(static_cast<uintptr_t>(generation)), 2,
            nullptr) != pdPASS) {
        ESP_LOGW(TAG, "create prepare watchdog failed");
    }
    return true;
}

bool PodcastPlayer::IsPreparedFor(const std::string& url) {
    if (!s_prepare_mode.load(std::memory_order_acquire)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(s_player_mutex);
    return s_player != nullptr && s_current_url == url;
}

bool PodcastPlayer::PlayPrepared(const std::string& url, std::string& error,
                                 int64_t click_us) {
    error.clear();
    if (!IsPreparedFor(url)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(s_player_mutex);
    if (s_player == nullptr || s_current_url != url ||
        !s_prepare_mode.load(std::memory_order_acquire)) {
        return false;
    }
    const uint32_t generation =
        s_prepare_generation.load(std::memory_order_acquire);
    MarkTimingStart(generation, click_us);
    s_output_generation.store(generation, std::memory_order_relaxed);
    s_output_enabled.store(true, std::memory_order_release);
    ResetPositionClock(0);
    s_prepare_play.store(true, std::memory_order_release);
    s_prepare_mode.store(false, std::memory_order_release);
    TimingLog(generation, "prepared_release");
    return true;
}

void PodcastPlayer::StopUnlessPrepared() {
    if (s_prepare_mode.load(std::memory_order_acquire)) {
        return;
    }
    Stop();
}

bool PodcastPlayer::Seek(int seconds, std::string& error) {
    error.clear();
    const int duration = EffectiveDurationSeconds();
    const int64_t total = s_total_bytes.load(std::memory_order_relaxed);
    std::string url;
    {
        std::lock_guard<std::mutex> lock(s_player_mutex);
        url = s_current_url;
    }
    if (url.empty() || duration <= 0 || total <= 0) {
        error = "暂不支持拖动";
        return false;
    }
    if (seconds < 0) {
        seconds = 0;
    }
    if (seconds >= duration) {
        Stop();
        return true;
    }
    const int64_t offset = total * seconds / duration;
    return StartInternal(url, error, offset,
                         static_cast<int64_t>(seconds) * 1000);
}

void PodcastPlayer::SetDuration(int seconds) {
    s_estimated_duration_sec.store(0, std::memory_order_relaxed);
    s_duration_sec.store(seconds > 0 ? seconds : 0,
                         std::memory_order_relaxed);
}

int PodcastPlayer::GetPositionSeconds() {
    return static_cast<int>(CurrentPositionMs() / 1000);
}

int PodcastPlayer::GetDurationSeconds() {
    return EffectiveDurationSeconds();
}

void PodcastPlayer::SetSpeed(float speed) {
    if (speed < 0.5f) {
        speed = 0.5f;
    } else if (speed > 1.5f) {
        speed = 1.5f;
    }
    const int64_t ms = CurrentPositionMs();
    const bool running = s_clock_us.load(std::memory_order_acquire) != 0;
    s_base_ms.store(ms, std::memory_order_relaxed);
    s_clock_us.store(running ? esp_timer_get_time() : 0,
                     std::memory_order_relaxed);
    s_speed_q1000.store(static_cast<int>(speed * 1000.0f + 0.5f),
                        std::memory_order_relaxed);
}

float PodcastPlayer::GetSpeed() {
    return static_cast<float>(s_speed_q1000.load(std::memory_order_relaxed)) /
           1000.0f;
}

bool PodcastPlayer::Pause() {
    esp_asp_handle_t player = nullptr;
    {
        std::lock_guard<std::mutex> lock(s_player_mutex);
        player = s_player;
    }
    if (player == nullptr) {
        return false;
    }
    PausePositionClock();
    s_output_enabled.store(false, std::memory_order_release);
    PostPlayerControl(true);
    return true;
}

bool PodcastPlayer::Resume() {
    esp_asp_handle_t player = nullptr;
    {
        std::lock_guard<std::mutex> lock(s_player_mutex);
        player = s_player;
    }
    if (player == nullptr) {
        return false;
    }
    if (s_clock_us.load(std::memory_order_acquire) == 0) {
        s_clock_us.store(esp_timer_get_time(), std::memory_order_release);
    }
    s_output_enabled.store(true, std::memory_order_release);
    PostPlayerControl(false);
    return true;
}

void PodcastPlayer::Stop() {
    s_prepare_mode.store(false, std::memory_order_release);
    s_prepare_play.store(false, std::memory_order_release);
    DetachCurrentPlayback();
    s_clock_us.store(0, std::memory_order_relaxed);
    s_base_ms.store(0, std::memory_order_relaxed);
    s_range_offset.store(0, std::memory_order_relaxed);
    s_consumed_live.store(0, std::memory_order_relaxed);
    s_consumed_committed.store(0, std::memory_order_relaxed);
    s_total_bytes.store(0, std::memory_order_relaxed);
    s_estimated_duration_sec.store(0, std::memory_order_relaxed);
    s_http_capture_allowed.store(false, std::memory_order_release);
    ClearHttpIo(0);
}

void PodcastPlayer::SetEventCallback(PodcastPlayerEventCallback callback,
                                     void* ctx) {
    std::lock_guard<std::mutex> lock(s_player_mutex);
    s_event_cb = callback;
    s_event_ctx = ctx;
}
