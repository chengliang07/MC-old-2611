#pragma once

// 页面级内存自动体检。
//
// 背景：这颗板子的内部 DRAM 只有约 487KB，历史上多次出现"某个页面进出一次就
// 悄悄少几十 KB / 几 MB"的泄漏，靠人工逐个页面加探针太慢，也容易漏。
//
// 做法：在 screen_util 的生命周期回调里统一调用 LogDelta()，每次页面进入/离开
// 都打一行带差值的内存快照（以该屏上一次快照为基准）。这样任何页面一旦漏内存，
// 第一次进出就会在日志里暴露，而不是等用户用久了崩机。
//
// 日志示例（括号内是相对该屏上一次快照的变化）：
//   [mem] unload scr=0x4c8a1b20 internal=229KB(-42) largest=98KB(-8) psram=19479KB(-2836) tasks=27(+1)

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdint>

namespace mem_probe {

struct Snapshot {
    uint32_t internal_kb = 0;
    uint32_t largest_kb = 0;
    uint32_t psram_kb = 0;
    uint32_t tasks = 0;
};

inline Snapshot Take() {
    Snapshot s;
    s.internal_kb =
        static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
    s.largest_kb = static_cast<uint32_t>(
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024);
    s.psram_kb =
        static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
    s.tasks = static_cast<uint32_t>(uxTaskGetNumberOfTasks());
    return s;
}

namespace detail {
constexpr int kSlots = 10;
struct Slot {
    const void* screen = nullptr;
    Snapshot snap;
    bool used = false;
};
inline Slot (&Slots())[kSlots] {
    static Slot slots[kSlots];
    return slots;
}
inline Slot* FindOrTake(const void* screen) {
    Slot(&slots)[kSlots] = Slots();
    for (int i = 0; i < kSlots; ++i) {
        if (slots[i].used && slots[i].screen == screen) {
            return &slots[i];
        }
    }
    for (int i = 0; i < kSlots; ++i) {
        if (!slots[i].used) {
            slots[i].used = true;
            slots[i].screen = screen;
            return &slots[i];
        }
    }
    // 满了就复用第一个槽（实际页面数不会超过 10 个同时存活）
    slots[0].screen = screen;
    return &slots[0];
}
}  // namespace detail

// phase: "load" / "unload"
inline void LogDelta(const char* phase, const void* screen) {
    if (screen == nullptr) {
        return;
    }
    const Snapshot now = Take();
    detail::Slot* slot = detail::FindOrTake(screen);
    const Snapshot prev = slot->snap;
    slot->snap = now;

    const int32_t d_internal = static_cast<int32_t>(now.internal_kb) - static_cast<int32_t>(prev.internal_kb);
    const int32_t d_largest = static_cast<int32_t>(now.largest_kb) - static_cast<int32_t>(prev.largest_kb);
    const int32_t d_psram = static_cast<int32_t>(now.psram_kb) - static_cast<int32_t>(prev.psram_kb);
    const int32_t d_tasks = static_cast<int32_t>(now.tasks) - static_cast<int32_t>(prev.tasks);

    ESP_LOGI("mem_probe",
             "[mem] %s scr=%p internal=%uKB(%+d) largest=%uKB(%+d) psram=%uKB(%+d) tasks=%u(%+d)",
             phase, screen, static_cast<unsigned>(now.internal_kb), static_cast<int>(d_internal),
             static_cast<unsigned>(now.largest_kb), static_cast<int>(d_largest),
             static_cast<unsigned>(now.psram_kb), static_cast<int>(d_psram),
             static_cast<unsigned>(now.tasks), static_cast<int>(d_tasks));
}

}  // namespace mem_probe
