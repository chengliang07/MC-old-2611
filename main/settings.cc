#include "settings.h"

#include <functional>
#include <mutex>
#include <string>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <nvs_flash.h>

#define TAG "Settings"

// NVS 写入会关闭 Cache 并访问 Flash；LVGL/其他组件的栈在 PSRAM 时
// 直接写会触发 spi_flash_disable_interrupts_caches_and_other_cpu 断言。
// 统一投递到内部 RAM 栈专用任务，且串行执行避免并发写 Flash。
struct NvsWriteJob {
    std::string ns;
    std::function<void(nvs_handle_t)> fn;
};

QueueHandle_t s_write_queue = nullptr;
StaticTask_t s_writer_tcb;
StaticQueue_t s_write_queue_cb;
StackType_t s_writer_stack[3072];
// 音量拖动 / 亮度滑动这类高频写很容易把队列刷满；32 槽足够覆盖一次手势。
constexpr size_t kNvsWriteQueueDepth = 32;
uint32_t s_write_queue_storage[kNvsWriteQueueDepth * sizeof(NvsWriteJob*) /
                               sizeof(uint32_t)];

void NvsWriterTask(void* /*arg*/) {
    NvsWriteJob* job = nullptr;
    for (;;) {
        if (xQueueReceive(s_write_queue, &job, portMAX_DELAY) != pdTRUE ||
            job == nullptr) {
            continue;
        }
        nvs_handle_t handle = 0;
        if (nvs_open(job->ns.c_str(), NVS_READWRITE, &handle) == ESP_OK) {
            job->fn(handle);
            nvs_commit(handle);
            nvs_close(handle);
        }
        delete job;
        job = nullptr;
    }
}

QueueHandle_t GetWriteQueue() {
    static std::once_flag once;
    std::call_once(once, []() {
        s_write_queue = xQueueCreateStatic(
            kNvsWriteQueueDepth, sizeof(NvsWriteJob*),
            reinterpret_cast<uint8_t*>(s_write_queue_storage),
            &s_write_queue_cb);
        if (s_write_queue == nullptr) {
            ESP_LOGE(TAG, "create nvs write queue failed");
            return;
        }
        if (xTaskCreateStatic(NvsWriterTask, "nvs_writer",
                              sizeof(s_writer_stack) / sizeof(StackType_t),
                              nullptr, 5, s_writer_stack,
                              &s_writer_tcb) == nullptr) {
            ESP_LOGE(TAG, "create nvs writer task failed");
            vQueueDelete(s_write_queue);
            s_write_queue = nullptr;
        }
    });
    return s_write_queue;
}

void Settings::EnqueueWrite(
    const std::string& ns,
    const std::function<void(nvs_handle_t)>& fn) {
    QueueHandle_t queue = GetWriteQueue();
    if (queue == nullptr) {
        ESP_LOGE(TAG, "nvs writer unavailable, drop write for %s",
                 ns.c_str());
        return;
    }
    auto* job = new NvsWriteJob{ns, fn};
    // 只等最多 20ms（约一帧）：正常情况立刻入队；真赶上队列满时宁可让
    // 调用方卡一帧，也不静默丢写（音量/亮度拖动刷满队列时会丢设置）。
    if (xQueueSend(queue, &job, pdMS_TO_TICKS(20)) != pdTRUE) {
        ESP_LOGE(TAG, "nvs write queue full after 20ms, drop write for %s",
                 ns.c_str());
        delete job;
    }
}

bool Settings::Flush(uint32_t timeout_ms) {
    QueueHandle_t queue = GetWriteQueue();
    if (queue == nullptr) {
        return false;
    }
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (uxQueueMessagesWaiting(queue) > 0) {
        if (xTaskGetTickCount() > deadline) {
            ESP_LOGW(TAG, "nvs write flush timed out, %u left",
                     static_cast<unsigned>(uxQueueMessagesWaiting(queue)));
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

Settings::Settings(const std::string& ns, bool read_write) : ns_(ns), read_write_(read_write) {
    // 写入走专用任务；这里不打开句柄，避免 LVGL/UI 任务直接碰 Flash。
    if (!read_write_) {
        nvs_open(ns.c_str(), NVS_READONLY, &nvs_handle_);
    }
}

Settings::~Settings() {
    if (nvs_handle_ != 0) {
        nvs_close(nvs_handle_);
    }
}

nvs_handle_t Settings::Handle() {
    if (nvs_handle_ == 0) {
        // 所有读取都走只读句柄。即使调用方构造的是写入型 Settings，
        // 也不能因为一次 Get* 就同步 nvs_open(READWRITE) 写 Flash。
        nvs_open(ns_.c_str(), NVS_READONLY, &nvs_handle_);
    }
    return nvs_handle_;
}

std::string Settings::GetString(const std::string& key, const std::string& default_value) {
    nvs_handle_t handle = Handle();
    if (handle == 0) {
        return default_value;
    }

    size_t length = 0;
    if (nvs_get_str(handle, key.c_str(), nullptr, &length) != ESP_OK) {
        return default_value;
    }

    std::string value;
    esp_err_t err = ESP_OK;
    value.resize(length);
    if (length > 0) {
        err = nvs_get_str(handle, key.c_str(), value.data(), &length);
    }
    if (err != ESP_OK) {
        // 两次查询之间写任务改了同一个 key 时长度会变（INVALID_LENGTH）。
        // 重试一次再放弃：这里绝不能用 ESP_ERROR_CHECK，否则设置写一半就整机重启。
        length = 0;
        if (nvs_get_str(handle, key.c_str(), nullptr, &length) != ESP_OK ||
            length == 0) {
            return default_value;
        }
        value.assign(length, '\0');
        if (nvs_get_str(handle, key.c_str(), value.data(), &length) != ESP_OK) {
            return default_value;
        }
    }
    while (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }
    return value;
}

void Settings::SetString(const std::string& key, const std::string& value) {
    if (read_write_) {
        EnqueueWrite(ns_, [key, value](nvs_handle_t handle) {
            nvs_set_str(handle, key.c_str(), value.c_str());
        });
    } else {
        ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
    }
}

int32_t Settings::GetInt(const std::string& key, int32_t default_value) {
    nvs_handle_t handle = Handle();
    if (handle == 0) {
        return default_value;
    }

    int32_t value;
    if (nvs_get_i32(handle, key.c_str(), &value) != ESP_OK) {
        return default_value;
    }
    return value;
}

void Settings::SetInt(const std::string& key, int32_t value) {
    if (read_write_) {
        EnqueueWrite(ns_, [key, value](nvs_handle_t handle) {
            nvs_set_i32(handle, key.c_str(), value);
        });
    } else {
        ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
    }
}

bool Settings::GetBool(const std::string& key, bool default_value) {
    nvs_handle_t handle = Handle();
    if (handle == 0) {
        return default_value;
    }

    uint8_t value;
    if (nvs_get_u8(handle, key.c_str(), &value) != ESP_OK) {
        return default_value;
    }
    return value != 0;
}

void Settings::SetBool(const std::string& key, bool value) {
    if (read_write_) {
        EnqueueWrite(ns_, [key, value](nvs_handle_t handle) {
            nvs_set_u8(handle, key.c_str(), value ? 1 : 0);
        });
    } else {
        ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
    }
}

void Settings::EraseKey(const std::string& key) {
    if (read_write_) {
        EnqueueWrite(ns_, [key](nvs_handle_t handle) {
            auto ret = nvs_erase_key(handle, key.c_str());
            (void)ret;
        });
    } else {
        ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
    }
}

void Settings::EraseAll() {
    if (read_write_) {
        EnqueueWrite(ns_, [](nvs_handle_t handle) {
            nvs_erase_all(handle);
        });
    } else {
        ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
    }
}
