#include "english_client.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "board.h"
#include "cJSON.h"
#include "esp_log.h"
#include "http.h"
#include "settings.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr const char* TAG = "EnglishClient";
constexpr const char* kSettingsNs = "podcast";
constexpr const char* kKeyDeviceToken = "device_token";
constexpr const char* kRequiredApiBase = "https://www.yi-edu.com";
constexpr const char* kPronunciationBase = "https://dict.youdao.com/dictvoice";
constexpr const char* kReviewCacheDir = "/sdcard/system";
constexpr int kHttpTimeoutMs = 6000;

std::mutex s_http_mutex;

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

bool LoadConfig(std::string& api_base, std::string& device_token,
                std::string& error) {
    Settings settings(kSettingsNs, false);
    api_base = kRequiredApiBase;
    device_token =
        settings.GetString(kKeyDeviceToken, CONFIG_PODCAST_DEVICE_TOKEN);
    if (device_token.empty()) {
        error = "设备未绑定\n请联系管理员配置";
        return false;
    }
    return true;
}

bool HttpRequest(const char* method, const std::string& url,
                 const std::string& device_token, std::string content,
                 std::string& body, int& status, std::string& error) {
    std::lock_guard<std::mutex> lock(s_http_mutex);
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

    http->SetTimeout(kHttpTimeoutMs);
    http->SetHeader("Accept", "application/json");
    http->SetHeader("Connection", "close");
    http->SetHeader("Authorization", "Bearer " + device_token);
    if (!content.empty()) {
        http->SetHeader("Content-Type", "application/json");
        http->SetContent(std::move(content));
    }
    if (!http->Open(method, url)) {
        error = "连接英语服务失败";
        return false;
    }
    status = http->GetStatusCode();
    body = http->ReadAll();
    http->Close();
    if (status < 200 || status >= 300) {
        cJSON* root = cJSON_Parse(body.c_str());
        const char* detail = root != nullptr ? JsonString(root, "error") : "";
        if (detail == nullptr || detail[0] == '\0') {
            error = "英语服务返回 HTTP " + std::to_string(status);
        } else {
            error = detail;
        }
        cJSON_Delete(root);
        return false;
    }
    return true;
}

bool GetJson(const std::string& path, const std::string& api_base,
             const std::string& device_token, std::string& body,
             std::string& error) {
    int status = 0;
    return HttpRequest("GET", api_base + path, device_token, "", body,
                       status, error);
}

std::string ReviewCachePath(const std::string& task_id) {
    return std::string(kReviewCacheDir) + "/english_review_" + task_id +
           ".json";
}

void SaveReviewCache(const std::string& task_id, const std::string& body) {
    if (task_id.empty() || body.empty()) {
        return;
    }
    const std::string path = ReviewCachePath(task_id);
    const std::string tmp = path + ".tmp";
    FILE* file = std::fopen(tmp.c_str(), "w");
    if (file == nullptr) {
        return;
    }
    const size_t written =
        std::fwrite(body.data(), 1, body.size(), file);
    std::fclose(file);
    if (written == body.size()) {
        std::rename(tmp.c_str(), path.c_str());
    } else {
        std::remove(tmp.c_str());
    }
}

std::string LoadReviewCache(const std::string& task_id) {
    if (task_id.empty()) {
        return "";
    }
    FILE* file = std::fopen(ReviewCachePath(task_id).c_str(), "r");
    if (file == nullptr) {
        return "";
    }
    std::string body;
    char buffer[1024];
    size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        body.append(buffer, read);
    }
    std::fclose(file);
    return body;
}

}  // namespace

EnglishTaskList EnglishClient::FetchTasks(std::string& error) {
    error.clear();
    std::string api_base;
    std::string device_token;
    if (!LoadConfig(api_base, device_token, error)) {
        return {};
    }
    std::string body;
    if (!GetJson("/api/device/v1/english/tasks", api_base, device_token,
                 body, error)) {
        return {};
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        error = "任务列表格式错误";
        return {};
    }
    const cJSON* tasks = cJSON_GetObjectItem(root, "tasks");
    if (!cJSON_IsArray(tasks)) {
        const cJSON* data = cJSON_GetObjectItem(root, "data");
        tasks = cJSON_IsObject(data)
                    ? cJSON_GetObjectItem(data, "tasks")
                    : nullptr;
    }
    if (!cJSON_IsArray(tasks)) {
        cJSON_Delete(root);
        error = "任务列表字段缺失";
        return {};
    }

    EnglishTaskList result;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, tasks) {
        if (!cJSON_IsObject(item)) {
            continue;
        }
        EnglishTask task;
        task.id = JsonString(item, "id", "task_id");
        task.task_date = JsonString(item, "task_date", "date");
        task.status = JsonString(item, "status");
        task.phrase_count =
            JsonInt(item, "phrase_count", "word_count");
        const cJSON* actions = cJSON_GetObjectItem(item, "actions");
        const cJSON* action = nullptr;
        cJSON_ArrayForEach(action, actions) {
            if (cJSON_IsString(action) &&
                std::strcmp(action->valuestring, "review") == 0) {
                task.can_review = true;
                break;
            }
        }
        const char* task_type = JsonString(item, "task_type");
        if (!task.id.empty() && task.can_review &&
            std::strcmp(task_type, "detection") != 0) {
            result.push_back(std::move(task));
        }
    }
    cJSON_Delete(root);
    return result;
}

EnglishReview EnglishClient::FetchReview(const std::string& task_id,
                                         std::string& error) {
    error.clear();
    std::string api_base;
    std::string device_token;
    if (!LoadConfig(api_base, device_token, error)) {
        return {};
    }
    std::string body;
    const std::string path = "/api/device/v1/english/tasks/" +
                             UrlEncode(task_id) + "/review";
    if (!GetJson(path, api_base, device_token, body, error)) {
        body = LoadReviewCache(task_id);
        if (body.empty()) {
            return {};
        }
        error.clear();
    } else {
        SaveReviewCache(task_id, body);
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        error = "复习内容格式错误";
        return {};
    }
    EnglishReview review;
    review.label = JsonString(root, "label");
    const cJSON* entries = cJSON_GetObjectItem(root, "entries");
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, entries) {
        if (!cJSON_IsObject(item)) {
            continue;
        }
        EnglishEntry entry;
        entry.word_id = JsonString(item, "word_id", "id");
        entry.word_id_from_server = !entry.word_id.empty();
        entry.term = JsonString(item, "term");
        entry.phonetic = JsonString(item, "phonetic");
        entry.meaning = JsonString(item, "meaning");
        entry.phrase = JsonString(item, "phrase");
        entry.example = JsonString(item, "example");
        const cJSON* options = cJSON_GetObjectItem(item, "options");
        const cJSON* option = nullptr;
        cJSON_ArrayForEach(option, options) {
            if (cJSON_IsString(option) && option->valuestring != nullptr &&
                option->valuestring[0] != '\0') {
                entry.options.emplace_back(option->valuestring);
            }
        }
        const cJSON* correct_index =
            cJSON_GetObjectItem(item, "correct_index");
        if (!cJSON_IsNumber(correct_index)) {
            correct_index = cJSON_GetObjectItem(item, "answer_index");
        }
        entry.correct_index =
            cJSON_IsNumber(correct_index) ? correct_index->valueint : -1;
        if (entry.correct_index >= 0 && entry.correct_index >=
                                          static_cast<int>(entry.options.size())) {
            entry.correct_index = -1;
        }
        if (entry.correct_index < 0 && !entry.meaning.empty()) {
            for (size_t i = 0; i < entry.options.size(); ++i) {
                if (entry.options[i] == entry.meaning) {
                    entry.correct_index = static_cast<int>(i);
                    break;
                }
            }
        }
        review.entries.push_back(std::move(entry));
    }
    cJSON_Delete(root);
    return review;
}

bool EnglishClient::SubmitResult(const std::string& task_id,
                                 const std::string& word_id,
                                 const std::string& result,
                                 std::string& error) {
    error.clear();
    std::string api_base;
    std::string device_token;
    if (!LoadConfig(api_base, device_token, error)) {
        return false;
    }

    cJSON* payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "word_id", word_id.c_str());
    cJSON_AddStringToObject(payload, "result", result.c_str());
    char* raw = cJSON_PrintUnformatted(payload);
    std::string content = raw != nullptr ? raw : "";
    cJSON_free(raw);
    cJSON_Delete(payload);

    const std::string path = "/api/device/v1/english/tasks/" +
                             UrlEncode(task_id) + "/result";
    std::string body;
    int status = 0;
    if (!HttpRequest("POST", api_base + path, device_token, content, body,
                     status, error)) {
        if (status == 404) {
            const std::string legacy_path =
                "/api/device/v1/english/tasks/" + UrlEncode(task_id) +
                "/review/result";
            if (HttpRequest("POST", api_base + legacy_path, device_token,
                            content, body, status, error)) {
                return true;
            }
        }
        ESP_LOGW(TAG, "submit result failed: %s", error.c_str());
        return false;
    }
    return true;
}

std::string EnglishClient::PronunciationUrl(const std::string& text,
                                            std::string& error) {
    error.clear();
    if (text.empty()) {
        error = "没有可发音的内容";
        return "";
    }
    // type=2 在部分短语上会返回空音频；type=1 更稳定。
    return std::string(kPronunciationBase) + "?audio=" + UrlEncode(text) +
           "&type=1";
}


void EnglishClient::PrefetchTasksAsync() {
    static std::atomic<bool> started{false};
    if (started.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    xTaskCreate(
        [](void* /*arg*/) {
            for (int attempt = 0; attempt < 2; ++attempt) {
                vTaskDelay(pdMS_TO_TICKS(attempt == 0 ? 8000 : 12000));
                std::string error;
                const EnglishTaskList tasks = FetchTasks(error);
                if (!error.empty() || tasks.empty()) {
                    continue;
                }
                const char* tmp = "/sdcard/system/english_tasks.tmp";
                FILE* file = std::fopen(tmp, "w");
                if (file == nullptr) {
                    continue;
                }
                for (const auto& task : tasks) {
                    std::fprintf(file, "%s|%s|%s|%d|%d\n", task.id.c_str(),
                                 task.task_date.c_str(), task.status.c_str(),
                                 task.phrase_count,
                                 task.can_review ? 1 : 0);
                }
                std::fclose(file);
                std::rename(tmp, "/sdcard/system/english_tasks.txt");
                break;
            }
            vTaskDelete(nullptr);
        },
        "english_prefetch", 8192, nullptr, 3, nullptr);
}
