#pragma once

#include <cstdint>
#include <string>

enum class PodcastPlayerEvent {
    Playing,
    Paused,
    Finished,
    Error,
    // 缓存的签名播放地址已失效（HTTP 403），页面应失效缓存后重试一次。
    UrlExpired,
};

using PodcastPlayerEventCallback =
    void (*)(PodcastPlayerEvent event, const char* detail, void* ctx);

class PodcastPlayer {
public:
    // click_us 为用户点击节目的时间戳（esp_timer_get_time），仅用于耗时诊断。
    static bool Start(const std::string& url, std::string& error,
                      int64_t click_us = 0);
    // 预播：进入列表就提前打开并解码到首帧后按住，点击时几乎零等待。
    // 只做一次、失败就退回普通 Start；总线或网络异常时自动放弃。
    static bool Prepare(const std::string& url, std::string& error);
    static bool IsPreparedFor(const std::string& url);
    static bool PlayPrepared(const std::string& url, std::string& error,
                             int64_t click_us = 0);
    // 停止当前播放；若手里只有“已预播未开始”的播放器则保留。
    static void StopUnlessPrepared();
    static bool Pause();
    static bool Resume();
    static void Stop();
    // 拖动进度条：按时长比例换算成字节偏移，重新从该位置拉流。
    static bool Seek(int seconds, std::string& error);
    static void SetDuration(int seconds);
    static int GetPositionSeconds();
    static int GetDurationSeconds();
    static void SetSpeed(float speed);
    static float GetSpeed();
    static void SetEventCallback(PodcastPlayerEventCallback callback,
                                 void* ctx);
};
