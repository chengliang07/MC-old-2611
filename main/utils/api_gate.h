#pragma once

// 全局网络/音频并发闸。
//
// 背景：这颗 P4 内部 RAM 只有约 487KB，而三类"重"资源各自要 30~130KB：
//   - HTTPS 请求（一条 TLS 约 35KB）
//   - 播客音频流（TLS + 解码 + 播放缓冲）
//   - 语音会话（AEC/唤醒词/Opus，实测约 130KB）
// 它们若同时开，会把内部 RAM 打到见底，最先崩的是 esp-aes 的 DMA 描述符
// 分配，连锁到 MQTT 的 TLS 写不出去 → 全模块"发送失败" → 只能重启。
//
// 这里提供两件事：
//   1) Mutex()：同一时刻只允许一条 HTTPS 在建连（播客/英语/宠物共用一个）。
//   2) VoiceFlag()：语音会话优先，播放器在语音进行中直接拒绝开播。

#include <atomic>
#include <mutex>

namespace api_gate {

// 所有 HTTPS 请求共用这一把锁。
inline std::mutex& Mutex() {
    static std::mutex mutex;
    return mutex;
}

inline std::atomic<bool>& VoiceFlag() {
    static std::atomic<bool> flag{false};
    return flag;
}

inline bool IsVoiceActive() {
    return VoiceFlag().load(std::memory_order_acquire);
}

// 语音会话开始/结束：由 Application 的状态切换调用。
inline void BeginVoice() {
    VoiceFlag().store(true, std::memory_order_release);
}

inline void EndVoice() {
    VoiceFlag().store(false, std::memory_order_release);
}

}  // namespace api_gate
