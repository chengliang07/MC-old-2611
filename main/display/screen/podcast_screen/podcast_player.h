#pragma once

#include <string>

enum class PodcastPlayerEvent {
    Playing,
    Paused,
    Finished,
    Error,
};

using PodcastPlayerEventCallback =
    void (*)(PodcastPlayerEvent event, const char* detail, void* ctx);

class PodcastPlayer {
public:
    static bool Start(const std::string& url, std::string& error);
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
