#pragma once

#include "lvgl.h"
#include "screen_util.h"

class PetScreen {
public:
    static lv_obj_t* Create(lv_obj_t* return_screen = nullptr);
    static void LifecycleCallback(screen_lifecycle_event_t event);

    // 语音对话：宠物页在前台时，聊天消息改投到宠物页字幕。
    // 由 LVAdapterDisplay::SetChatMessage 在持有 esp_lv_adapter 锁后调用。
    static bool IsActive();
    static void ShowUserText(const char* text);
    static void ShowAssistantText(const char* text);
    static void ClearText();
};
