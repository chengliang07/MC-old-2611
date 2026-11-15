#include "pet_client.h"

#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#include "board.h"
#include "cJSON.h"
#include "esp_log.h"
#include "http.h"
#include "settings.h"

namespace {

constexpr const char* TAG = "PetClient";
constexpr const char* kSettingsNs = "podcast";
constexpr const char* kKeyDeviceToken = "device_token";
constexpr const char* kApiBase = "https://www.yi-edu.com";
constexpr int kHttpTimeoutMs = 15000;

std::mutex s_http_mutex;

const char* JsonString(const cJSON* object, const char* key) {
    const cJSON* item = cJSON_GetObjectItem(object, key);
    return cJSON_IsString(item) ? item->valuestring : "";
}

int JsonInt(const cJSON* object, const char* key, int fallback = 0) {
    const cJSON* item = cJSON_GetObjectItem(object, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

bool JsonBool(const cJSON* object, const char* key, bool fallback = false) {
    const cJSON* item = cJSON_GetObjectItem(object, key);
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    return fallback;
}

bool LoadDeviceToken(std::string& token, std::string& error) {
    Settings settings(kSettingsNs, false);
    token = settings.GetString(kKeyDeviceToken, CONFIG_PODCAST_DEVICE_TOKEN);
    if (token.empty()) {
        error = "设备未绑定\n请联系管理员配置";
        return false;
    }
    return true;
}

bool ParsePet(const cJSON* root, PetState& state) {
    const cJSON* pet = cJSON_GetObjectItem(root, "pet");
    if (!cJSON_IsObject(pet)) {
        return false;
    }

    state.name = JsonString(pet, "name");
    state.food_balance = JsonInt(pet, "food_balance");
    state.max_food = JsonInt(pet, "max_food", 99);
    state.satiety = JsonInt(pet, "satiety", 70);
    state.mood = JsonString(pet, "mood");
    state.level = JsonInt(pet, "level", 1);
    state.xp = JsonInt(pet, "xp");
    state.next_level_xp = JsonInt(pet, "next_level_xp");
    state.xp_to_next = JsonInt(pet, "xp_to_next");
    state.stage = JsonString(pet, "stage");
    state.hatched = JsonBool(pet, "hatched");
    state.dormant = JsonBool(pet, "dormant");
    state.hatch_food_required = JsonInt(pet, "hatch_food_required", 3);
    state.today_earned = JsonInt(pet, "today_earned");
    state.daily_cap = JsonInt(pet, "daily_cap", 20);
    state.reward_remembered = JsonInt(pet, "reward_remembered", 2);
    state.reward_forgotten = JsonInt(pet, "reward_forgotten", 1);
    if (state.stage.empty()) {
        state.stage = state.hatched ? "chick" : "egg";
    }
    return true;
}

bool HttpRequest(const char* method, const std::string& path,
                 const std::string& token, std::string content,
                 std::string& body, int& status, std::string& error,
                 std::string& code) {
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

    const std::string url = std::string(kApiBase) + path;
    http->SetTimeout(kHttpTimeoutMs);
    http->SetHeader("Accept", "application/json");
    http->SetHeader("Connection", "close");
    http->SetHeader("Authorization", "Bearer " + token);
    if (!content.empty()) {
        http->SetHeader("Content-Type", "application/json");
        http->SetContent(std::move(content));
    }
    if (!http->Open(method, url)) {
        error = "连接宠物服务失败";
        return false;
    }

    status = http->GetStatusCode();
    body = http->ReadAll();
    http->Close();

    if (status >= 200 && status < 300) {
        return true;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (root != nullptr) {
        code = JsonString(root, "code");
        error = JsonString(root, "error");
        cJSON_Delete(root);
    }
    if (error.empty()) {
        error = "宠物服务返回 HTTP " + std::to_string(status);
    }
    return false;
}

std::string EmptyJsonObject() {
    cJSON* payload = cJSON_CreateObject();
    char* raw = cJSON_PrintUnformatted(payload);
    std::string content = raw != nullptr ? raw : "{}";
    cJSON_free(raw);
    cJSON_Delete(payload);
    return content;
}

bool RequestPet(const char* method, const std::string& path,
                PetState& state, std::string& error, std::string& code) {
    error.clear();
    code.clear();

    std::string token;
    if (!LoadDeviceToken(token, error)) {
        return false;
    }

    std::string body;
    int status = 0;
    const std::string content =
        std::strcmp(method, "POST") == 0 ? EmptyJsonObject() : "";
    if (!HttpRequest(method, path, token, content, body, status, error, code)) {
        ESP_LOGW(TAG, "pet request failed: %s", error.c_str());
        return false;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        error = "宠物数据格式错误";
        return false;
    }
    const bool ok = ParsePet(root, state);
    cJSON_Delete(root);
    if (!ok) {
        error = "宠物数据字段缺失";
        return false;
    }
    return true;
}

}  // namespace

bool PetClient::Fetch(PetState& state, std::string& error) {
    std::string code;
    return RequestPet("GET", "/api/device/v1/pet", state, error, code);
}

bool PetClient::Feed(PetState& state, std::string& error, std::string& code) {
    return RequestPet("POST", "/api/device/v1/pet/feed", state, error, code);
}

bool PetClient::Hatch(PetState& state, std::string& error, std::string& code) {
    return RequestPet("POST", "/api/device/v1/pet/hatch", state, error, code);
}
