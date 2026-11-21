#pragma once

#include "lvgl.h"
#include "screen_util.h"

class OralScreen {
 public:
    static lv_obj_t* Create(lv_obj_t* return_screen = nullptr);
    static void LifecycleCallback(screen_lifecycle_event_t event);
    static bool IsActive();
    static void Interrupt();
    static void RequestInterrupt();
};
