#include <esp_log.h>
#include <esp_err.h>
#include <esp_app_desc.h>
#include <esp_system.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <lwip/sockets.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include "application.h"
#include "i18n.h"
#include "settings.h"
#include "system_info.h"

#define TAG "main"

#ifndef APP_GIT_COMMIT
#define APP_GIT_COMMIT "unknown"
#endif
#ifndef APP_GIT_DIRTY
#define APP_GIT_DIRTY 0
#endif

// 单行启动横幅：一眼确认"这块板子跑的是哪份代码、连的是哪个后端"。
static void LogBootBanner() {
    Settings podcast("podcast", false);
    std::string api_base = podcast.GetString("api_base", CONFIG_PODCAST_API_BASE);
    std::string device_token = podcast.GetString("device_token", CONFIG_PODCAST_DEVICE_TOKEN);
    ESP_LOGI(TAG, "@@BOOT@@ ver=%s commit=%s dirty=%d api_base=%s token_len=%d",
             esp_app_get_description()->version, APP_GIT_COMMIT, APP_GIT_DIRTY,
             api_base.c_str(), static_cast<int>(device_token.size()));
}

// 调试通道：把关键日志同时通过 UDP 广播到局域网（8266），
// 避免 USB Serial/JTAG 句柄在运行中失效导致无法抓日志。
static int s_diag_udp_sock = -1;
static vprintf_like_t s_prev_log_vprintf = nullptr;
static bool s_diag_udp_enabled = false;
static int s_boot_reset_reason = -1;

static void DiagUdpSend(const char* line);

// 必须在网络（lwIP）初始化完成后再打开，否则 socket() 会导致启动崩溃。
void DiagLogEnableUdp(bool enable) {
    s_diag_udp_enabled = enable;
    if (enable && s_boot_reset_reason >= 0) {
        char line[64];
        snprintf(line, sizeof(line), "@@RESET@@ reason=%d\n",
                 s_boot_reset_reason);
        DiagUdpSend(line);
    }
}

static void DiagUdpSend(const char* line) {
    if (!s_diag_udp_enabled) {
        return;
    }
    if (s_diag_udp_sock < 0) {
        s_diag_udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (s_diag_udp_sock < 0) {
            return;
        }
        int enable = 1;
        setsockopt(s_diag_udp_sock, SOL_SOCKET, SO_BROADCAST, &enable,
                   sizeof(enable));
    }
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8266);
    addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(s_diag_udp_sock, line, strlen(line), 0,
           reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
}

static int DiagLogVprintf(const char* fmt, va_list ap) {
    char buf[256];
    va_list copy;
    va_copy(copy, ap);
    const int n = vsnprintf(buf, sizeof(buf), fmt, copy);
    va_end(copy);
    if (n > 0 &&
        (strstr(buf, "echo diag") != nullptr ||
         strstr(buf, "aec ref delay") != nullptr ||
         strstr(buf, "STATE:") != nullptr)) {
        DiagUdpSend(buf);
    }
    if (s_prev_log_vprintf != nullptr) {
        return s_prev_log_vprintf(fmt, ap);
    }
    return vprintf(fmt, ap);
}

extern "C" void app_main(void)
{
    s_prev_log_vprintf = esp_log_set_vprintf(DiagLogVprintf);
    s_boot_reset_reason = static_cast<int>(esp_reset_reason());
    ESP_LOGI(TAG, "@@RESET@@ reason=%d", s_boot_reset_reason);

    // Initialize the default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize NVS flash for WiFi configuration
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // UI locale from NVS (must run before first screen create).
    I18n::Init();

    LogBootBanner();

    // Launch the application
    auto& app = Application::GetInstance();
    app.Start();
}
