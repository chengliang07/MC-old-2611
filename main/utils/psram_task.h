#pragma once

// 把任务栈放进 PSRAM 的统一封装。
//
// 背景：这颗 P4 的内部 DRAM 只有约 487KB（静态已占 74KB），而任务栈默认就从
// 内部 RAM 分配。新增的页面（口语/播客/宠物/英语）一共有几十 KB 的栈需求，
// 叠上 TLS(30~40KB/条) 和 LVGL 后，内部 RAM 长期在 150KB 上下、最大连续块
// 只剩十几 KB —— 表现为卡顿、断网、偶发重启。
//
// 栈搬到 PSRAM 的代价只是访问稍慢（实测无感），收益是内部 RAM 直接回血。
// 注意：用 CreatePsramTask 建的任务必须用 DeleteCurrentPsramTask 自行退出，
// 否则 PSRAM 栈不会释放。

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#if CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
#include <freertos/idf_additions.h>
#endif

inline BaseType_t CreatePsramTask(TaskFunction_t function, const char* name,
                                  uint32_t stack_depth, void* arg,
                                  UBaseType_t priority,
                                  TaskHandle_t* handle = nullptr) {
#if CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
    const BaseType_t result =
        xTaskCreateWithCaps(function, name, stack_depth, arg, priority, handle,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (result == pdPASS) {
        return result;
    }
    // PSRAM 建不出来时退回内部 RAM，宁可占内部也不让功能起不来。
#endif
    return xTaskCreate(function, name, stack_depth, arg, priority, handle);
}

inline void DeleteCurrentPsramTask() {
#if CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
    vTaskDeleteWithCaps(nullptr);
#else
    vTaskDelete(nullptr);
#endif
}
