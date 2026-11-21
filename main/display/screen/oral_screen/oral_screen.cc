#include "oral_screen.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <deque>
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
#include "chat_screen/chat_screen.h"
#include "pwr_key_handler.h"
#include "settings.h"
#include <web_socket.h>

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr const char* TAG = "OralScreen";
constexpr int32_t kPanelW = 720;
constexpr int32_t kPanelH = 720;
constexpr int32_t kHeaderH = 88;
constexpr int32_t kBackBtnSize = 72;
constexpr int32_t kHeaderRightPad = 16;
constexpr int32_t kListPadH = 18;
constexpr int32_t kListPadTop = 102;
constexpr int32_t kListPadBottom = 32;
constexpr int32_t kRowGap = 12;
constexpr int32_t kBubblePadX = 18;
constexpr int32_t kBubblePadY = 14;
constexpr int32_t kBubbleRadius = 18;
constexpr int32_t kSideMargin = 8;
constexpr int32_t kMaxMessages = 12;
constexpr const char* kRelayUrl =
    "wss://oemsbxavqkmtuyasxsba.supabase.co/functions/v1/oral-coach-relay-probe";
constexpr int kInputSampleRate = 16000;
constexpr int kInputFrameMs = 100;
constexpr int kInputSamples = kInputSampleRate * kInputFrameMs / 1000;
constexpr int kOutputSampleRate = 24000;
constexpr int kMaxTextChars = 800;
constexpr int kMaxTextBytes = kMaxTextChars * 4 + 1;
constexpr size_t kMaxAudioChunks = 16;
constexpr size_t kMaxAudioBytes = 192 * 1024;
constexpr uint32_t kColorBg = 0x0E1116;
constexpr uint32_t kColorHeaderBg = 0x12151C;
constexpr uint32_t kColorDivider = 0x2A2F3A;
constexpr uint32_t kColorHeaderText = 0xFFFFFF;
constexpr uint32_t kColorLeftBubble = 0x202736;
constexpr uint32_t kColorRightBubble = 0x1E3A2F;
constexpr uint32_t kColorLeftBubbleText = 0xE5E7EB;
constexpr uint32_t kColorRightBubbleText = 0xE8F5E9;
constexpr uint32_t kColorStatusListening = 0x34D399;
constexpr uint32_t kColorStatusAnswering = 0x60A5FA;
constexpr uint32_t kColorStatusConnecting = 0xFBBF24;
constexpr uint32_t kColorStatusError = 0xF87171;

enum class State { Idle, Connecting, Listening, Answering, Error };
enum class BubbleRole { User, Assistant };

std::atomic<uint32_t> s_session{0};
std::atomic<State> s_state{State::Idle};
std::atomic<bool> s_stop_requested{true};
std::atomic<bool> s_ws_ready{false};
std::atomic<bool> s_mic_enabled{false};
std::atomic<bool> s_interrupt_requested{false};
std::atomic<bool> s_drop_incoming_audio{false};
std::atomic<bool> s_barge_detection_requested{false};
std::atomic<bool> s_reconnect_requested{false};
std::atomic<bool> s_user_exit{false};
std::atomic<bool> s_response_done_pending{false};
std::atomic<int64_t> s_response_done_at_us{0};
std::atomic<uint32_t> s_generation{0};
std::atomic<uint32_t> s_last_done_response_id{0};
std::atomic<bool> s_worker_running{false};
std::atomic<int> s_start_retries{0};

lv_obj_t* s_screen = nullptr;
lv_obj_t* s_title = nullptr;
lv_obj_t* s_status = nullptr;
lv_obj_t* s_text = nullptr;
lv_obj_t* s_text_bubble = nullptr;
lv_obj_t* s_assistant_row = nullptr;
lv_obj_t* s_user_text_label = nullptr;
lv_obj_t* s_user_text_bubble = nullptr;
lv_obj_t* s_user_row = nullptr;
lv_obj_t* s_chat = nullptr;
lv_obj_t* s_return_screen = nullptr;
TaskHandle_t s_worker_task = nullptr;
std::unique_ptr<WebSocket> s_ws;
std::mutex s_ws_mutex;
std::mutex s_text_mutex;
std::string s_answer;
std::string s_user_text;
std::string s_stream_text;
uint32_t s_stream_text_ms = 0;
int s_resample_position = 0;

struct AudioChunk {
    std::vector<int16_t> pcm;
    uint32_t generation;
};
std::deque<AudioChunk> s_audio_chunks;
size_t s_audio_bytes = 0;
std::mutex s_audio_mutex;
std::atomic<bool> s_audio_task_running{false};

bool session_alive(uint32_t session) {
    return session == s_session.load(std::memory_order_acquire) &&
           s_state.load(std::memory_order_acquire) != State::Idle;
}

uint32_t status_color(State state) {
    switch (state) {
        case State::Listening:
            return kColorStatusListening;
        case State::Connecting:
            return kColorStatusConnecting;
        case State::Error:
            return kColorStatusError;
        case State::Answering:
            return kColorStatusAnswering;
        default:
            return kColorStatusConnecting;
    }
}

void trim_messages() {
    if (s_chat == nullptr) return;
    while (lv_obj_get_child_count(s_chat) > kMaxMessages) {
        lv_obj_t* oldest = lv_obj_get_child(s_chat, 0);
        if (oldest == nullptr) break;
        if (oldest == s_assistant_row) {
            s_assistant_row = nullptr;
            s_text = nullptr;
            s_text_bubble = nullptr;
        }
        if (oldest == s_user_row) {
            s_user_row = nullptr;
            s_user_text_label = nullptr;
            s_user_text_bubble = nullptr;
        }
        lv_obj_delete(oldest);
    }
}

int32_t bubble_width_for_text(const char* text, bool is_right) {
    const lv_font_t* font = &font_puhui_20_4;
    const int32_t max_bubble_w = kPanelW * (is_right ? 72 : 90) / 100;
    int32_t text_w = lv_txt_get_width(text, std::strlen(text), font, 0);
    if (text_w < 24) text_w = 24;
    int32_t bubble_w = text_w + kBubblePadX * 2;
    if (bubble_w > max_bubble_w) bubble_w = max_bubble_w;
    return bubble_w;
}

void resize_bubble(lv_obj_t* bubble, lv_obj_t* label, const char* text, bool is_right) {
    if (bubble == nullptr || label == nullptr) return;
    const int32_t width = bubble_width_for_text(text, is_right);
    lv_obj_set_width(bubble, width);
    lv_obj_set_width(label, width - kBubblePadX * 2);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
}

uint32_t now_ms() {
    return static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

void post_ui(uint32_t session, State state, const char* status,
             BubbleRole role, const char* text, bool append_user = false,
             bool reset_assistant = false);

void flush_stream_text(uint32_t session) {
    std::string text;
    {
        std::lock_guard<std::mutex> lock(s_text_mutex);
        if (s_stream_text.empty()) return;
        text.swap(s_stream_text);
        s_stream_text_ms = 0;
    }
    post_ui(session, State::Answering, "回答中…", BubbleRole::Assistant,
            text.c_str(), false);
}

struct WakeWordGuard {
    AudioService& audio;
    explicit WakeWordGuard(AudioService& audio_service) : audio(audio_service) {
        if (audio.IsWakeWordRunning()) {
            audio.EnableWakeWordDetection(false);
        }
    }
};

void start_barge_in_detection() {
    auto& audio = Application::GetInstance().GetAudioService();
    audio.EnableWakeWordDetection(true);
    audio.SetBargeInVad(true, nullptr, false);
}

void stop_barge_in_detection() {
    auto& audio = Application::GetInstance().GetAudioService();
    audio.SetBargeInVad(false, nullptr, false);
    audio.EnableWakeWordDetection(false);
}

void post_ui(uint32_t session, State state, const char* status,
             BubbleRole role, const char* text, bool append_user,
             bool reset_assistant) {
    struct UiPayload {
        uint32_t session;
        State state;
        char status[96];
        char append[512];
        BubbleRole role;
        bool replace_text;
        bool append_user;
        uint32_t generation;
        bool reset_assistant;
    };
    if (!session_alive(session)) state = State::Error;
    auto* payload = new UiPayload{};
    payload->session = session;
    payload->state = state;
    std::snprintf(payload->status, sizeof(payload->status), "%s", status ? status : "");
    std::snprintf(payload->append, sizeof(payload->append), "%s", text ? text : "");
    payload->role = role;
    payload->replace_text = false;
    payload->append_user = append_user;
    payload->generation = s_generation.load(std::memory_order_acquire);
    payload->reset_assistant = reset_assistant;
    if (lv_async_call([](void* data) {
            std::unique_ptr<UiPayload> item(static_cast<UiPayload*>(data));
            if (!session_alive(item->session)) return;
            s_state.store(item->state, std::memory_order_release);
            if (s_status != nullptr) {
                lv_label_set_text(s_status, item->status);
                lv_obj_set_style_text_color(s_status, lv_color_hex(status_color(item->state)), LV_PART_MAIN);
            }
            if (item->append[0] == '\0' || s_chat == nullptr ||
                item->generation != s_generation.load(std::memory_order_acquire)) return;
            if (item->reset_assistant) {
                s_text = nullptr;
                s_text_bubble = nullptr;
                s_assistant_row = nullptr;
                std::lock_guard<std::mutex> lock(s_text_mutex);
                s_answer.clear();
            }
            if (item->append_user && item->role == BubbleRole::User && s_user_text_label != nullptr) {
                std::lock_guard<std::mutex> lock(s_text_mutex);
                s_user_text += item->append;
                lv_label_set_text(s_user_text_label, s_user_text.c_str());
                if (s_user_text.size() >= 24) {
                    resize_bubble(s_user_text_bubble, s_user_text_label, s_user_text.c_str(), true);
                }
                return;
            }
            if (!item->replace_text && item->role == BubbleRole::Assistant &&
                s_text != nullptr) {
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
                if (s_answer.size() >= 24) {
                    resize_bubble(s_text_bubble, s_text, s_answer.c_str(), false);
                }
                return;
            }
            bool is_user = item->role == BubbleRole::User;
            lv_obj_t* row = lv_obj_create(s_chat);
            screen_strip_obj_chrome(row);
            lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_set_style_pad_bottom(row, kRowGap, LV_PART_MAIN);
            lv_obj_set_width(row, LV_PCT(100));
            lv_obj_set_height(row, LV_SIZE_CONTENT);

            lv_obj_t* bubble = lv_obj_create(row);
            screen_strip_obj_chrome(bubble);
            lv_obj_set_height(bubble, LV_SIZE_CONTENT);
            lv_obj_set_style_radius(bubble, kBubbleRadius, LV_PART_MAIN);
            lv_obj_set_style_border_width(bubble, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_hor(bubble, kBubblePadX, LV_PART_MAIN);
            lv_obj_set_style_pad_ver(bubble, kBubblePadY, LV_PART_MAIN);
            lv_obj_set_style_shadow_width(bubble, 0, LV_PART_MAIN);
            lv_obj_remove_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_bg_color(bubble, lv_color_hex(is_user ? kColorRightBubble : kColorLeftBubble), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, LV_PART_MAIN);
            const int32_t width = bubble_width_for_text(item->append, is_user);
            lv_obj_set_width(bubble, width);
            if (is_user) lv_obj_align(bubble, LV_ALIGN_TOP_RIGHT, -kSideMargin, 0);
            else lv_obj_align(bubble, LV_ALIGN_TOP_LEFT, kSideMargin, 0);

            lv_obj_t* label = lv_label_create(bubble);
            lv_label_set_text(label, item->append);
            lv_obj_set_width(label, width - kBubblePadX * 2);
            lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_font(label, &font_puhui_20_4, LV_PART_MAIN);
            lv_obj_set_style_text_color(label, lv_color_hex(is_user ? kColorRightBubbleText : kColorLeftBubbleText), LV_PART_MAIN);
            screen_make_input_passive(row);
            if (!is_user) {
                s_text = label;
                s_text_bubble = bubble;
                s_assistant_row = row;
                std::lock_guard<std::mutex> lock(s_text_mutex);
                s_answer = item->append;
            } else {
                s_user_text_label = label;
                s_user_text_bubble = bubble;
                s_user_row = row;
                std::lock_guard<std::mutex> lock(s_text_mutex);
                s_user_text = item->append;
            }
        }, payload) != LV_RESULT_OK) {
        delete payload;
    }
}

void clear_audio_queue() {
    std::lock_guard<std::mutex> lock(s_audio_mutex);
    s_audio_chunks.clear();
    s_audio_bytes = 0;
}

bool has_queued_audio() {
    std::lock_guard<std::mutex> lock(s_audio_mutex);
    return !s_audio_chunks.empty();
}

void audio_playback_task(void* /*arg*/) {
    while (true) {
        AudioChunk chunk;
        {
            std::lock_guard<std::mutex> lock(s_audio_mutex);
            if (s_audio_chunks.empty()) {
                chunk.pcm.clear();
            } else {
                chunk = std::move(s_audio_chunks.front());
                s_audio_chunks.pop_front();
                s_audio_bytes -= chunk.pcm.size() * sizeof(int16_t);
            }
        }
        if (chunk.pcm.empty()) {
            vTaskDelay(pdMS_TO_TICKS(120));
            {
                std::lock_guard<std::mutex> lock(s_audio_mutex);
                if (!s_audio_chunks.empty()) continue;
                s_audio_task_running.store(false, std::memory_order_release);
            }
            vTaskDelete(nullptr);
            return;
        }
        if (chunk.generation != s_generation.load(std::memory_order_acquire)) continue;
        auto& audio = Application::GetInstance().GetAudioService();
        for (int attempt = 0;
             attempt < 20 && !audio.PushPcmToPlaybackQueue(
                                  chunk.pcm.data(), chunk.pcm.size());
             ++attempt) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

void queue_audio_output(const std::vector<int16_t>& output) {
    if (output.empty()) return;
    {
        std::lock_guard<std::mutex> lock(s_audio_mutex);
        size_t bytes = output.size() * sizeof(int16_t);
        while (s_audio_chunks.size() >= kMaxAudioChunks ||
               s_audio_bytes + bytes > kMaxAudioBytes) {
            s_audio_bytes -= s_audio_chunks.front().pcm.size() * sizeof(int16_t);
            s_audio_chunks.pop_front();
        }
        s_audio_chunks.push_back(AudioChunk{output, s_generation.load(std::memory_order_acquire)});
        s_audio_bytes += bytes;
    }
    if (!s_audio_task_running.load(std::memory_order_acquire)) {
        if (xTaskCreate(audio_playback_task, "oral_audio", 3584, nullptr, 3, nullptr) == pdPASS) {
            s_audio_task_running.store(true, std::memory_order_release);
        } else {
            clear_audio_queue();
        }
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

void start_session();

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
    if (!session_alive(session)) return;
    if (s_drop_incoming_audio.load(std::memory_order_acquire)) return;
    if (data == nullptr || len < sizeof(int16_t)) return;
    size_t samples = (len / sizeof(int16_t)) & ~static_cast<size_t>(1);
    auto* pcm = reinterpret_cast<const int16_t*>(data);
    std::vector<int16_t> input(pcm, pcm + samples);
    std::vector<int16_t> output;
    resample_24k_to_16k(input, output);
    queue_audio_output(output);
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
        s_drop_incoming_audio.store(false, std::memory_order_release);
        s_response_done_pending.store(false, std::memory_order_release);
        post_ui(session, State::Listening, "聆听中…", BubbleRole::Assistant, nullptr);
    } else if (std::strcmp(type, "relay.speech_stopped") == 0) {
        s_mic_enabled.store(false, std::memory_order_release);
        post_ui(session, State::Answering, "思考中…", BubbleRole::Assistant, nullptr);
    } else if (std::strcmp(type, "relay.user_transcript.delta") == 0) {
        const cJSON* delta = cJSON_GetObjectItem(root, "delta");
        if (cJSON_IsString(delta) && delta->valuestring != nullptr) {
            post_ui(session, State::Listening, "聆听中…", BubbleRole::User, delta->valuestring, true);
        }
    } else if (std::strcmp(type, "relay.user_transcript.done") == 0) {
        const cJSON* text = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(text) && text->valuestring != nullptr && text->valuestring[0] != '\0') {
            post_ui(session, State::Answering, "思考中…", BubbleRole::User, text->valuestring);
        }
    } else if (std::strcmp(type, "relay.response.start") == 0) {
        s_barge_detection_requested.store(true, std::memory_order_release);
        s_generation.fetch_add(1, std::memory_order_acq_rel);
        s_drop_incoming_audio.store(false, std::memory_order_release);
        s_response_done_pending.store(false, std::memory_order_release);
        s_mic_enabled.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(s_text_mutex);
            s_stream_text.clear();
            s_stream_text_ms = 0;
        }
        post_ui(session, State::Answering, "回答中…", BubbleRole::Assistant,
                nullptr, false, true);
    } else if (std::strcmp(type, "relay.text.delta") == 0) {
        const cJSON* delta = cJSON_GetObjectItem(root, "delta");
        if (cJSON_IsString(delta) && delta->valuestring != nullptr) {
            {
                std::lock_guard<std::mutex> lock(s_text_mutex);
                s_stream_text += delta->valuestring;
                if (s_stream_text.size() > kMaxTextBytes) {
                    s_stream_text.erase(0, s_stream_text.size() - kMaxTextBytes);
                }
                if (s_stream_text_ms == 0) s_stream_text_ms = now_ms();
            }
            if (s_stream_text.size() >= 48 || now_ms() - s_stream_text_ms >= 220) {
                flush_stream_text(session);
            }
        }
    } else if (std::strcmp(type, "relay.response.done") == 0) {
        if (session_alive(session)) {
            uint32_t response_id = 0;
            const cJSON* response_item = cJSON_GetObjectItem(root, "responseId");
            if (cJSON_IsNumber(response_item)) response_id = static_cast<uint32_t>(response_item->valuedouble);
            if (response_id != 0 && response_id == s_last_done_response_id.load(std::memory_order_acquire)) {
                cJSON_Delete(root);
                return;
            }
            if (response_id != 0) s_last_done_response_id.store(response_id, std::memory_order_release);
            flush_stream_text(session);
            s_mic_enabled.store(false, std::memory_order_release);
            s_response_done_pending.store(true, std::memory_order_release);
            s_response_done_at_us.store(esp_timer_get_time(),
                                        std::memory_order_release);
            post_ui(session, State::Answering, "回答完成，正在恢复聆听…", BubbleRole::Assistant, nullptr);
        }
    } else if (std::strcmp(type, "relay.interrupted") == 0) {
        s_drop_incoming_audio.store(true, std::memory_order_release);
        s_response_done_pending.store(false, std::memory_order_release);
        s_mic_enabled.store(true, std::memory_order_release);
        post_ui(session, State::Listening, "聆听中…", BubbleRole::Assistant, nullptr);
    } else if (std::strcmp(type, "relay.error") == 0) {
        const cJSON* error = cJSON_GetObjectItem(root, "error");
        const char* detail = cJSON_IsString(error) ? error->valuestring : "服务错误";
        char message[256];
        std::snprintf(message, sizeof(message), "错误：%s", detail);
        post_ui(session, State::Error, message, BubbleRole::Assistant, nullptr);
    }
    cJSON_Delete(root);
}

bool connect_websocket(uint32_t session, std::string& error) {
    Settings settings("oral", false);
    const std::string relay_url = settings.GetString("relay_url", kRelayUrl);
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
        if (!s_user_exit.load(std::memory_order_acquire)) {
            s_reconnect_requested.store(true, std::memory_order_release);
        }
    });
    ws->OnError([session](int code) {
        char message[128];
        std::snprintf(message, sizeof(message), "WebSocket 错误：%d", code);
        post_ui(session, State::Error, message, BubbleRole::Assistant, nullptr);
    });
    if (!ws->Connect(relay_url.c_str())) {
        char message[128];
        std::snprintf(message, sizeof(message), "连接口语服务失败：%d", ws->GetLastError());
        error = message;
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

    while (!s_stop_requested.load(std::memory_order_acquire) &&
           !s_reconnect_requested.load(std::memory_order_acquire) &&
           session_alive(session)) {
        if (s_barge_detection_requested.exchange(false,
                                                std::memory_order_acq_rel)) {
            start_barge_in_detection();
        }
        if (s_interrupt_requested.exchange(false, std::memory_order_acq_rel)) {
            auto& audio = Application::GetInstance().GetAudioService();
            stop_barge_in_detection();
            s_drop_incoming_audio.store(true, std::memory_order_release);
            audio.ClearPlaybackQueue();
            clear_audio_queue();
            s_generation.fetch_add(1, std::memory_order_acq_rel);
            s_response_done_pending.store(false, std::memory_order_release);
            send_ws_json("{\"type\":\"interrupt\"}");
            send_ws_json("{\"type\":\"resume_mic\"}");
            s_mic_enabled.store(true, std::memory_order_release);
            post_ui(session, State::Listening, "聆听中…", BubbleRole::Assistant, nullptr);
        }
        if (s_response_done_pending.load(std::memory_order_acquire)) {
            if (audio.IsPlaybackPending() || has_queued_audio()) {
                playback_idle_tick = 0;
            } else {
                const TickType_t now = xTaskGetTickCount();
                const int64_t done_at_us =
                    s_response_done_at_us.load(std::memory_order_acquire);
                if (playback_idle_tick == 0) {
                    playback_idle_tick = now;
                }
                if (esp_timer_get_time() - done_at_us >= 8000LL * 1000) {
                    stop_barge_in_detection();
                    s_response_done_pending.store(false, std::memory_order_release);
                    s_mic_enabled.store(true, std::memory_order_release);
                    send_ws_json("{\"type\":\"resume_mic\"}");
                    post_ui(session, State::Listening,
                            "聆听中…", BubbleRole::Assistant, nullptr);
                } else if (now - playback_idle_tick >= pdMS_TO_TICKS(250)) {
                    stop_barge_in_detection();
                    s_response_done_pending.store(false, std::memory_order_release);
                    s_mic_enabled.store(true, std::memory_order_release);
                    send_ws_json("{\"type\":\"resume_mic\"}");
                    post_ui(session, State::Listening, "聆听中…", BubbleRole::Assistant, nullptr);
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
        post_ui(session, State::Error, "音频未就绪", BubbleRole::Assistant, nullptr);
        s_worker_task = nullptr;
        s_worker_running.store(false, std::memory_order_release);
        vTaskDelete(nullptr);
        return;
    }

    int connect_failures = 0;
    while (!s_stop_requested.load(std::memory_order_acquire) &&
           session_alive(session)) {
        if (s_stop_requested.load(std::memory_order_acquire)) {
            break;
        }
        post_ui(session, State::Connecting, "连接中…", BubbleRole::Assistant, nullptr);
        std::string error;
        if (!connect_websocket(session, error)) {
            if (++connect_failures >= 3) {
                post_ui(session, State::Error, error.c_str(), BubbleRole::Assistant, nullptr);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        connect_failures = 0;
        send_ws_json("{\"type\":\"start\"}");
        for (int i = 0; i < 100 && session_alive(session) &&
             !s_ws_ready.load(std::memory_order_acquire); ++i) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (!s_ws_ready.load(std::memory_order_acquire)) {
            close_websocket();
            if (++connect_failures >= 3) {
                post_ui(session, State::Error, "等待口语服务就绪超时", BubbleRole::Assistant, nullptr);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        s_mic_enabled.store(true, std::memory_order_release);
        stream_microphone(session);
        s_mic_enabled.store(false, std::memory_order_release);
        close_websocket();
        if (s_reconnect_requested.load(std::memory_order_acquire)) {
            s_reconnect_requested.store(false, std::memory_order_release);
            stop_barge_in_detection();
            s_response_done_pending.store(false, std::memory_order_release);
            audio.ClearPlaybackQueue();
            clear_audio_queue();
            s_generation.fetch_add(1, std::memory_order_acq_rel);
            post_ui(session, State::Connecting, "连接中断，正在重连…", BubbleRole::Assistant, nullptr);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        break;
    }
    s_worker_task = nullptr;
    s_worker_running.store(false, std::memory_order_release);
    stop_barge_in_detection();
    clear_audio_queue();
    audio.ClearPlaybackQueue();
    close_websocket();
    s_interrupt_requested.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}

void start_session() {
    if (s_worker_running.load(std::memory_order_acquire)) {
        if (s_start_retries.fetch_add(1, std::memory_order_acq_rel) < 50) {
            lv_timer_create(
                [](lv_timer_t* timer) {
                    const uint32_t session = static_cast<uint32_t>(
                        reinterpret_cast<uintptr_t>(
                            lv_timer_get_user_data(timer)));
                    lv_timer_delete(timer);
                    if (session_alive(session)) start_session();
                },
                100, reinterpret_cast<void*>(
                         static_cast<uintptr_t>(s_session.load())));
        } else {
            s_start_retries.store(0, std::memory_order_release);
            post_ui(s_session.load(), State::Error, "口语会话切换超时",
                    BubbleRole::Assistant, nullptr);
        }
        return;
    }
    s_start_retries.store(0, std::memory_order_release);
    s_session.fetch_add(1, std::memory_order_acq_rel);
    s_stop_requested.store(false, std::memory_order_release);
    s_interrupt_requested.store(false, std::memory_order_release);
    s_ws_ready.store(false, std::memory_order_release);
    s_response_done_pending.store(false, std::memory_order_release);
    s_response_done_at_us.store(0, std::memory_order_release);
    s_barge_detection_requested.store(false, std::memory_order_release);
    s_mic_enabled.store(false, std::memory_order_release);
    s_last_done_response_id.store(0, std::memory_order_release);
    s_drop_incoming_audio.store(false, std::memory_order_release);
    s_text = nullptr;
    s_text_bubble = nullptr;
    s_assistant_row = nullptr;
    s_user_text_label = nullptr;
    s_user_text_bubble = nullptr;
    s_user_row = nullptr;
    clear_audio_queue();
    {
        std::lock_guard<std::mutex> lock(s_text_mutex);
        s_answer.clear();
        s_user_text.clear();
        s_resample_position = 0;
    }
    s_worker_running.store(true, std::memory_order_release);
    if (xTaskCreate(worker_task, "oral_ws", 8192, nullptr, 4, &s_worker_task) != pdPASS) {
        s_worker_running.store(false, std::memory_order_release);
        s_worker_task = nullptr;
        post_ui(s_session.load(), State::Error, "启动口语会话失败", BubbleRole::Assistant, nullptr);
    }
}

void stop_session() {
    s_user_exit.store(true, std::memory_order_release);
    s_stop_requested.store(true, std::memory_order_release);
    s_reconnect_requested.store(false, std::memory_order_release);
    s_interrupt_requested.store(false, std::memory_order_release);
    s_response_done_pending.store(false, std::memory_order_release);
    s_barge_detection_requested.store(false, std::memory_order_release);
    s_mic_enabled.store(false, std::memory_order_release);
    // WebSocket Send/Close 可能阻塞；LVGL 线程只置退出标志，由 worker 清理。
    s_generation.fetch_add(1, std::memory_order_acq_rel);
}

void on_back(lv_event_t* /*event*/) {
    stop_session();
    lv_obj_t* old_screen = lv_screen_active();
    lv_obj_t* target = s_return_screen;
    s_return_screen = nullptr;
    if (target == nullptr || target == old_screen) {
        target = HomeScreen::Create();
    }
    lv_screen_load(target);
    if (target == ChatScreen::CurrentScreen()) {
        lv_timer_create(
            [](lv_timer_t* timer) {
                lv_timer_delete(timer);
                lv_timer_create(
                    [](lv_timer_t* retry_timer) {
                        lv_timer_delete(retry_timer);
                        ChatScreen::ResumeAfterTemporaryLeave();
                    },
                    300, nullptr);
            },
            1200, nullptr);
    }
    if (old_screen != nullptr && old_screen != target) {
        lv_obj_delete_async(old_screen);
    }
}

void on_screen_unloaded(lv_event_t* /*event*/) {
    s_session.fetch_add(1, std::memory_order_acq_rel);
    stop_session();
    s_screen = nullptr;
    s_title = nullptr;
    s_status = nullptr;
    s_text = nullptr;
    s_text_bubble = nullptr;
    s_assistant_row = nullptr;
    s_user_text_label = nullptr;
    s_user_text_bubble = nullptr;
    s_user_row = nullptr;
    s_chat = nullptr;
}

lv_obj_t* create_label(lv_obj_t* parent, const char* text, const lv_font_t& font, lv_color_t color) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, &font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    return label;
}

}  // namespace

lv_obj_t* OralScreen::Create(lv_obj_t* return_screen) {
    PwrKey_SetShortPressHandler("oral", Interrupt);
    s_return_screen = return_screen;
    s_session.fetch_add(1, std::memory_order_acq_rel);
    s_state.store(State::Connecting, std::memory_order_release);
    s_stop_requested.store(false, std::memory_order_release);
    s_reconnect_requested.store(false, std::memory_order_release);
    s_user_exit.store(false, std::memory_order_release);

    lv_obj_t* screen = lv_obj_create(nullptr);
    s_screen = screen;
    screen_strip_obj_chrome(screen);
    lv_obj_set_size(screen, kPanelW, kPanelH);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0F172A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* header = lv_obj_create(screen);
    screen_strip_obj_chrome(header);
    lv_obj_add_flag(header, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(header, kPanelW, kHeaderH);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(kColorHeaderBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* divider = lv_obj_create(header);
    screen_strip_obj_chrome(divider);
    lv_obj_set_size(divider, kPanelW, 1);
    lv_obj_align(divider, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(divider, lv_color_hex(kColorDivider), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, LV_PART_MAIN);
    screen_make_input_passive(divider);

    lv_obj_t* back = lv_button_create(header);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, kBackBtnSize, kBackBtnSize);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back, LV_OPA_20, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);
    lv_obj_t* back_icon = lv_image_create(back);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    s_title = lv_label_create(header);
    lv_label_set_text(s_title, "一丢口语");
    lv_obj_set_style_text_color(s_title, lv_color_hex(kColorHeaderText), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(s_title, LV_ALIGN_LEFT_MID, 16 + kBackBtnSize + 12, 0);

    s_status = lv_label_create(header);
    lv_label_set_text(s_status, "连接中…");
    lv_obj_set_style_text_font(s_status, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_status, lv_color_hex(kColorStatusConnecting), LV_PART_MAIN);
    lv_obj_align_to(s_status, s_title, LV_ALIGN_OUT_RIGHT_MID, 12, 0);
    screen_make_input_passive(s_status);

    s_chat = lv_obj_create(screen);
    lv_obj_remove_style_all(s_chat);
    lv_obj_set_size(s_chat, kPanelW, kPanelH - kHeaderH);
    lv_obj_set_pos(s_chat, 0, kHeaderH);
    lv_obj_set_flex_flow(s_chat, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_chat, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_left(s_chat, kListPadH, LV_PART_MAIN);
    lv_obj_set_style_pad_right(s_chat, kListPadH, LV_PART_MAIN);
    lv_obj_set_style_pad_top(s_chat, kListPadTop - kHeaderH, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(s_chat, kListPadBottom, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(s_chat, kRowGap, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_chat, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_chat, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(s_chat, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_chat, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_chat, LV_SCROLLBAR_MODE_AUTO);

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
    s_drop_incoming_audio.store(true, std::memory_order_release);
    audio.ClearPlaybackQueue();
    clear_audio_queue();
    s_generation.fetch_add(1, std::memory_order_acq_rel);
    s_response_done_pending.store(false, std::memory_order_release);
    s_interrupt_requested.store(true, std::memory_order_release);
    s_mic_enabled.store(true, std::memory_order_release);
    post_ui(s_session.load(), State::Listening, "聆听中…", BubbleRole::Assistant, nullptr);
}

void OralScreen::RequestInterrupt() {
    s_interrupt_requested.store(true, std::memory_order_release);
}
