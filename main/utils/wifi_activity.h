#pragma once

// WiFi 活动协调标记。
//
// 起因（真机现场）：删掉唯一保存的 WiFi 后，"永不放弃重连"任务每 15 秒调一次
// esp_wifi_connect()，而 esp_wifi_connect() 会打断正在进行的扫描 —— 表现就是
// 网络设置页的"附近 WiFi"永远是空的（明明有网络）。
//
// 这里用一个全局标记让重连任务在扫描期间让路。

#include <atomic>

namespace wifi_activity {

inline std::atomic<bool>& Scanning() {
    static std::atomic<bool> flag{false};
    return flag;
}

inline bool IsScanning() {
    return Scanning().load(std::memory_order_acquire);
}

inline void SetScanning(bool scanning) {
    Scanning().store(scanning, std::memory_order_release);
}

}  // namespace wifi_activity
