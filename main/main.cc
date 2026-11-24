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
#include "board.h"
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

// ---------------------------------------------------------------------------
// 一次性设备凭据导入：把 SD 卡根目录的绑定文件写进 NVS，避免 token 进固件。
//
// 出厂/换绑：把 podcast_token.txt 放到 SD 卡根目录，开机导入一次后文件被删除。
// 内容一行一个 key=value（'#' 开头为注释；整份只有一行裸 token 也接受）：
//     token=<设备令牌>                   // 必填
//     api_base=https://www.yi-edu.com    // 可选，不给则沿用原值
//
// 这里直接调 nvs_* 而不走 Settings::EnqueueWrite：启动早期需要"写完立刻可读"，
// 异步写队列既不保证顺序，也无法在 Application::Start() 之前完成落盘。
// 本函数运行在 main 任务的内部 RAM 栈上，不存在 PSRAM 栈写 Flash 的断言问题。
// ---------------------------------------------------------------------------
namespace {

constexpr const char* kProvisionPath = "/sdcard/podcast_token.txt";
constexpr const char* kProvisionNs = "podcast";

std::string TrimAscii(const std::string& in) {
    size_t begin = 0;
    size_t end = in.size();
    while (begin < end) {
        const char c = in[begin];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            break;
        }
        ++begin;
    }
    while (end > begin) {
        const char c = in[end - 1];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            break;
        }
        --end;
    }
    return in.substr(begin, end - begin);
}

// 凭据只接受常见字符并限定长度：挡住空文件、二进制垃圾，
// 以及"误把整份说明文本当成 token"的情况。
bool IsPlausibleToken(const std::string& value) {
    if (value.size() < 16 || value.size() > 512) {
        return false;
    }
    for (char c : value) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                        (c >= 'A' && c <= 'Z') || c == '-' || c == '_' ||
                        c == '.' || c == '~' || c == '+' || c == '/' ||
                        c == '=';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool IsPlausibleBaseUrl(const std::string& value) {
    if (value.size() < 8 || value.size() > 128) {
        return false;
    }
    return value.compare(0, 8, "https://") == 0 ||
           value.compare(0, 7, "http://") == 0;
}

}  // namespace

static void ImportProvisioningFromSdCard() {
    FILE* file = std::fopen(kProvisionPath, "rb");
    if (file == nullptr) {
        // 没有文件、或 SD 没插：都是正常情况，不打扰用户。
        return;
    }
    char buf[512] = {};
    const size_t got = std::fread(buf, 1, sizeof(buf) - 1, file);
    std::fclose(file);

    std::string token;
    std::string api_base;
    const std::string text(buf, got);
    size_t begin = 0;
    while (begin <= text.size()) {
        size_t end = text.find('\n', begin);
        if (end == std::string::npos) {
            end = text.size();
        }
        const std::string line = TrimAscii(text.substr(begin, end - begin));
        begin = end + 1;
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            if (token.empty()) {
                token = line;  // 兼容"整份文件只有一行裸 token"
            }
            continue;
        }
        const std::string key = TrimAscii(line.substr(0, eq));
        const std::string value = TrimAscii(line.substr(eq + 1));
        if (key == "token" || key == "device_token") {
            token = value;
        } else if (key == "api_base") {
            api_base = value;
        }
    }

    if (!IsPlausibleToken(token)) {
        ESP_LOGE(TAG, "provisioning: %s has no usable token, file kept",
                 kProvisionPath);
        return;
    }
    if (!api_base.empty() && !IsPlausibleBaseUrl(api_base)) {
        ESP_LOGW(TAG, "provisioning: api_base looks invalid, ignored");
        api_base.clear();
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kProvisionNs, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "device_token", token.c_str());
        if (err == ESP_OK && !api_base.empty()) {
            err = nvs_set_str(handle, "api_base", api_base.c_str());
        }
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "provisioning: nvs write failed (%s), file kept",
                 esp_err_to_name(err));
        return;
    }

    // 只打长度：日志会被 UDP 广播到局域网，绝不能带凭据原文。
    ESP_LOGI(TAG, "provisioning: imported token_len=%u api_base=%s",
             static_cast<unsigned>(token.size()),
             api_base.empty() ? "(unchanged)" : api_base.c_str());
    if (std::remove(kProvisionPath) == 0) {
        ESP_LOGI(TAG, "provisioning: %s removed", kProvisionPath);
    }
}

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

    // 板级构造会挂载 /sdcard（metalio-claw-4.cc 的 InitializeSdCard），
    // 一次性凭据导入依赖它，所以先确保板级单例已经构造。
    (void)Board::GetInstance();
    ImportProvisioningFromSdCard();

    LogBootBanner();

    // Launch the application
    auto& app = Application::GetInstance();
    app.Start();
}
