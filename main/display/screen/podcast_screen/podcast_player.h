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
