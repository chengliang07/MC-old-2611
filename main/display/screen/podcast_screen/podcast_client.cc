#include "podcast_client.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "board.h"
#include "cJSON.h"
#include "esp_log.h"
#include "http.h"
#include "settings.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr const char* TAG = "PodcastClient";
constexpr const char* kSettingsNs = "podcast";
constexpr const char* kKeyApiBase = "api_base";
constexpr const char* kKeyDeviceToken = "device_token";
constexpr int kHttpTimeoutMs = 10000;

// 列表响应缓存：接口本身约 2-3 秒，避免同一次入口/下拉刷新重复请求。
std::mutex s_list_cache_mutex;
PodcastList s_list_cache;
time_t s_list_cache_at = 0;

// 播放地址缓存：签名地址 60 分钟有效，但设备切到不同 WiFi / 网络后
// JSON 内的签名主机可能暂时不可达；缩短缓存让下一次播放尽快重新换地址。
constexpr int kPlayCacheSeconds = 45 * 60;

struct PlayCacheEntry {
    std::string id;
    PodcastPlayInfo info;
    time_t expires = 0;
};

std::mutex s_play_cache_mutex;
std::vector<PlayCacheEntry> s_play_cache;

std::string TrimTrailingSlash(std::string value) {
    while (!value.empty() && value.back() == '/') {
        value.pop_back();
    }
    return value;
}

std::string UrlEncode(const std::string& value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 3);
    for (unsigned char ch : value) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' ||
            ch == '~') {
            out.push_back(static_cast<char>(ch));
        } else {
            out.push_back('%');
            out.push_back(kHex[ch >> 4]);
            out.push_back(kHex[ch & 0x0F]);
        }
    }
    return out;
}

const char* JsonString(const cJSON* object, const char* primary,
                       const char* fallback = nullptr) {
    const cJSON* item = cJSON_GetObjectItem(object, primary);
    if (!cJSON_IsString(item) && fallback != nullptr) {
        item = cJSON_GetObjectItem(object, fallback);
    }
    return cJSON_IsString(item) ? item->valuestring : "";
}

int JsonInt(const cJSON* object, const char* primary,
            const char* fallback = nullptr) {
    const cJSON* item = cJSON_GetObjectItem(object, primary);
    if (!cJSON_IsNumber(item) && fallback != nullptr) {
        item = cJSON_GetObjectItem(object, fallback);
    }
    return cJSON_IsNumber(item) ? item->valueint : 0;
}

bool JsonBool(const cJSON* object, const char* primary,
              const char* fallback = nullptr) {
    const cJSON* item = cJSON_GetObjectItem(object, primary);
    if (item == nullptr && fallback != nullptr) {
        item = cJSON_GetObjectItem(object, fallback);
    }
    return cJSON_IsTrue(item);
}

const cJSON* FindPodcastArray(const cJSON* root) {
    if (cJSON_IsArray(root)) {
        return root;
    }
    if (!cJSON_IsObject(root)) {
        return nullptr;
    }
    const cJSON* podcasts = cJSON_GetObjectItem(root, "podcasts");
    if (cJSON_IsArray(podcasts)) {
        return podcasts;
    }
    const cJSON* data = cJSON_GetObjectItem(root, "data");
    if (cJSON_IsArray(data)) {
        return data;
    }
    if (cJSON_IsObject(data)) {
        const cJSON* nested = cJSON_GetObjectItem(data, "podcasts");
        if (cJSON_IsArray(nested)) {
            return nested;
        }
    }
    return nullptr;
}

bool LoadConfig(std::string& api_base, std::string& device_token,
                std::string& error) {
    Settings settings(kSettingsNs, false);
    api_base = TrimTrailingSlash(settings.GetString(kKeyApiBase, CONFIG_PODCAST_API_BASE));
    device_token = settings.GetString(kKeyDeviceToken, CONFIG_PODCAST_DEVICE_TOKEN);
    if (api_base.empty() || device_token.empty()) {
        error = "播客服务未配置\n请联系管理员绑定设备";
        return false;
    }
    return true;
}

bool HttpGet(const std::string& url, const std::string& device_token,
             std::string& body, int& status, std::string& error) {
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        error = "网络未连接";
        return false;
    }
    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        error = "创建网络请求失败";
        return false;
    }

    const std::string authorization = "Bearer " + device_token;
    http->SetTimeout(kHttpTimeoutMs);
    http->SetHeader("Accept", "application/json");
    http->SetHeader("Connection", "close");
    http->SetHeader("Authorization", authorization);
    if (!http->Open("GET", url)) {
        error = "连接播客服务失败";
        return false;
    }

    status = http->GetStatusCode();
    body = http->ReadAll();
    http->Close();
    if (status < 200 || status >= 300) {
        if (status == 401 || status == 403) {
            error = "设备未授权";
        } else {
            error = "播客服务返回 HTTP " + std::to_string(status);
        }
        return false;
    }
    return true;
}

}  // namespace

bool TryCachedPlayUrl(const std::string& podcast_id, PodcastPlayInfo& result) {
    std::lock_guard<std::mutex> lock(s_play_cache_mutex);
    const time_t now = time(nullptr);
    for (const auto& entry : s_play_cache) {
        if (entry.id == podcast_id && entry.expires > now) {
            result = entry.info;
            ESP_LOGI(TAG, "play url cache hit: %s", podcast_id.c_str());
            return true;
        }
    }
    return false;
}

bool IsLeapYear(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

time_t UtcTime(struct tm* value) {
    static constexpr int kMonthDays[] = {0,  31, 59, 90,  120, 151,
                                         181, 212, 243, 273, 304, 334};
    if (value->tm_mon < 0 || value->tm_mon > 11) {
        return 0;
    }
    const int year = value->tm_year + 1900;
    int days = 0;
    if (year >= 1970) {
        for (int y = 1970; y < year; ++y) {
            days += 365 + (IsLeapYear(y) ? 1 : 0);
        }
    } else {
        for (int y = year; y < 1970; ++y) {
            days -= 365 + (IsLeapYear(y) ? 1 : 0);
        }
    }
    days += kMonthDays[value->tm_mon] +
            ((value->tm_mon > 1 && IsLeapYear(year)) ? 1 : 0) +
            value->tm_mday - 1;
    return static_cast<time_t>(days) * 86400 +
           value->tm_hour * 3600 + value->tm_min * 60 + value->tm_sec;
}

time_t ParseExpiresAt(const std::string& value) {
    struct tm tm_value = {};
    char* end =
        value.empty()
            ? nullptr
            : strptime(value.c_str(), "%Y-%m-%dT%H:%M:%S", &tm_value);
    if (end == nullptr) {
        return 0;
    }
    // 后端返回 ISO-8601 UTC，例如 2026-11-22T06:56:37.712Z。
    // 必须按 UTC 解析；按本地时间解析会让缓存一生成就过期。
    while (*end == '.' ||
           std::isdigit(static_cast<unsigned char>(*end))) {
        ++end;
    }
    if (*end == 'Z' || *end == 'z') {
        return UtcTime(&tm_value);
    }
    if (*end == '+' || *end == '-') {
        int hours = 0;
        int minutes = 0;
        if (std::sscanf(end + 1, "%2d:%2d", &hours, &minutes) == 2) {
            const time_t base = UtcTime(&tm_value);
            const int offset = hours * 3600 + minutes * 60;
            return *end == '+' ? base - offset : base + offset;
        }
    }
    return mktime(&tm_value);
}

PodcastList PodcastClient::FetchPodcasts(std::string& error) {
    error.clear();
    const time_t now = time(nullptr);
    {
        std::lock_guard<std::mutex> lock(s_list_cache_mutex);
        // 列表不需要每次进入都刷新；半小时内直接复用，避免重复 2-4 秒请求。
        if (!s_list_cache.empty() && s_list_cache_at > 0 &&
            now - s_list_cache_at < 30 * 60) {
            return s_list_cache;
        }
    }
    std::string api_base;
    std::string device_token;
    if (!LoadConfig(api_base, device_token, error)) {
        return {};
    }

    std::string body;
    int status = 0;
    const std::string url = api_base + "/api/device/v1/podcasts";
    if (!HttpGet(url, device_token, body, status, error)) {
        ESP_LOGW(TAG, "podcast list failed: %s", error.c_str());
        return {};
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        error = "节目列表格式错误";
        return {};
    }
    const cJSON* array = FindPodcastArray(root);
    if (array == nullptr) {
        cJSON_Delete(root);
        error = "节目列表字段缺失";
        return {};
    }

    PodcastList result;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, array) {
        if (!cJSON_IsObject(item)) {
            continue;
        }
        PodcastItem podcast;
        podcast.id = JsonString(item, "id", "podcast_id");
        podcast.title = JsonString(item, "title");
        podcast.description =
            JsonString(item, "description", "summary");
        podcast.cover_url = JsonString(item, "cover_url", "cover");
        podcast.duration_seconds =
            JsonInt(item, "duration_seconds", "duration");
        podcast.completed = JsonBool(item, "completed", "is_completed");
        // 列表接口若已带回临时播放地址，直接种进缓存，点击时无需再等 /play。
        const char* list_play_url = JsonString(item, "play_url", "url");
        if (list_play_url[0] != '\0' && !podcast.id.empty()) {
            PodcastPlayInfo info;
            info.ok = true;
            info.url = list_play_url;
            info.format = JsonString(item, "format");
            info.expires_at =
                JsonString(item, "expires_at", "expiresAt");
            time_t expires = ParseExpiresAt(info.expires_at);
            if (expires <= 0) {
                expires = time(nullptr) + kPlayCacheSeconds;
            }
            std::lock_guard<std::mutex> lock(s_play_cache_mutex);
            bool replaced = false;
            for (auto& entry : s_play_cache) {
                if (entry.id == podcast.id) {
                    entry.info = info;
                    entry.expires = expires;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                if (s_play_cache.size() >= 8) {
                    s_play_cache.erase(s_play_cache.begin());
                }
                s_play_cache.push_back({podcast.id, info, expires});
            }
        }
        if (!podcast.id.empty() && !podcast.title.empty()) {
            result.push_back(std::move(podcast));
        }
    }
    cJSON_Delete(root);
    {
        std::lock_guard<std::mutex> lock(s_list_cache_mutex);
        s_list_cache = result;
        s_list_cache_at = time(nullptr);
    }
    return result;
}

PodcastPlayInfo PodcastClient::ResolvePlayUrl(const std::string& podcast_id,
                                              std::string& error) {
    error.clear();
    PodcastPlayInfo result;
    if (TryCachedPlayUrl(podcast_id, result)) {
        return result;
    }
    std::string api_base;
    std::string device_token;
    if (!LoadConfig(api_base, device_token, error)) {
        result.error = error;
        return result;
    }

    std::string body;
    int status = 0;
    const std::string url = api_base + "/api/device/v1/podcasts/" +
                            UrlEncode(podcast_id) + "/play";
    if (!HttpGet(url, device_token, body, status, error)) {
        result.error = error;
        return result;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        error = "播放地址格式错误";
        result.error = error;
        return result;
    }
    const cJSON* data = cJSON_GetObjectItem(root, "data");
    if (!cJSON_IsObject(data)) {
        data = root;
    }
    result.url = JsonString(data, "url", "play_url");
    if (result.url.empty()) {
        result.url = JsonString(data, "signed_url");
    }
    result.format = JsonString(data, "format");
    result.expires_at = JsonString(data, "expires_at", "expiresAt");
    result.duration_seconds =
        JsonInt(data, "duration_seconds", "duration");
    cJSON_Delete(root);

    if (result.url.empty()) {
        error = "播放地址为空";
        result.error = error;
        return result;
    }
    result.ok = true;
    {
        std::lock_guard<std::mutex> lock(s_play_cache_mutex);
        time_t expires = ParseExpiresAt(result.expires_at);
        if (expires <= 0) {
            expires = time(nullptr) + kPlayCacheSeconds;
        }
        bool replaced = false;
        for (auto& entry : s_play_cache) {
            if (entry.id == podcast_id) {
                entry.info = result;
                entry.expires = expires;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            if (s_play_cache.size() >= 8) {
                s_play_cache.erase(s_play_cache.begin());
            }
            s_play_cache.push_back({podcast_id, result, expires});
        }
    }
    return result;
}

std::string PodcastClient::DeviceToken() {
    Settings settings(kSettingsNs, false);
    return settings.GetString(kKeyDeviceToken, CONFIG_PODCAST_DEVICE_TOKEN);
}

void PodcastClient::PrefetchListAsync() {
    static std::atomic<bool> started{false};
    if (started.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    xTaskCreate(
        [](void* /*arg*/) {
            // 等开机联网/激活流程稳定后再抓，失败就放弃，不打扰用户。
            vTaskDelay(pdMS_TO_TICKS(8000));
            std::string error;
            const PodcastList items = FetchPodcasts(error);
            if (error.empty() && !items.empty()) {
                const char* tmp = "/sdcard/system/podcast_list.tmp";
                FILE* file = std::fopen(tmp, "w");
                if (file != nullptr) {
                    for (const auto& item : items) {
                        std::fprintf(file, "%s|%s|%d|%d\n", item.id.c_str(),
                                     item.title.c_str(),
                                     item.duration_seconds,
                                     item.completed ? 1 : 0);
                    }
                    std::fclose(file);
                    std::rename(tmp, "/sdcard/system/podcast_list.txt");
                }
            }
            vTaskDelete(nullptr);
        },
        "podcast_prefetch", 8192, nullptr, 3, nullptr);
}
