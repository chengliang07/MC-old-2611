#include "oral_screen.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
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
constexpr int32_t kFooterH = 96;
constexpr int32_t kHeaderSidePad = 8;
constexpr const char* kRelayUrl =
    "wss://oemsbxavqkmtuyasxsba.supabase.co/functions/v1/oral-coach-relay-probe";
constexpr int kInputSampleRate = 16000;
constexpr int kInputFrameMs = 100;
constexpr int kInputSamples = kInputSampleRate * kInputFrameMs / 1000;
constexpr int kOutputSampleRate = 24000;
constexpr int kMaxTextChars = 800;
constexpr int kMaxTextBytes = kMaxTextChars * 4 + 1;
constexpr uint32_t kRelayIdleTimeoutMs = 180000;
constexpr uint32_t kInterruptTranscriptGuardMs = 300;
constexpr uint32_t kStreamFlushMs = 180;
constexpr uint32_t kMaxQueuedAudioFrames = 8;
constexpr uint32_t kColorBg = 0x0E1116;
constexpr uint32_t kColorHeaderBg = 0x12151C;
constexpr uint32_t kColorCard = 0x202736;
constexpr uint32_t kColorUserCard = 0x1E3A2F;
constexpr uint32_t kColorDivider = 0x2A2F3A;
constexpr uint32_t kColorText = 0xE5E7EB;
constexpr uint32_t kColorUserText = 0xE8F5E9;
constexpr uint32_t kColorMuted = 0x60A5FA;
constexpr uint32_t kColorHint = 0x94A3B8;
constexpr uint32_t kColorListening = 0x34D399;
constexpr uint32_t kColorConnecting = 0xFBBF24;
constexpr uint32_t kColorError = 0xF87171;

enum class State { Idle, Connecting, Listening, Answering, Error };
enum class TextRole { Assistant, User };

std::atomic<uint32_t> s_session{0};
std::atomic<State> s_state{State::Idle};
std::atomic<bool> s_stop_requested{true};
std::atomic<bool> s_ws_ready{false};
std::atomic<bool> s_mic_enabled{false};
std::atomic<bool> s_response_done_pending{false};
std::atomic<bool> s_relay_needs_restart{false};
std::atomic<bool> s_ws_closing{false};
std::atomic<uint32_t> s_last_relay_message_ms{0};
std::atomic<uint32_t> s_drop_relay_before_ms{0};
std::atomic<uint32_t> s_stream_expected_sequence{1};
std::atomic<uint32_t> s_ignore_transcript_until_ms{0};
std::atomic<uint32_t> s_connection_epoch{0};
std::atomic<uint32_t> s_generation{0};

lv_obj_t* s_screen = nullptr;
lv_obj_t* s_title = nullptr;
lv_obj_t* s_status = nullptr;
lv_obj_t* s_user_text = nullptr;
lv_obj_t* s_text = nullptr;
lv_obj_t* s_user_bubble = nullptr;
lv_obj_t* s_assistant_bubble = nullptr;
lv_obj_t* s_chat = nullptr;
TaskHandle_t s_worker_task = nullptr;
std::unique_ptr<WebSocket> s_ws;
std::mutex s_ws_mutex;
std::mutex s_text_mutex;
std::string s_answer;
std::string s_user_answer;
std::string s_stream_text;
uint32_t s_stream_text_ms = 0;
int s_resample_position = 0;
std::mutex s_audio_queue_mutex;
std::deque<std::vector<int16_t>> s_audio_frames;
std::map<uint32_t, std::string> s_stream_chunks;
std::atomic<bool> s_audio_drain_running{false};

bool session_alive(uint32_t session) {
    return session == s_session.load(std::memory_order_acquire) &&
           s_state.load(std::memory_order_acquire) != State::Idle;
}

void send_ws_json(const char* text);

uint32_t now_ms() {
    return static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

void set_mic_enabled(bool enabled) {
    if (s_mic_enabled.exchange(enabled, std::memory_order_acq_rel) == enabled) {
        return;
    }
    send_ws_json(enabled ? "{\"type\":\"mic_state\",\"enabled\":true}"
                         : "{\"type\":\"mic_state\",\"enabled\":false}");
}

uint32_t status_color(State state) {
    switch (state) {
        case State::Listening:
            return kColorListening;
        case State::Answering:
            return kColorMuted;
        case State::Error:
            return kColorError;
        default:
            return kColorConnecting;
    }
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

void post_ui(uint32_t session, State state, const char* status, const char* text,
             TextRole role = TextRole::Assistant, bool replace = false) {
    struct UiPayload {
        uint32_t session;
        State state;
        uint32_t generation;
        char status[96];
        char text[512];
        TextRole role;
        bool replace;
    };
    if (!session_alive(session)) state = State::Error;
    auto* payload = new UiPayload{};
    payload->session = session;
    payload->state = state;
    payload->generation = s_generation.load(std::memory_order_acquire);
    payload->role = role;
    payload->replace = replace;
    std::snprintf(payload->status, sizeof(payload->status), "%s", status ? status : "");
    std::snprintf(payload->text, sizeof(payload->text), "%s", text ? text : "");
    if (lv_async_call([](void* data) {
            std::unique_ptr<UiPayload> item(static_cast<UiPayload*>(data));
            if (!session_alive(item->session)) return;
            if (item->generation != 0 &&
                item->generation != s_generation.load(std::memory_order_acquire)) return;
            s_state.store(item->state, std::memory_order_release);
            if (s_status != nullptr) {
                lv_label_set_text(s_status, item->status);
                lv_obj_set_style_text_color(s_status, lv_color_hex(status_color(item->state)), LV_PART_MAIN);
            }
            if (item->replace && item->role == TextRole::User && s_user_text != nullptr) {
                std::lock_guard<std::mutex> lock(s_text_mutex);
                s_user_answer = item->text;
                lv_label_set_text(s_user_text, s_user_answer.c_str());
                if (s_user_bubble != nullptr) lv_obj_scroll_to_y(s_user_bubble, 0, LV_ANIM_OFF);
            }
            if (item->replace && item->role == TextRole::Assistant && s_text != nullptr) {
                std::lock_guard<std::mutex> lock(s_text_mutex);
                s_answer = item->text;
                lv_label_set_text(s_text, s_answer.c_str());
                if (s_assistant_bubble != nullptr) lv_obj_scroll_to_y(s_assistant_bubble, 0, LV_ANIM_OFF);
                if (s_chat != nullptr) lv_obj_scroll_to_y(s_chat, LV_COORD_MAX, LV_ANIM_OFF);
            } else if (!item->replace && item->text[0] != '\0' && item->role == TextRole::User &&
                       s_user_text != nullptr) {
                std::lock_guard<std::mutex> lock(s_text_mutex);
                s_user_answer += item->text;
                while (s_user_answer.size() > kMaxTextBytes) {
                    s_user_answer.erase(s_user_answer.begin());
                    while (!s_user_answer.empty() &&
                           (static_cast<unsigned char>(s_user_answer.front()) & 0xC0) == 0x80) {
                        s_user_answer.erase(s_user_answer.begin());
                    }
                }
                lv_label_set_text(s_user_text, s_user_answer.c_str());
                if (s_user_bubble != nullptr) lv_obj_scroll_to_y(s_user_bubble, LV_COORD_MAX, LV_ANIM_OFF);
            } else if (!item->replace && item->text[0] != '\0' && s_text != nullptr) {
                std::lock_guard<std::mutex> lock(s_text_mutex);
                s_answer += item->text;
                while (s_answer.size() > kMaxTextBytes) {
                    s_answer.erase(s_answer.begin());
                    while (!s_answer.empty() &&
                           (static_cast<unsigned char>(s_answer.front()) & 0xC0) == 0x80) {
                        s_answer.erase(s_answer.begin());
                    }
                }
                lv_label_set_text(s_text, s_answer.c_str());
                if (s_assistant_bubble != nullptr) lv_obj_scroll_to_y(s_assistant_bubble, LV_COORD_MAX, LV_ANIM_OFF);
                if (s_chat != nullptr) lv_obj_scroll_to_y(s_chat, LV_COORD_MAX, LV_ANIM_OFF);
            }
        }, payload) != LV_RESULT_OK) {
        delete payload;
    }
}

void clear_chat_text(uint32_t session, const char* status) {
    {
        std::lock_guard<std::mutex> lock(s_text_mutex);
        s_answer.clear();
        s_stream_text.clear();
        s_stream_chunks.clear();
        s_stream_text_ms = 0;
    }
    s_stream_expected_sequence.store(1, std::memory_order_release);
    post_ui(session, State::Answering, status, "", TextRole::Assistant, true);
}

void flush_stream_chunks(uint32_t session, bool force = false) {
    std::string text;
    {
        std::lock_guard<std::mutex> lock(s_text_mutex);
        uint32_t next_sequence =
            s_stream_expected_sequence.load(std::memory_order_acquire);
        while (true) {
            auto it = s_stream_chunks.find(next_sequence);
            if (it == s_stream_chunks.end()) break;
            text += it->second;
            s_stream_chunks.erase(it);
            ++next_sequence;
        }
        s_stream_expected_sequence.store(next_sequence, std::memory_order_release);
        s_stream_text += text;
        if (s_stream_text.empty()) return;
        if (!force && s_stream_text.size() < 24 &&
            now_ms() - s_stream_text_ms < kStreamFlushMs) {
            return;
        }
        text.swap(s_stream_text);
        s_stream_text.clear();
        s_stream_text_ms = 0;
    }
    post_ui(session, State::Answering, "回答中…", text.c_str());
}

void flush_stream_text(uint32_t session) {
    flush_stream_chunks(session, true);
}

void drain_audio_frames(uint32_t session, uint32_t generation) {
    auto& audio = Application::GetInstance().GetAudioService();
    while (!s_stop_requested.load(std::memory_order_acquire) && session_alive(session) &&
           generation == s_generation.load(std::memory_order_acquire)) {
        std::vector<int16_t> frame;
        {
            std::lock_guard<std::mutex> lock(s_audio_queue_mutex);
            if (s_audio_frames.empty()) break;
            frame.swap(s_audio_frames.front());
            s_audio_frames.pop_front();
        }
        if (generation != s_generation.load(std::memory_order_acquire)) continue;
        for (int attempt = 0; attempt < 40; ++attempt) {
            if (audio.PushPcmToPlaybackQueue(frame.data(), frame.size(), generation)) break;
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    s_audio_drain_running.store(false, std::memory_order_release);
}

void close_websocket() {
    std::lock_guard<std::mutex> lock(s_ws_mutex);
    if (s_ws) {
        s_ws_closing.store(true, std::memory_order_release);
        s_connection_epoch.fetch_add(1, std::memory_order_acq_rel);
        s_ws->Close();
        s_ws.reset();
    }
    s_ws_closing.store(false, std::memory_order_release);
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
    const uint32_t received_ms = now_ms();
    s_last_relay_message_ms.store(received_ms, std::memory_order_release);
    if (received_ms < s_drop_relay_before_ms.load(std::memory_order_acquire)) return;
    if (data == nullptr || len < sizeof(int16_t)) return;
    size_t samples = (len / sizeof(int16_t)) & ~static_cast<size_t>(1);
    auto* pcm = reinterpret_cast<const int16_t*>(data);
    std::vector<int16_t> input(pcm, pcm + samples);
    std::vector<int16_t> output;
    resample_24k_to_16k(input, output);
    const uint32_t generation = s_generation.load(std::memory_order_acquire);
    bool start_drain = false;
    {
        std::lock_guard<std::mutex> lock(s_audio_queue_mutex);
        if (s_audio_frames.size() >= kMaxQueuedAudioFrames) {
            s_audio_frames.pop_front();
        }
        s_audio_frames.emplace_back(std::move(output));
        if (!s_audio_drain_running.exchange(true, std::memory_order_acq_rel)) {
            start_drain = true;
        }
    }
    if (start_drain) {
        drain_audio_frames(session, generation);
    }
}

void handle_json(const char* data, size_t len, uint32_t session) {
    if (data == nullptr || len == 0) return;
    const uint32_t received_ms = now_ms();
    s_last_relay_message_ms.store(received_ms, std::memory_order_release);
    cJSON* root = cJSON_ParseWithLength(data, len);
    if (root == nullptr) return;
    char type[64] = {0};
    const cJSON* type_item = cJSON_GetObjectItem(root, "type");
    if (cJSON_IsString(type_item) && type_item->valuestring != nullptr) {
        std::snprintf(type, sizeof(type), "%s", type_item->valuestring);
    }
    if (std::strncmp(type, "relay.", 6) == 0 && received_ms <
        s_drop_relay_before_ms.load(std::memory_order_acquire)) {
        cJSON_Delete(root);
        return;
    }

    if (std::strcmp(type, "relay.ready") == 0) {
        s_drop_relay_before_ms.store(0, std::memory_order_release);
        s_ignore_transcript_until_ms.store(0, std::memory_order_release);
        s_ws_ready.store(true, std::memory_order_release);
        s_response_done_pending.store(false, std::memory_order_release);
        s_stream_expected_sequence.store(1, std::memory_order_release);
        set_mic_enabled(true);
        post_ui(session, State::Listening, "聆听中…", nullptr);
    } else if (std::strcmp(type, "relay.speech_stopped") == 0) {
        set_mic_enabled(false);
        post_ui(session, State::Answering, "思考中…", nullptr);
    } else if (std::strcmp(type, "relay.user_transcript.delta") == 0) {
        if (received_ms < s_ignore_transcript_until_ms.load(std::memory_order_acquire)) {
            cJSON_Delete(root);
            return;
        }
        const cJSON* delta = cJSON_GetObjectItem(root, "delta");
        if (cJSON_IsString(delta) && delta->valuestring != nullptr) {
            post_ui(session, State::Listening, "聆听中…", delta->valuestring, TextRole::User, false);
        }
    } else if (std::strcmp(type, "relay.user_transcript.done") == 0) {
        if (received_ms < s_ignore_transcript_until_ms.load(std::memory_order_acquire)) {
            cJSON_Delete(root);
            return;
        }
        const cJSON* text = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(text) && text->valuestring != nullptr) {
            post_ui(session, State::Answering, "思考中…", "", TextRole::Assistant, true);
            post_ui(session, State::Answering, "思考中…", text->valuestring, TextRole::User, true);
        }
    } else if (std::strcmp(type, "relay.response.start") == 0) {
        const cJSON* generation_item = cJSON_GetObjectItem(root, "generation");
        const uint32_t generation = cJSON_IsNumber(generation_item)
            ? static_cast<uint32_t>(generation_item->valuedouble)
            : s_generation.load(std::memory_order_acquire);
        if (generation != s_generation.load(std::memory_order_acquire)) {
            cJSON_Delete(root);
            return;
        }
        s_drop_relay_before_ms.store(0, std::memory_order_release);
        s_response_done_pending.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(s_audio_queue_mutex);
            s_audio_frames.clear();
        }
        {
            std::lock_guard<std::mutex> lock(s_text_mutex);
            s_stream_chunks.clear();
            s_stream_text.clear();
            s_stream_text_ms = 0;
        }
        s_stream_expected_sequence.store(1, std::memory_order_release);
        set_mic_enabled(false);
        clear_chat_text(session, "回答中…");
    } else if (std::strcmp(type, "relay.interrupted") == 0) {
        const cJSON* generation_item = cJSON_GetObjectItem(root, "generation");
        const uint32_t generation = cJSON_IsNumber(generation_item)
            ? static_cast<uint32_t>(generation_item->valuedouble)
            : s_generation.load(std::memory_order_acquire);
        if (generation != s_generation.load(std::memory_order_acquire)) {
            cJSON_Delete(root);
            return;
        }
        s_response_done_pending.store(false, std::memory_order_release);
        s_drop_relay_before_ms.store(0, std::memory_order_release);
        set_mic_enabled(true);
        post_ui(session, State::Listening, "聆听中…", nullptr);
    } else if (std::strcmp(type, "relay.text.delta") == 0) {
        const cJSON* generation_item = cJSON_GetObjectItem(root, "generation");
        const cJSON* sequence_item = cJSON_GetObjectItem(root, "seq");
        const uint32_t generation = cJSON_IsNumber(generation_item)
            ? static_cast<uint32_t>(generation_item->valuedouble)
            : s_generation.load(std::memory_order_acquire);
        const uint32_t sequence = cJSON_IsNumber(sequence_item)
            ? static_cast<uint32_t>(sequence_item->valuedouble)
            : 0;
        if (generation != s_generation.load(std::memory_order_acquire)) {
            cJSON_Delete(root);
            return;
        }
        const cJSON* delta = cJSON_GetObjectItem(root, "delta");
        if (cJSON_IsString(delta) && delta->valuestring != nullptr) {
            {
                std::lock_guard<std::mutex> lock(s_text_mutex);
                if (sequence != 0) {
                    s_stream_chunks[sequence] = delta->valuestring;
                } else {
                    s_stream_text += delta->valuestring;
                }
                if (s_stream_text_ms == 0) {
                    s_stream_text_ms = now_ms();
                }
            }
            flush_stream_chunks(session);
        }
    } else if (std::strcmp(type, "relay.response.done") == 0) {
        const cJSON* generation_item = cJSON_GetObjectItem(root, "generation");
        const uint32_t generation = cJSON_IsNumber(generation_item)
            ? static_cast<uint32_t>(generation_item->valuedouble)
            : s_generation.load(std::memory_order_acquire);
        if (generation != s_generation.load(std::memory_order_acquire)) {
            cJSON_Delete(root);
            return;
        }
        if (session_alive(session)) {
            flush_stream_text(session);
            set_mic_enabled(false);
            s_response_done_pending.store(true, std::memory_order_release);
            post_ui(session, State::Answering, "回答完成，正在恢复聆听…", nullptr);
        }
    } else if (std::strcmp(type, "relay.error") == 0) {
        const cJSON* error = cJSON_GetObjectItem(root, "error");
        const char* detail = cJSON_IsString(error) ? error->valuestring : "服务错误";
        char message[256];
        std::snprintf(message, sizeof(message), "错误：%s", detail);
        post_ui(session, State::Error, message, nullptr);
    } else if (std::strcmp(type, "relay.qwen.closed") == 0) {
        s_relay_needs_restart.store(true, std::memory_order_release);
        s_ws_ready.store(false, std::memory_order_release);
        s_mic_enabled.store(false, std::memory_order_release);
        s_response_done_pending.store(false, std::memory_order_release);
        post_ui(session, State::Connecting, "口语服务重连中…", nullptr);
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
    const uint32_t connection_epoch =
        s_connection_epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
    ws->OnData([session, connection_epoch](const char* data, size_t len, bool binary) {
        if (!session_alive(session) ||
            connection_epoch != s_connection_epoch.load(std::memory_order_acquire)) {
            return;
        }
        if (binary) handle_binary_audio(data, len, session);
        else handle_json(data, len, session);
    });
    ws->OnDisconnected([connection_epoch]() {
        if (connection_epoch != s_connection_epoch.load(std::memory_order_acquire)) {
            return;
        }
        s_ws_ready.store(false, std::memory_order_release);
        s_mic_enabled.store(false, std::memory_order_release);
        if (!s_ws_closing.load(std::memory_order_acquire) &&
            !s_stop_requested.load(std::memory_order_acquire)) {
            s_relay_needs_restart.store(true, std::memory_order_release);
        }
    });
    ws->OnError([session, connection_epoch](int code) {
        if (connection_epoch != s_connection_epoch.load(std::memory_order_acquire)) {
            return;
        }
        char message[128];
        std::snprintf(message, sizeof(message), "WebSocket 错误：%d", code);
        post_ui(session, State::Error, message, nullptr);
        s_ws_ready.store(false, std::memory_order_release);
        s_mic_enabled.store(false, std::memory_order_release);
        if (!s_stop_requested.load(std::memory_order_acquire)) {
            s_relay_needs_restart.store(true, std::memory_order_release);
        }
    });
    if (!ws->Connect(relay_url.c_str())) {
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
        const uint32_t last_message_ms =
            s_last_relay_message_ms.load(std::memory_order_acquire);
        if (s_relay_needs_restart.load(std::memory_order_acquire)) {
            break;
        }
        if (last_message_ms != 0 &&
            now_ms() - last_message_ms > kRelayIdleTimeoutMs) {
            s_relay_needs_restart.store(true, std::memory_order_release);
            break;
        }
        if (s_response_done_pending.load(std::memory_order_acquire)) {
            if (s_state.load(std::memory_order_acquire) == State::Listening) {
                s_response_done_pending.store(false, std::memory_order_release);
                continue;
            }
            if (audio.IsPlaybackPending()) {
                playback_idle_tick = 0;
            } else {
                const TickType_t now = xTaskGetTickCount();
                if (playback_idle_tick == 0) {
                    playback_idle_tick = now;
                } else if (now - playback_idle_tick >= pdMS_TO_TICKS(250)) {
                    s_response_done_pending.store(false, std::memory_order_release);
                    set_mic_enabled(true);
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

    while (!s_stop_requested.load(std::memory_order_acquire) &&
           session_alive(session)) {
        s_relay_needs_restart.store(false, std::memory_order_release);
        s_ws_ready.store(false, std::memory_order_release);
        s_mic_enabled.store(false, std::memory_order_release);
        s_response_done_pending.store(false, std::memory_order_release);
        s_drop_relay_before_ms.store(0, std::memory_order_release);
        s_ignore_transcript_until_ms.store(0, std::memory_order_release);
        s_last_relay_message_ms.store(now_ms(), std::memory_order_release);
        post_ui(session, State::Connecting, "连接中…", nullptr);
        std::string error;
        if (!connect_websocket(session, error)) {
            post_ui(session, State::Error, error.c_str(), nullptr);
            break;
        }
        send_ws_json("{\"type\":\"start\"}");
        for (int i = 0; i < 100 && session_alive(session) &&
             !s_stop_requested.load(std::memory_order_acquire) &&
             !s_ws_ready.load(std::memory_order_acquire); ++i) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (!s_ws_ready.load(std::memory_order_acquire)) {
            if (!s_stop_requested.load(std::memory_order_acquire)) {
                post_ui(session, State::Error, "等待口语服务就绪超时", nullptr);
            }
            close_websocket();
            break;
        }
        set_mic_enabled(true);
        stream_microphone(session);
        set_mic_enabled(false);
        close_websocket();
        if (!s_relay_needs_restart.load(std::memory_order_acquire) ||
            s_stop_requested.load(std::memory_order_acquire) ||
            !session_alive(session)) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    s_worker_task = nullptr;
    vTaskDelete(nullptr);
}

void start_session() {
    if (s_worker_task != nullptr) return;
    s_relay_needs_restart.store(false, std::memory_order_release);
    s_drop_relay_before_ms.store(0, std::memory_order_release);
    s_ignore_transcript_until_ms.store(0, std::memory_order_release);
    s_session.fetch_add(1, std::memory_order_acq_rel);
    s_stop_requested.store(false, std::memory_order_release);
    s_ws_ready.store(false, std::memory_order_release);
    s_response_done_pending.store(false, std::memory_order_release);
    s_last_relay_message_ms.store(now_ms(), std::memory_order_release);
    s_stream_expected_sequence.store(1, std::memory_order_release);
    set_mic_enabled(false);
    {
        std::lock_guard<std::mutex> lock(s_text_mutex);
        s_answer.clear();
        s_user_answer.clear();
        s_stream_text.clear();
        s_stream_chunks.clear();
        s_stream_text_ms = 0;
        s_resample_position = 0;
    }
    if (s_user_text != nullptr) lv_label_set_text(s_user_text, "");
    if (s_text != nullptr) lv_label_set_text(s_text, "");
    if (xTaskCreate(worker_task, "oral_ws", 4096, nullptr, 4, &s_worker_task) != pdPASS) {
        s_worker_task = nullptr;
        post_ui(s_session.load(), State::Error, "启动口语会话失败", nullptr);
    }
}

void stop_session() {
    s_relay_needs_restart.store(false, std::memory_order_release);
    s_stop_requested.store(true, std::memory_order_release);
    s_response_done_pending.store(false, std::memory_order_release);
    s_drop_relay_before_ms.store(0, std::memory_order_release);
    s_ignore_transcript_until_ms.store(0, std::memory_order_release);
    s_ws_ready.store(false, std::memory_order_release);
    s_mic_enabled.store(false, std::memory_order_release);
    auto& audio = Application::GetInstance().GetAudioService();
    audio.ClearPlaybackQueue();
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
    s_user_text = nullptr;
    s_text = nullptr;
    s_user_bubble = nullptr;
    s_assistant_bubble = nullptr;
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

void build_header(lv_obj_t* parent) {
    lv_obj_t* top = lv_obj_create(parent);
    screen_strip_obj_chrome(top);
    lv_obj_set_size(top, kPanelW, kHeaderH);
    lv_obj_set_pos(top, 0, 0);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(top, lv_color_hex(kColorHeaderBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t* divider = lv_obj_create(top);
    screen_strip_obj_chrome(divider);
    lv_obj_set_size(divider, kPanelW, 1);
    lv_obj_align(divider, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(divider, lv_color_hex(kColorDivider), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, LV_PART_MAIN);
    screen_make_input_passive(divider);

    lv_obj_t* back = lv_button_create(top);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, kBackBtnSize, kBackBtnSize);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, kHeaderSidePad, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back, LV_OPA_20, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);

    lv_obj_t* back_icon = lv_image_create(back);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    s_title = lv_label_create(top);
    lv_label_set_text(s_title, "一丢口语");
    lv_obj_set_style_text_font(s_title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_title, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_align(s_title, LV_ALIGN_LEFT_MID, kHeaderSidePad + kBackBtnSize + 8, 0);
    screen_make_input_passive(s_title);

    s_status = lv_label_create(top);
    lv_label_set_text(s_status, "准备中…");
    lv_obj_set_style_text_font(s_status, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_status, lv_color_hex(kColorConnecting), LV_PART_MAIN);
    lv_obj_align(s_status, LV_ALIGN_RIGHT_MID, -16, 0);
    screen_make_input_passive(s_status);
}

void build_bubble(lv_obj_t* parent, bool is_user, lv_obj_t** out_label, lv_obj_t** out_bubble) {
    lv_obj_t* row = lv_obj_create(parent);
    screen_strip_obj_chrome(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(row, 12, LV_PART_MAIN);

    lv_obj_t* bubble = lv_obj_create(row);
    screen_strip_obj_chrome(bubble);
    lv_obj_set_width(bubble, is_user ? 460 : 620);
    lv_obj_set_height(bubble, is_user ? 90 : 300);
    lv_obj_set_style_bg_color(bubble, lv_color_hex(is_user ? kColorUserCard : kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bubble, 18, LV_PART_MAIN);
    lv_obj_set_style_border_width(bubble, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(bubble, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(bubble, 14, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(bubble, 0, LV_PART_MAIN);
    lv_obj_remove_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    if (!is_user) {
        lv_obj_add_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(bubble, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(bubble, LV_SCROLLBAR_MODE_AUTO);
    }
    if (is_user) lv_obj_align(bubble, LV_ALIGN_TOP_RIGHT, -8, 0);
    else lv_obj_align(bubble, LV_ALIGN_TOP_LEFT, 8, 0);

    lv_obj_t* label = lv_label_create(bubble);
    lv_label_set_text(label, "");
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(is_user ? kColorUserText : kColorText), LV_PART_MAIN);
    screen_make_input_passive(label);
    *out_label = label;
    *out_bubble = bubble;
}

void build_body(lv_obj_t* parent) {
    s_chat = lv_obj_create(parent);
    screen_strip_obj_chrome(s_chat);
    lv_obj_set_size(s_chat, kPanelW, kPanelH - kHeaderH - kFooterH);
    lv_obj_set_pos(s_chat, 0, kHeaderH);
    lv_obj_set_style_bg_color(s_chat, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_chat, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_chat, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(s_chat, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_chat, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_chat, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_add_flag(s_chat, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_chat, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_chat, LV_SCROLLBAR_MODE_AUTO);
    screen_swipe_back_ignore(s_chat, true);

    build_bubble(s_chat, true, &s_user_text, &s_user_bubble);
    build_bubble(s_chat, false, &s_text, &s_assistant_bubble);
}

void build_footer(lv_obj_t* parent) {
    lv_obj_t* footer = lv_obj_create(parent);
    screen_strip_obj_chrome(footer);
    lv_obj_set_size(footer, kPanelW, kFooterH);
    lv_obj_set_pos(footer, 0, kPanelH - kFooterH);
    lv_obj_set_style_bg_color(footer, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(footer, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(footer, 16, LV_PART_MAIN);
    lv_obj_remove_flag(footer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* hint = create_label(footer, "短按电源键打断 · 返回键退出", font_puhui_20_4,
                                  lv_color_hex(kColorHint));
    lv_obj_center(hint);
    screen_make_input_passive(hint);
}

}  // namespace

lv_obj_t* OralScreen::Create() {
    PwrKey_SetShortPressHandler("oral", Interrupt);
    s_session.fetch_add(1, std::memory_order_acq_rel);
    s_state.store(State::Connecting, std::memory_order_release);
    s_stop_requested.store(false, std::memory_order_release);
    s_drop_relay_before_ms.store(0, std::memory_order_release);
    s_ignore_transcript_until_ms.store(0, std::memory_order_release);

    lv_obj_t* screen = lv_obj_create(nullptr);
    s_screen = screen;
    screen_strip_obj_chrome(screen);
    lv_obj_set_size(screen, kPanelW, kPanelH);
    lv_obj_set_style_bg_color(screen, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    build_header(screen);
    build_body(screen);
    build_footer(screen);

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
    const uint32_t started_ms = now_ms();
    s_ignore_transcript_until_ms.store(started_ms + kInterruptTranscriptGuardMs,
                                       std::memory_order_release);
    s_response_done_pending.store(false, std::memory_order_release);
    const uint32_t generation = s_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    audio.StopPlaybackNow();
    {
        std::lock_guard<std::mutex> lock(s_audio_queue_mutex);
        s_audio_frames.clear();
    }
    {
        std::lock_guard<std::mutex> lock(s_text_mutex);
        s_stream_chunks.clear();
        s_stream_text.clear();
        s_stream_text_ms = 0;
    }
    s_stream_expected_sequence.store(1, std::memory_order_release);
    s_drop_relay_before_ms.store(started_ms, std::memory_order_release);
    const std::string interrupt_message =
        "{\"type\":\"interrupt\",\"generation\":" + std::to_string(generation) + "}";
    send_ws_json(interrupt_message.c_str());
    set_mic_enabled(true);
    post_ui(s_session.load(), State::Listening, "聆听中…", nullptr);
}
