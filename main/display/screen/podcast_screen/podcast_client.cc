#include "podcast_client.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <cstring>
#include <memory>
#include <mutex>
#include <netdb.h>
#include <set>
#include <string>
#include <sys/socket.h>
#include <vector>

#include "board.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "http.h"
#include "settings.h"
#include "system_info.h"
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
// 同一集可能被多个线程同时解析（页面预取 + 用户点击 + 后台任务），
// 让后来者等第一个的结果，避免重复发请求（实测同一集发了 3 次、白花十几秒）。
std::mutex s_inflight_mutex;
std::condition_variable s_inflight_cv;
std::set<std::string> s_inflight;
// 所有设备 API 请求串行化：并发 TLS 会话会把内部 RAM（实测只剩 130KB 量级）
// 和连接槽吃光，播放流就会 "Failed to open new connection in specified timeout"。
std::mutex s_api_gate_mutex;
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
// /stream 直连是否可用：一旦音频请求被判过期/拒绝，就关掉直连、回退到 /play。
std::atomic<bool> s_direct_stream_enabled{true};

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

// 取出 URL 的 host（跳过 scheme 与端口），用于判断"是不是我们自己的域名"。
std::string HostOfUrl(const std::string& url) {
    const size_t scheme = url.find("://");
    const size_t begin = scheme == std::string::npos ? 0 : scheme + 3;
    size_t end = url.find_first_of("/?#", begin);
    if (end == std::string::npos) {
        end = url.size();
    }
    std::string host = url.substr(begin, end - begin);
    const size_t colon = host.find(':');
    if (colon != std::string::npos) {
        host = host.substr(0, colon);
    }
    for (auto& c : host) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return host;
}

bool UrlBelongsToApiHost(const std::string& url) {
    Settings settings(kSettingsNs, false);
    const std::string api_base = TrimTrailingSlash(
        settings.GetString(kKeyApiBase, CONFIG_PODCAST_API_BASE));
    const std::string api_host = HostOfUrl(api_base);
    if (api_host.empty()) {
        return false;
    }
    return HostOfUrl(url) == api_host;
}

bool HttpGet(const std::string& url, const std::string& device_token,
             std::string& body, int& status, std::string& error,
             int timeout_ms = kHttpTimeoutMs, bool hold_gate = true) {
    // 逐个来：并发的 TLS 握手会互相挤占内部 RAM，直接导致建流失败。
    // 调用方已持锁时（hold_gate=false）不再重复加锁。
    std::unique_lock<std::mutex> gate(s_api_gate_mutex, std::defer_lock);
    if (hold_gate) {
        gate.lock();
    }
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
    http->SetTimeout(timeout_ms);
    http->SetHeader("Accept", "application/json");
    // Cloudflare 会直接 403 掉不带 User-Agent 的请求（固件里表现为“设备未授权”）。
    http->SetHeader("User-Agent", SystemInfo::GetUserAgent());
    // 不再强制 close：每次点击都重握手 TLS 要多花数百毫秒。
    http->SetHeader("Connection", "keep-alive");
    http->SetHeader("Authorization", authorization);
    if (!http->Open("GET", url)) {
        error = "连接播客服务失败";
        return false;
    }

    status = http->GetStatusCode();
    // ReadAll() 对超过 8KB 的 chunked 响应会死锁，这里流式读取。
    body.clear();
    char chunk[2048];
    while (true) {
        const int read = http->Read(chunk, sizeof(chunk));
        if (read < 0) {
            http->Close();
            error = "读取播客服务响应失败";
            return false;
        }
        if (read == 0) {
            break;
        }
        body.append(chunk, static_cast<size_t>(read));
        if (body.size() > 512 * 1024) {
            break;
        }
    }
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
    // 预留 60 秒安全余量，避免在签名地址即将过期时开始播放。
    constexpr time_t kExpiryMarginSeconds = 60;
    for (const auto& entry : s_play_cache) {
        if (entry.id == podcast_id &&
            entry.expires > now + kExpiryMarginSeconds) {
            result = entry.info;
            ESP_LOGI(TAG, "play url cache hit: %s", podcast_id.c_str());
            return true;
        }
    }
    for (auto it = s_play_cache.begin(); it != s_play_cache.end(); ++it) {
        if (it->id == podcast_id) {
            // 该节目地址即将过期，直接移除，避免下次继续命中。
            s_play_cache.erase(it);
            break;
        }
    }
    return false;
}

void PodcastClient::InvalidatePlayUrl(const std::string& podcast_id) {
    std::lock_guard<std::mutex> lock(s_play_cache_mutex);
    for (auto it = s_play_cache.begin(); it != s_play_cache.end(); ++it) {
        if (it->id == podcast_id) {
            s_play_cache.erase(it);
            ESP_LOGI(TAG, "play url cache invalidated: %s",
                     podcast_id.c_str());
            return;
        }
    }
}

void PodcastClient::InvalidateListCache() {
    std::lock_guard<std::mutex> lock(s_list_cache_mutex);
    s_list_cache.clear();
    s_list_cache_at = 0;
    ESP_LOGI(TAG, "list cache invalidated");
}

void PodcastClient::InvalidateAllPlayUrls() {
    std::lock_guard<std::mutex> lock(s_play_cache_mutex);
    s_play_cache.clear();
    ESP_LOGI(TAG, "all play url caches invalidated");
}

void PodcastClient::ReportDirectStreamFailure() {
    if (s_direct_stream_enabled.exchange(false, std::memory_order_acq_rel)) {
        ESP_LOGW(TAG, "direct /stream rejected, fallback to /play");
    }
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
    // 这条链路实测 2~10 秒都在抖：短超时 + 立刻重试一次，比干等 10 秒超时稳。
    constexpr int kListTimeoutMs = 6000;
    if (!HttpGet(url, device_token, body, status, error, kListTimeoutMs)) {
        ESP_LOGW(TAG, "podcast list failed (%s), retrying once", error.c_str());
        error.clear();
        body.clear();
        status = 0;
        if (!HttpGet(url, device_token, body, status, error, kListTimeoutMs)) {
            ESP_LOGE(TAG, "podcast list failed after retry: %s", error.c_str());
            // 有旧数据就先给旧数据：界面上"刷新变暂无节目"比显示略旧更糟。
            std::lock_guard<std::mutex> lock(s_list_cache_mutex);
            if (!s_list_cache.empty()) {
                error.clear();
                return s_list_cache;
            }
            return {};
        }
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
    const int64_t started_us = esp_timer_get_time();
    if (TryCachedPlayUrl(podcast_id, result)) {
        ESP_LOGI(TAG, "[timing] resolve cache hit id=%s dt=%dms",
                 podcast_id.c_str(),
                 static_cast<int>(
                     (esp_timer_get_time() - started_us) / 1000));
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

    // /play 在设备上实测要 3~6 秒（两次 TLS + 服务端排队），而它返回的就是
    // 固定的 /stream 地址（请求自带设备 Bearer）。直接拼出来就能省掉这一整段；
    // 只有真的 403/404 才会调 ReportDirectStreamFailure() 回退到 /play。
    if (s_direct_stream_enabled.load(std::memory_order_relaxed)) {
        result.ok = true;
        result.url = api_base + "/api/device/v1/podcasts/" +
                     UrlEncode(podcast_id) + "/stream";
        result.format = "mp3";
        {
            std::lock_guard<std::mutex> lock(s_play_cache_mutex);
            bool replaced = false;
            for (auto& entry : s_play_cache) {
                if (entry.id == podcast_id) {
                    entry.info = result;
                    entry.expires = time(nullptr) + 10 * 60;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                if (s_play_cache.size() >= 8) {
                    s_play_cache.erase(s_play_cache.begin());
                }
                s_play_cache.push_back(
                    {podcast_id, result, time(nullptr) + 10 * 60});
            }
        }
        ESP_LOGI(TAG, "[timing] resolve direct id=%s dt=%dms",
                 podcast_id.c_str(),
                 static_cast<int>((esp_timer_get_time() - started_us) / 1000));
        return result;
    }

    // 并发去重：如果别的线程正在解析同一集，就等它的结果。
    // 最多等 2 秒：预取线程若卡在慢请求上，点击不能跟着一起等（实测能等出十几秒），
    // 超时就自己发一次请求，宁可多一次请求也不能让用户干等。
    struct InflightClaim {
        std::string id;
        bool held = false;
        ~InflightClaim() {
            if (!held) {
                return;
            }
            {
                std::lock_guard<std::mutex> lk(s_inflight_mutex);
                s_inflight.erase(id);
            }
            s_inflight_cv.notify_all();
        }
    } claim;
    {
        std::unique_lock<std::mutex> lk(s_inflight_mutex);
        if (s_inflight.count(podcast_id) != 0) {
            s_inflight_cv.wait_for(lk, std::chrono::seconds(2), [&]() {
                return s_inflight.count(podcast_id) == 0;
            });
            if (TryCachedPlayUrl(podcast_id, result)) {
                ESP_LOGI(TAG, "[timing] resolve shared id=%s dt=%dms",
                         podcast_id.c_str(),
                         static_cast<int>(
                             (esp_timer_get_time() - started_us) / 1000));
                return result;
            }
        }
        s_inflight.insert(podcast_id);
        claim.id = podcast_id;
        claim.held = true;
    }
    // 先拿全局请求锁再二次查缓存：预取线程可能刚把这条地址填好，
    // 直接命中就省掉一次 2~3 秒的请求（点击时这点最明显）。
    std::lock_guard<std::mutex> gate(s_api_gate_mutex);
    if (TryCachedPlayUrl(podcast_id, result)) {
        ESP_LOGI(TAG, "[timing] resolve cache hit(queued) id=%s dt=%dms",
                 podcast_id.c_str(),
                 static_cast<int>((esp_timer_get_time() - started_us) / 1000));
        return result;
    }
    // 服务端偶发卡顿（实测出现过 10s 超时）：用更短的超时 + 立刻重试，
    // 比在默认 10s 上干等再重试快得多。
    constexpr int kResolveTimeoutMs = 5000;
    if (!HttpGet(url, device_token, body, status, error, kResolveTimeoutMs,
                 /*hold_gate=*/false)) {
        ESP_LOGW(TAG, "resolve failed (%s), retrying once", error.c_str());
        error.clear();
        body.clear();
        status = 0;
        if (!HttpGet(url, device_token, body, status, error,
                     kResolveTimeoutMs, /*hold_gate=*/false)) {
            result.error = error;
            return result;
        }
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
    ESP_LOGI(TAG, "[timing] resolve api id=%s dt=%dms status=%d",
             podcast_id.c_str(),
             static_cast<int>((esp_timer_get_time() - started_us) / 1000),
             status);
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

void PodcastClient::WarmUpHost(const std::string& url) {
    const std::string host = HostOfUrl(url);
    if (host.empty()) {
        return;
    }
    const int64_t started_us = esp_timer_get_time();
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* result = nullptr;
    const int rc = getaddrinfo(host.c_str(), "443", &hints, &result);
    if (result != nullptr) {
        freeaddrinfo(result);
    }
    ESP_LOGI(TAG, "[timing] dns host=%s rc=%d dt=%dms", host.c_str(), rc,
             static_cast<int>((esp_timer_get_time() - started_us) / 1000));
}

bool PodcastClient::IsOwnHost(const std::string& url) {
    return UrlBelongsToApiHost(url);
}

// 旧的"开机预取列表"实现已删除：没有任何调用点，且落盘格式缺少
// `#<timestamp>` 头，与 LoadListCache 的约定不一致（一旦接回去每次都会判过期）。

