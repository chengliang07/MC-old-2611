#include "oral_screen.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "application.h"
#include "audio_codec.h"
#include "audio_service.h"
#include "board.h"
#include "home_screen/home_screen.h"
#include "pwr_key_handler.h"
#include <web_socket.h>

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr const char* TAG = "OralScreen";
constexpr int32_t kPanelW = 720;
constexpr int32_t kPanelH = 720;
constexpr const char* kRelayUrl =
    "wss://oemsbxavqkmtuyasxsba.supabase.co/functions/v1/oral-coach-relay-probe";
constexpr int kInputSampleRate = 16000;
constexpr int kInputFrameMs = 100;
constexpr int kInputSamples = kInputSampleRate * kInputFrameMs / 1000;
constexpr int kOutputSampleRate = 24000;
constexpr int kMaxTextChars = 800;
constexpr int kMaxTextBytes = kMaxTextChars * 4 + 1;

enum class State { Idle, Connecting, Listening, Answering, Error };

std::atomic<uint32_t> s_session{0};
std::atomic<State> s_state{State::Idle};
std::atomic<bool> s_stop_requested{true};
std::atomic<bool> s_ws_ready{false};
std::atomic<bool> s_mic_enabled{false};
std::atomic<bool> s_response_done_pending{false};
std::atomic<uint32_t> s_generation{0};

lv_obj_t* s_screen = nullptr;
lv_obj_t* s_title = nullptr;
lv_obj_t* s_status = nullptr;
lv_obj_t* s_text = nullptr;
TaskHandle_t s_worker_task = nullptr;
std::unique_ptr<WebSocket> s_ws;
std::mutex s_ws_mutex;
std::mutex s_text_mutex;
std::string s_answer;
int s_resample_position = 0;

bool session_alive(uint32_t session) {
    return session == s_session.load(std::memory_order_acquire) &&
           s_state.load(std::memory_order_acquire) != State::Idle;
}

struct WakeWordGuard {
    AudioService& audio;
    bool disabled = false;
    explicit WakeWordGuard(AudioService& audio_service) : audio(audio_service) {
        if (audio.IsWakeWordRunning()) {
            audio.EnableWakeWordDetection(false);
            disabled = true;
        }
    }
    ~WakeWordGuard() {
        if (disabled) audio.EnableWakeWordDetection(true);
    }
};

void post_ui(uint32_t session, State state, const char* status, const char* append_text) {
    struct UiPayload {
        uint32_t session;
        State state;
        char status[96];
        char append[512];
    };
    if (!session_alive(session)) state = State::Error;
    auto* payload = new UiPayload{};
    payload->session = session;
    payload->state = state;
    std::snprintf(payload->status, sizeof(payload->status), "%s", status ? status : "");
    std::snprintf(payload->append, sizeof(payload->append), "%s", append_text ? append_text : "");
    if (lv_async_call([](void* data) {
            std::unique_ptr<UiPayload> item(static_cast<UiPayload*>(data));
            if (!session_alive(item->session)) return;
            s_state.store(item->state, std::memory_order_release);
            if (s_status != nullptr) lv_label_set_text(s_status, item->status);
            if (item->append[0] != '\0' && s_text != nullptr) {
                std::lock_guard<std::mutex> lock(s_text_mutex);
                s_answer += item->append;
                while (s_answer.size() > kMaxTextBytes) {
                    s_answer.erase(s_answer.begin());
                    while (!s_answer.empty() &&
                           (static_cast<unsigned char>(s_answer.front()) & 0xC0) == 0x80) {
                        s_answer.erase(s_answer.begin());
                    }
                }
                lv_label_set_text(s_text, s_answer.c_str());
            }
        }, payload) != LV_RESULT_OK) {
        delete payload;
    }
}

void close_websocket() {
    std::lock_guard<std::mutex> lock(s_ws_mutex);
    if (s_ws) {
        s_ws->Close();
        s_ws.reset();
    }
    s_ws_ready.store(false, std::memory_order_release);
}

void send_ws_text(const char* text) {
    std::lock_guard<std::mutex> lock(s_ws_mutex);
    if (s_ws && s_ws->IsConnected()) s_ws->Send(text);
}

void send_ws_json(const char* text) {
    send_ws_text(text);
}

void resample_24k_to_16k(const std::vector<int16_t>& input, std::vector<int16_t>& output) {
    output.clear();
    output.reserve(input.size() * 2 / 3 + 2);
    for (size_t i = 0; i < input.size(); ++i) {
        s_resample_position += kInputSampleRate;
        bool emit = s_resample_position >= kOutputSampleRate;
        while (s_resample_position >= kOutputSampleRate) {
            s_resample_position -= kOutputSampleRate;
        }
        if (emit) output.push_back(input[i]);
    }
}

void handle_binary_audio(const char* data, size_t len, uint32_t session) {
    (void)session;
    if (data == nullptr || len < sizeof(int16_t)) return;
    size_t samples = (len / sizeof(int16_t)) & ~static_cast<size_t>(1);
    auto* pcm = reinterpret_cast<const int16_t*>(data);
    std::vector<int16_t> input(pcm, pcm + samples);
    std::vector<int16_t> output;
    resample_24k_to_16k(input, output);
    auto& audio = Application::GetInstance().GetAudioService();
    for (int attempt = 0;
         attempt < 20 &&
         !audio.PushPcmToPlaybackQueue(output.data(), output.size());
         ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void handle_json(const char* data, size_t len, uint32_t session) {
    if (data == nullptr || len == 0) return;
    cJSON* root = cJSON_ParseWithLength(data, len);
    if (root == nullptr) return;
    char type[64] = {0};
    const cJSON* type_item = cJSON_GetObjectItem(root, "type");
    if (cJSON_IsString(type_item) && type_item->valuestring != nullptr) {
        std::snprintf(type, sizeof(type), "%s", type_item->valuestring);
    }

    if (std::strcmp(type, "relay.ready") == 0) {
        s_ws_ready.store(true, std::memory_order_release);
        s_response_done_pending.store(false, std::memory_order_release);
        post_ui(session, State::Listening, "聆听中…", nullptr);
    } else if (std::strcmp(type, "relay.speech_stopped") == 0) {
        s_mic_enabled.store(false, std::memory_order_release);
        post_ui(session, State::Answering, "思考中…", nullptr);
    } else if (std::strcmp(type, "relay.response.start") == 0) {
        s_generation.fetch_add(1, std::memory_order_acq_rel);
        s_response_done_pending.store(false, std::memory_order_release);
        s_mic_enabled.store(false, std::memory_order_release);
        post_ui(session, State::Answering, "回答中…", nullptr);
    } else if (std::strcmp(type, "relay.text.delta") == 0) {
        const cJSON* delta = cJSON_GetObjectItem(root, "delta");
        if (cJSON_IsString(delta) && delta->valuestring != nullptr) {
            post_ui(session, State::Answering, "回答中…", delta->valuestring);
        }
    } else if (std::strcmp(type, "relay.response.done") == 0) {
        if (session_alive(session)) {
            s_mic_enabled.store(false, std::memory_order_release);
            s_response_done_pending.store(true, std::memory_order_release);
            s_generation.fetch_add(1, std::memory_order_acq_rel);
            post_ui(session, State::Answering, "回答完成，正在恢复聆听…", nullptr);
        }
    } else if (std::strcmp(type, "relay.error") == 0) {
        const cJSON* error = cJSON_GetObjectItem(root, "error");
        const char* detail = cJSON_IsString(error) ? error->valuestring : "服务错误";
        char message[256];
        std::snprintf(message, sizeof(message), "错误：%s", detail);
        post_ui(session, State::Error, message, nullptr);
    }
    cJSON_Delete(root);
}

bool connect_websocket(uint32_t session, std::string& error) {
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        error = "网络未就绪";
        return false;
    }
    auto ws = network->CreateWebSocket(1);
    if (!ws) {
        error = "创建 WebSocket 失败";
        return false;
    }
    ws->SetReceiveBufferSize(32768);
    ws->OnData([session](const char* data, size_t len, bool binary) {
        if (!session_alive(session)) return;
        if (binary) handle_binary_audio(data, len, session);
        else handle_json(data, len, session);
    });
    ws->OnDisconnected([]() {
        s_ws_ready.store(false, std::memory_order_release);
        s_stop_requested.store(true, std::memory_order_release);
    });
    ws->OnError([session](int code) {
        char message[128];
        std::snprintf(message, sizeof(message), "WebSocket 错误：%d", code);
        post_ui(session, State::Error, message, nullptr);
    });
    if (!ws->Connect(kRelayUrl)) {
        error = "连接口语服务失败";
        return false;
    }
    std::lock_guard<std::mutex> lock(s_ws_mutex);
    s_ws = std::move(ws);
    return true;
}

void stream_microphone(uint32_t session) {
    auto& audio = Application::GetInstance().GetAudioService();
    auto* codec = Board::GetInstance().GetAudioCodec();
    std::vector<int16_t> frame;
    frame.reserve(kInputSamples * 2);
    TickType_t playback_idle_tick = 0;

    while (!s_stop_requested.load(std::memory_order_acquire) && session_alive(session)) {
        if (s_response_done_pending.load(std::memory_order_acquire)) {
            if (audio.IsPlaybackPending()) {
                playback_idle_tick = 0;
            } else {
                const TickType_t now = xTaskGetTickCount();
                if (playback_idle_tick == 0) {
                    playback_idle_tick = now;
                } else if (now - playback_idle_tick >= pdMS_TO_TICKS(250)) {
                    s_response_done_pending.store(false, std::memory_order_release);
                    s_mic_enabled.store(true, std::memory_order_release);
                    post_ui(session, State::Listening, "聆听中…", nullptr);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (!s_ws_ready.load(std::memory_order_acquire) ||
            !s_mic_enabled.load(std::memory_order_acquire)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (!audio.ReadAudioData(frame, kInputSampleRate, kInputSamples)) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (codec != nullptr && codec->input_channels() == 2) {
            for (size_t i = 0; i + 1 < frame.size(); i += 2) frame[i / 2] = frame[i];
            frame.resize(frame.size() / 2);
        }
        std::lock_guard<std::mutex> lock(s_ws_mutex);
        if (!s_ws || !s_ws->IsConnected()) break;
        if (!s_ws->Send(frame.data(), frame.size() * sizeof(int16_t), true)) break;
    }
}

void worker_task(void* /*arg*/) {
    const uint32_t session = s_session.load(std::memory_order_acquire);
    auto& audio = Application::GetInstance().GetAudioService();
    WakeWordGuard wake_guard(audio);
    s_generation.fetch_add(1, std::memory_order_acq_rel);
    s_resample_position = 0;

    if (!audio.IsStarted()) {
        post_ui(session, State::Error, "音频未就绪", nullptr);
        s_worker_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    post_ui(session, State::Connecting, "连接中…", nullptr);
    std::string error;
    if (!connect_websocket(session, error)) {
        post_ui(session, State::Error, error.c_str(), nullptr);
        s_worker_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    send_ws_json("{\"type\":\"start\"}");
    for (int i = 0; i < 100 && session_alive(session) &&
         !s_ws_ready.load(std::memory_order_acquire); ++i) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!s_ws_ready.load(std::memory_order_acquire)) {
        post_ui(session, State::Error, "等待口语服务就绪超时", nullptr);
        close_websocket();
        s_worker_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    s_mic_enabled.store(true, std::memory_order_release);
    stream_microphone(session);
    s_mic_enabled.store(false, std::memory_order_release);
    close_websocket();
    s_worker_task = nullptr;
    vTaskDelete(nullptr);
}

void start_session() {
    if (s_worker_task != nullptr) return;
    s_session.fetch_add(1, std::memory_order_acq_rel);
    s_stop_requested.store(false, std::memory_order_release);
    s_ws_ready.store(false, std::memory_order_release);
    s_response_done_pending.store(false, std::memory_order_release);
    s_mic_enabled.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(s_text_mutex);
        s_answer.clear();
        s_resample_position = 0;
    }
    if (s_text != nullptr) lv_label_set_text(s_text, "");
    if (xTaskCreate(worker_task, "oral_ws", 4096, nullptr, 4, &s_worker_task) != pdPASS) {
        s_worker_task = nullptr;
        post_ui(s_session.load(), State::Error, "启动口语会话失败", nullptr);
    }
}

void stop_session() {
    s_stop_requested.store(true, std::memory_order_release);
    s_response_done_pending.store(false, std::memory_order_release);
    s_mic_enabled.store(false, std::memory_order_release);
    auto& audio = Application::GetInstance().GetAudioService();
    audio.ClearPlaybackQueue();
    send_ws_json("{\"type\":\"interrupt\"}");
    close_websocket();
}

void on_back(lv_event_t* /*event*/) {
    lv_obj_t* home = HomeScreen::Create();
    if (home != nullptr) lv_screen_load(home);
}

void on_screen_unloaded(lv_event_t* /*event*/) {
    s_session.fetch_add(1, std::memory_order_acq_rel);
    stop_session();
    s_screen = nullptr;
    s_title = nullptr;
    s_status = nullptr;
    s_text = nullptr;
}

lv_obj_t* create_label(lv_obj_t* parent, const char* text, const lv_font_t* font, lv_color_t color) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, &font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    return label;
}

}  // namespace

lv_obj_t* OralScreen::Create() {
    PwrKey_SetShortPressHandler("oral", Interrupt);
    s_session.fetch_add(1, std::memory_order_acq_rel);
    s_state.store(State::Connecting, std::memory_order_release);
    s_stop_requested.store(false, std::memory_order_release);

    lv_obj_t* screen = lv_obj_create(nullptr);
    s_screen = screen;
    screen_strip_obj_chrome(screen);
    lv_obj_set_size(screen, kPanelW, kPanelH);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0F172A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* header = lv_obj_create(screen);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, lv_pct(100), 64);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(header, 12, LV_PART_MAIN);

    lv_obj_t* back = lv_button_create(header);
    lv_obj_set_size(back, 52, 40);
    lv_obj_set_style_radius(back, 12, LV_PART_MAIN);
    lv_obj_t* back_label = lv_label_create(back);
    lv_label_set_text(back_label, "←");
    lv_obj_center(back_label);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, nullptr);

    s_title = create_label(header, "一丢口语", font_puhui_30_4, lv_color_hex(0xFFFFFF));
    lv_obj_set_flex_grow(s_title, 1);
    lv_obj_set_style_text_align(s_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    s_status = create_label(screen, "准备中…", font_puhui_20_4, lv_color_hex(0x93C5FD));
    lv_obj_set_pos(s_status, 24, 76);
    s_text = create_label(screen, "", font_puhui_20_4, lv_color_hex(0xF8FAFC));
    lv_obj_set_pos(s_text, 24, 116);
    lv_obj_set_size(s_text, 672, 280);

    lv_obj_add_event_cb(screen, on_screen_unloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);
    start_session();
    return screen;
}

void OralScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: oral_screen");
        Application::GetInstance().SetVoiceUiDesired(false);
        return;
    }
    ESP_LOGI(TAG, "unload: oral_screen");
    stop_session();
}

bool OralScreen::IsActive() {
    return s_screen != nullptr && lv_screen_active() == s_screen;
}

void OralScreen::Interrupt() {
    if (!IsActive()) return;
    auto& audio = Application::GetInstance().GetAudioService();
    audio.ClearPlaybackQueue();
    audio.AbortPlaybackGeneration();
    s_generation.fetch_add(1, std::memory_order_acq_rel);
    send_ws_json("{\"type\":\"interrupt\"}");
    s_mic_enabled.store(false, std::memory_order_release);
    vTaskDelay(pdMS_TO_TICKS(150));
    s_mic_enabled.store(true, std::memory_order_release);
    post_ui(s_session.load(), State::Listening, "聆听中…", nullptr);
}
