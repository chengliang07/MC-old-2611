#include "oral_screen.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <deque>
#include <vector>

#include "cJSON.h"
#include "esp_heap_caps.h"
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
#include "idle_power_policy.h"
#include "pwr_key_handler.h"
#include "settings.h"
#include <web_socket.h>

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr const char* TAG = "OralScreen";
// 临时诊断打点（定位口语卡顿）：1=打开，0=关闭。只打日志，不改任何逻辑。
#define ORAL_TRACE 1
constexpr int32_t kPanelW = 720;
constexpr int32_t kPanelH = 720;
constexpr int32_t kHeaderH = 88;
constexpr int32_t kBackBtnSize = 72;
constexpr int32_t kHeaderRightPad = 16;
constexpr int32_t kListPadH = 18;
constexpr int32_t kListPadTop = 102;
// 底部固定留出麦克风/波形区，最后一条气泡不被压住。
constexpr int32_t kFooterH = 104;
constexpr int32_t kListPadBottom = 108;
constexpr int32_t kRowGap = 14;
constexpr int32_t kBubblePadX = 18;
constexpr int32_t kBubblePadY = 14;
constexpr int32_t kBubbleRadius = 22;
constexpr int32_t kSideMargin = 8;
// AI 侧头像；用户侧不加头像，右对齐本身就是身份标识。
constexpr int32_t kAvatarSize = 36;
constexpr int32_t kAvatarGap = 8;
constexpr int32_t kMaxMessages = 12;
constexpr const char* kRelayUrl =
    "wss://oemsbxavqkmtuyasxsba.supabase.co/functions/v1/oral-coach-relay-probe";
constexpr int kInputSampleRate = 16000;
constexpr int kInputFrameMs = 100;
constexpr int kInputSamples = kInputSampleRate * kInputFrameMs / 1000;
constexpr int kOutputSampleRate = 24000;
constexpr int kMaxTextChars = 800;
constexpr int kMaxTextBytes = kMaxTextChars * 4 + 1;
// 100ms 切片配合 MAX_PLAYBACK_TASKS_IN_QUEUE=8，播放侧缓冲约 800ms。
constexpr size_t kPlaybackSliceSamples = 1600;
// 下行缓冲：relay 是突发投递（实测 0.32 s 音频 / 0.2 s 墙钟），
// 192 KB（6 s）在长回答时会被顶满并丢弃最旧音频 —— 表现为“AI 吞字”。
constexpr size_t kMaxAudioChunks = 32;
constexpr size_t kMaxAudioBytes = 256 * 1024;
// 上行发送缓冲：采集只入队，网络再慢也不阻塞读麦（原来 Send 阻塞
// 100–600ms 会直接丢掉 I2S DMA 里的麦克风样本）。
constexpr size_t kMaxTxChunks = 8;
// 服务端漏发 / 长时间不发 response.done 时，客户端会永远停在“回答中”，
// 只能按电源键才能继续。这里做兜底：最后一块音频之后静默太久，就当作本轮结束。
constexpr uint32_t kTurnIdleTimeoutMs = 8000;
// ---- 配色（改这 6 行就能整页换肤）----
// 底色：深海军蓝的竖向渐变，替代原来的纯黑平铺。
constexpr uint32_t kColorBgTop = 0x0E1A2B;
constexpr uint32_t kColorBgBottom = 0x06090F;
// 主色：暖琥珀。备选：蓝 0x4C8DFF / 紫 0xA78BFA / 珊瑚 0xFF7A59。
constexpr uint32_t kColorAccent = 0xF2B25C;
constexpr uint32_t kColorAccentDeep = 0xD98E3E;
constexpr uint32_t kColorAccentInk = 0x241505;  // 主色气泡上的深色字
// 辅助色：用户/AI 两侧的语义色，不参与品牌色。
constexpr uint32_t kColorStatusAnswering = 0x60A5FA;
constexpr uint32_t kColorStatusConnecting = 0x94A3B8;
constexpr uint32_t kColorStatusError = 0xF87171;

constexpr uint32_t kColorHeaderText = 0xF4F6FA;
constexpr uint32_t kColorAiBubbleText = 0xE9EDF5;

enum class State { Idle, Connecting, Listening, Answering, Error };
enum class BubbleRole { User, Assistant };

std::atomic<uint32_t> s_session{0};
std::atomic<State> s_state{State::Idle};
std::atomic<bool> s_stop_requested{true};
std::atomic<bool> s_ws_ready{false};
std::atomic<bool> s_mic_enabled{false};
std::atomic<bool> s_waiting_wake{false};
std::atomic<bool> s_user_streamed{false};
// 用户气泡已用占位符创建、等待转写回填
std::atomic<bool> s_user_pending{false};
// 当前 AI 气泡已设置的宽度（UI 线程独占），用于“只增不减”的平滑扩张
int32_t s_bubble_w = 0;
// 打断/收尾后的上行静音窗：扬声器余音 + 房间混响会被裸麦克风当成用户说话，
// 送上去就变成“AI 自问自答”。窗口内继续读麦（排空 I2S），只是不上传。
std::atomic<uint32_t> s_uplink_mute_until_ms{0};
// 占位气泡的创建时间，用于超时清理（一直没转写就把 "…" 删掉）
std::atomic<uint32_t> s_user_pending_ms{0};
// user_transcript.done 之后迟到的 delta 不能再追加，否则会把下一轮的话
// 拼进上一轮气泡，长对话看起来就是“顺序乱”。
std::atomic<bool> s_user_transcript_closed{false};
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
std::atomic<uint32_t> s_last_text_seq{0};
std::atomic<bool> s_end_after_playback{false};

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
// 状态胶囊：容器 + 发光圆点，颜色跟 State 走。
lv_obj_t* s_status_pill = nullptr;
lv_obj_t* s_status_dot = nullptr;
// 底部聆听区：麦克风胶囊 + 5 根波柱 + 一句提示。
lv_obj_t* s_footer = nullptr;
lv_obj_t* s_hint = nullptr;
lv_obj_t* s_wave_bars[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
lv_timer_t* s_wave_timer = nullptr;
uint32_t s_wave_tick = 0;
lv_obj_t* s_return_screen = nullptr;
TaskHandle_t s_worker_task = nullptr;
std::unique_ptr<WebSocket> s_ws;
// Send 可能同步触发回调，回调里还会发控制帧，必须允许重入。
std::recursive_mutex s_ws_mutex;
lv_timer_t* s_start_retry_timer = nullptr;
std::mutex s_text_mutex;
std::string s_answer;
std::string s_user_text;
std::string s_stream_text;
std::string s_response_text;
uint32_t s_stream_text_ms = 0;
uint32_t s_text_layout_ms = 0;
int s_resample_position = 0;

struct AudioChunk {
    std::vector<int16_t> pcm;
    uint32_t generation;
};
std::deque<AudioChunk> s_audio_chunks;
size_t s_audio_bytes = 0;
std::mutex s_audio_mutex;
std::atomic<bool> s_audio_task_running{false};
// 上行（麦克风）发送队列：采集线程只入队，独立任务负责 WS 发送。
std::deque<std::vector<int16_t>> s_tx_chunks;
std::mutex s_tx_mutex;
std::atomic<bool> s_tx_stop{true};
std::atomic<bool> s_tx_running{false};
std::atomic<uint32_t> s_tx_dropped{0};
std::atomic<uint32_t> s_last_audio_ms{0};
std::atomic<uint32_t> s_rstart_ms{0};

bool session_alive(uint32_t session) {
    return session == s_session.load(std::memory_order_acquire) &&
           s_state.load(std::memory_order_acquire) != State::Idle;
}

uint32_t status_color(State state) {
    switch (state) {
        case State::Listening:
            return kColorAccent;
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

// 状态胶囊 + 底部提示统一在这里改，避免状态色散落在多个回调里。
void set_status_visual(State state, const char* text) {
    const uint32_t color = status_color(state);
    if (s_status != nullptr) {
        lv_label_set_text(s_status, text != nullptr ? text : "");
        lv_obj_set_style_text_color(s_status, lv_color_hex(color), LV_PART_MAIN);
    }
    if (s_status_dot != nullptr) {
        lv_obj_set_style_bg_color(s_status_dot, lv_color_hex(color), LV_PART_MAIN);
        lv_obj_set_style_shadow_color(s_status_dot, lv_color_hex(color), LV_PART_MAIN);
    }
    if (s_status_pill != nullptr) {
        lv_obj_set_style_bg_color(s_status_pill, lv_color_hex(color), LV_PART_MAIN);
        lv_obj_set_style_border_color(s_status_pill, lv_color_hex(color), LV_PART_MAIN);
    }
    if (s_hint != nullptr) {
        const char* hint = "轻触屏幕开始";
        switch (state) {
            case State::Listening:
                hint = "直接说英文，说完停一下";
                break;
            case State::Answering:
                hint = "AI 回答中 · 可随时打断";
                break;
            case State::Connecting:
                hint = "正在连接…";
                break;
            case State::Error:
                hint = "连接失败 · 轻触屏幕重试";
                break;
            default:
                break;
        }
        lv_label_set_text(s_hint, hint);
    }
}

// 底部波柱：5 根柱子按 6 帧循环涨落，只在聆听/回答时动。
void wave_timer_cb(lv_timer_t* /*timer*/) {
    static const uint8_t kFrames[6][5] = {
        {10, 22, 14, 26, 12}, {16, 14, 26, 12, 22}, {22, 26, 12, 22, 14},
        {26, 12, 22, 14, 26}, {14, 22, 12, 26, 16}, {12, 26, 16, 12, 22},
    };
    if (s_footer == nullptr) return;
    const State state = s_state.load(std::memory_order_acquire);
    const bool active =
        state == State::Listening || state == State::Answering;
    const uint8_t* frame = kFrames[s_wave_tick % 6];
    ++s_wave_tick;
    const uint32_t color =
        state == State::Answering ? kColorStatusAnswering : kColorAccent;
    for (int i = 0; i < 5; ++i) {
        if (s_wave_bars[i] == nullptr) continue;
        lv_obj_set_height(s_wave_bars[i], active ? frame[i] : 6);
        lv_obj_set_style_bg_color(s_wave_bars[i], lv_color_hex(color),
                                  LV_PART_MAIN);
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
    // 最大宽度：AI 78%、用户 70%，都不占满屏；超过就在气泡内自动换行。
    const int32_t max_bubble_w = kPanelW * (is_right ? 70 : 78) / 100;
    int32_t text_w = lv_txt_get_width(text, std::strlen(text), font, 0);
    // 最小宽度按“最长单词”算：既能让短句气泡跟着字数变窄（Yes/OK 就是小气泡），
    // 又不会把一个单词逐字折行成“又长又细”的条。
    int32_t longest_word_w = 0;
    {
        const char* p = text;
        while (*p != '\0') {
            while (*p == ' ' || *p == '\n' || *p == '\t') ++p;
            const char* start = p;
            while (*p != '\0' && *p != ' ' && *p != '\n' && *p != '\t') ++p;
            if (p > start) {
                const int32_t w = lv_txt_get_width(start,
                                                   static_cast<uint32_t>(p - start),
                                                   font, 0);
                if (w > longest_word_w) longest_word_w = w;
            }
        }
    }
    const int32_t min_text_w = longest_word_w > 0 ? longest_word_w + 8 : 24;
    if (text_w < min_text_w) text_w = min_text_w;
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

#if ORAL_TRACE
// ---- 临时诊断打点（定位口语卡顿）----
// 只记录时间与队列水位，不改任何行为；ORAL_TRACE=0 时全部消失。
std::atomic<uint32_t> t_last_rx_ms{0};
std::atomic<uint32_t> t_rstart_ms{0};
std::atomic<uint32_t> t_rdone_ms{0};
std::atomic<uint32_t> t_first_audio_ms{0};
std::atomic<uint32_t> t_last_audio_ms{0};
std::atomic<uint32_t> t_tick_ms{0};
std::atomic<uint32_t> t_rx_pkts{0};
std::atomic<uint32_t> t_rx_bytes{0};
std::atomic<uint32_t> t_rx_max_dt{0};
std::atomic<uint32_t> t_rx_slow{0};
std::atomic<uint32_t> t_drop_bytes{0};
std::atomic<uint32_t> t_gap_cnt{0};
std::atomic<uint32_t> t_gap_ms{0};
std::atomic<uint32_t> t_gap_max{0};
std::atomic<uint32_t> t_tx_max_us{0};
std::atomic<uint32_t> t_tx_slow{0};
std::atomic<bool> t_turn_open{false};
uint32_t t_gap_begin_ms = 0;  // 只在 oral_ws 任务里访问
#endif

bool ends_sentence(const char* text) {
    if (text == nullptr || text[0] == '\0') return false;
    const char* end = text + std::strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t' ||
                          end[-1] == '\r')) {
        --end;
    }
    if (end == text) return false;
    const char last = end[-1];
    if (last == '!' || last == '?' || last == ';') return true;
    // 全角标点是 3 字节 UTF-8，不能和 char 直接比较（旧写法永远为 false）。
    if (end - text >= 3) {
        const std::string tail(end - 3, end);
        if (tail == "。" || tail == "！" || tail == "？" || tail == "；") {
            return true;
        }
    }
    return false;
}

// 合并连续换行/空白，避免流式分片在气泡里留下空行。
std::string normalize_text(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    bool last_space = false;
    int newline_run = 0;
    for (char ch : input) {
        if (ch == '\r') continue;
        if (ch == '\n') {
            ++newline_run;
            continue;
        }
        if (newline_run > 0) {
            if (!out.empty()) out.push_back('\n');
            newline_run = 0;
            last_space = false;
        }
        if (ch == ' ' || ch == '\t') {
            if (!last_space && !out.empty()) out.push_back(' ');
            last_space = true;
            continue;
        }
        out.push_back(ch);
        last_space = false;
    }
    while (!out.empty() &&
           (out.back() == ' ' || out.back() == '\n' || out.back() == '\t')) {
        out.pop_back();
    }
    return out;
}

void scroll_chat_to_bottom() {
    if (s_chat == nullptr) return;
    lv_obj_update_layout(s_chat);
    const int32_t y = lv_obj_get_scroll_y(s_chat) +
                      lv_obj_get_scroll_bottom(s_chat);
    lv_obj_scroll_to_y(s_chat, y, LV_ANIM_OFF);
}

bool response_says_goodbye() {
    std::string text;
    {
        std::lock_guard<std::mutex> lock(s_text_mutex);
        text = s_response_text;
    }
    text = normalize_text(text);
    if (text.empty()) return false;
    // 只有“整句就是道别”才算结束会话。之前只要出现 see you / 再见 就退出，
    // 辅导老师一句 "See you next time" 会把会话直接掐掉（表现为“自动等待唤醒”）。
    if (text.size() > 40) return false;
    if (text.find("再见") != std::string::npos ||
        text.find("拜拜") != std::string::npos ||
        text.find("下次见") != std::string::npos ||
        text.find("回头见") != std::string::npos) {
        return true;
    }
    for (char& ch : text) {
        ch = static_cast<char>(
            std::tolower(static_cast<unsigned char>(ch)));
    }
    return text.find("goodbye") != std::string::npos ||
           text.find("see you") != std::string::npos ||
           text.find("bye") != std::string::npos;
}

void post_ui(uint32_t session, State state, const char* status,
             BubbleRole role, const char* text, bool append_user = false,
             bool reset_assistant = false, bool close_assistant = false,
             bool reset_user = false, bool replace_user = false);
void exit_to_return_screen();

void flush_stream_text(uint32_t session, bool close_assistant = false) {
    std::string text;
    {
        std::lock_guard<std::mutex> lock(s_text_mutex);
        if (s_stream_text.empty()) return;
        text = normalize_text(s_stream_text);
        s_stream_text.clear();
        s_stream_text_ms = 0;
    }
    if (text.empty()) return;
    post_ui(session, State::Answering, "回答中…", BubbleRole::Assistant,
            text.c_str(), false, false, close_assistant);
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
             bool reset_assistant, bool close_assistant, bool reset_user,
             bool replace_user) {
    struct UiPayload {
        uint32_t session;
        State state;
        char status[96];
        std::string append;
        BubbleRole role;
        bool replace_text;
        bool append_user;
        uint32_t generation;
        bool reset_assistant;
        bool close_assistant;
        bool reset_user;
        bool replace_user;
    };
    if (!session_alive(session)) state = State::Error;
    auto* payload = new UiPayload{};
    payload->session = session;
    payload->state = state;
    std::snprintf(payload->status, sizeof(payload->status), "%s", status ? status : "");
    payload->append = text ? text : "";
    payload->role = role;
    payload->replace_text = false;
    payload->append_user = append_user;
    payload->generation = s_generation.load(std::memory_order_acquire);
    payload->reset_assistant = reset_assistant;
    payload->close_assistant = close_assistant;
    payload->reset_user = reset_user;
    payload->replace_user = replace_user;
    if (lv_async_call([](void* data) {
            std::unique_ptr<UiPayload> item(static_cast<UiPayload*>(data));
            if (!session_alive(item->session)) return;
            s_state.store(item->state, std::memory_order_release);
            set_status_visual(item->state, item->status);
            if (item->reset_assistant) {
                s_text = nullptr;
                s_text_bubble = nullptr;
                s_assistant_row = nullptr;
                std::lock_guard<std::mutex> lock(s_text_mutex);
                s_answer.clear();
            }
            if (item->reset_user) {
                s_user_text_label = nullptr;
                s_user_text_bubble = nullptr;
                s_user_row = nullptr;
                std::lock_guard<std::mutex> lock(s_text_mutex);
                s_user_text.clear();
            }
            // 用户气泡先占位（"…"）再回填：新模型的 ASR 转写比 response.start
            // 晚到，先建 Assistant 气泡会导致 AI 气泡跑到用户前面。
            // 空文本表示“本轮到结束都没拿到转写”，直接删掉占位气泡。
            if (item->replace_user && item->role == BubbleRole::User) {
                if (item->append.empty()) {
                    if (s_user_row != nullptr) {
                        lv_obj_t* row = s_user_row;
                        s_user_row = nullptr;
                        s_user_text_label = nullptr;
                        s_user_text_bubble = nullptr;
                        {
                            std::lock_guard<std::mutex> lock(s_text_mutex);
                            s_user_text.clear();
                        }
                        lv_obj_delete(row);
                        scroll_chat_to_bottom();
                    }
                    return;
                }
                if (s_user_text_label != nullptr) {
                    std::lock_guard<std::mutex> lock(s_text_mutex);
                    s_user_text = item->append;
                    lv_label_set_text(s_user_text_label, s_user_text.c_str());
                    resize_bubble(s_user_text_bubble, s_user_text_label,
                                  s_user_text.c_str(), true);
                    scroll_chat_to_bottom();
                }
                return;
            }
            // generation 只用来丢弃过期的流式文本；用户气泡是结构性消息，
            // 晚一拍到也必须在，否则最终会被 response.start 的 generation++
            // 判过期而整条丢失（表现为只有第一句有气泡）。
            if (item->role != BubbleRole::User &&
                item->generation != s_generation.load(std::memory_order_acquire)) {
                return;
            }
            if (item->append.empty() || s_chat == nullptr) return;
            if (item->append_user && item->role == BubbleRole::User && s_user_text_label != nullptr) {
                std::lock_guard<std::mutex> lock(s_text_mutex);
                s_user_text += item->append;
                lv_label_set_text(s_user_text_label, s_user_text.c_str());
                // 用户气泡都很短，但同样是流式追加：60ms 一次重排足够，
                // 每个 delta 都 lv_obj_update_layout 会拖慢音频播放。
                const uint32_t now = now_ms();
                if (now - s_text_layout_ms < 60) return;
                s_text_layout_ms = now;
                resize_bubble(s_user_text_bubble, s_user_text_label, s_user_text.c_str(), true);
                scroll_chat_to_bottom();
                return;
            }
            if (!item->replace_text && item->role == BubbleRole::Assistant &&
                s_text != nullptr) {
                bool overflow = false;
                {
                    std::lock_guard<std::mutex> lock(s_text_mutex);
                    // 单气泡到上限就另起一个，绝不从头部删字：
                    // 头部删除会让长回答前后颠倒、缺句。
                    overflow =
                        !item->close_assistant &&
                        s_answer.size() + item->append.size() >
                            static_cast<size_t>(kMaxTextBytes);
                    if (!overflow) {
                        s_answer += item->append;
                    }
                }
                if (!overflow) {
                    const uint32_t now = now_ms();
                    // 关键：流式期间最多 80ms 重排一次。每个 delta 都
                    // lv_label_set_text + lv_obj_update_layout 会让 LVGL 任务
                    // 把 CPU/PSRAM 吃满（实测每秒 20~40 次），音频包被延迟、
                    // 播放端欠载 —— 表现为“改气泡之后 AI 变卡”。
                    const bool due = item->close_assistant ||
                                     now - s_text_layout_ms >= 80;
                    if (!due) return;
                    s_text_layout_ms = now;
                    std::lock_guard<std::mutex> lock(s_text_mutex);
                    lv_label_set_text(s_text, s_answer.c_str());
                    if (item->close_assistant) {
                        resize_bubble(s_text_bubble, s_text, s_answer.c_str(), false);
                        scroll_chat_to_bottom();
                        s_text = nullptr;
                        s_text_bubble = nullptr;
                        s_assistant_row = nullptr;
                        s_answer.clear();
                        s_text_layout_ms = 0;
                        s_bubble_w = 0;
                        return;
                    }
                    // 只在明显变宽时才重排，宽度单调增长，不再抖动。
                    const int32_t want =
                        bubble_width_for_text(s_answer.c_str(), false);
                    if (want >= s_bubble_w + 24) {
                        resize_bubble(s_text_bubble, s_text, s_answer.c_str(), false);
                        s_bubble_w = want;
                    }
                    scroll_chat_to_bottom();
                    return;
                }
                s_text = nullptr;
                s_text_bubble = nullptr;
                s_assistant_row = nullptr;
                {
                    std::lock_guard<std::mutex> lock(s_text_mutex);
                    s_answer.clear();
                }
                s_text_layout_ms = 0;
            }
            bool is_user = item->role == BubbleRole::User;
            lv_obj_t* row = lv_obj_create(s_chat);
            screen_strip_obj_chrome(row);
            lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_set_style_pad_bottom(row, kRowGap, LV_PART_MAIN);
            lv_obj_set_width(row, LV_PCT(100));
            lv_obj_set_height(row, LV_SIZE_CONTENT);

            // AI 侧挂一个 36px 角色方块；用户侧右对齐本身就是身份标识。
            int32_t bubble_x = kSideMargin;
            if (!is_user) {
                lv_obj_t* avatar = lv_obj_create(row);
                screen_strip_obj_chrome(avatar);
                lv_obj_remove_flag(avatar, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_remove_flag(avatar, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_set_size(avatar, kAvatarSize, kAvatarSize);
                lv_obj_set_style_radius(avatar, 12, LV_PART_MAIN);
                lv_obj_set_style_bg_color(avatar, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
                lv_obj_set_style_bg_opa(avatar, 12, LV_PART_MAIN);
                lv_obj_set_style_border_width(avatar, 1, LV_PART_MAIN);
                lv_obj_set_style_border_color(avatar, lv_color_hex(kColorAccent), LV_PART_MAIN);
                lv_obj_set_style_border_opa(avatar, 70, LV_PART_MAIN);
                lv_obj_set_style_pad_all(avatar, 0, LV_PART_MAIN);
                lv_obj_align(avatar, LV_ALIGN_TOP_LEFT, kSideMargin, 2);
                lv_obj_t* avatar_text = lv_label_create(avatar);
                lv_label_set_text(avatar_text, "AI");
                lv_obj_set_style_text_font(avatar_text, &font_puhui_20_4, LV_PART_MAIN);
                lv_obj_set_style_text_color(avatar_text, lv_color_hex(kColorAccent), LV_PART_MAIN);
                lv_obj_center(avatar_text);
                bubble_x = kSideMargin + kAvatarSize + kAvatarGap;
            }

            lv_obj_t* bubble = lv_obj_create(row);
            screen_strip_obj_chrome(bubble);
            lv_obj_set_height(bubble, LV_SIZE_CONTENT);
            lv_obj_set_style_radius(bubble, kBubbleRadius, LV_PART_MAIN);
            lv_obj_set_style_pad_hor(bubble, kBubblePadX, LV_PART_MAIN);
            lv_obj_set_style_pad_ver(bubble, kBubblePadY, LV_PART_MAIN);
            lv_obj_set_style_shadow_width(bubble, 0, LV_PART_MAIN);
            lv_obj_remove_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
            if (is_user) {
                // 用户：主色竖向渐变 + 深色字，整屏对比度最高的一条。
                lv_obj_set_style_border_width(bubble, 0, LV_PART_MAIN);
                lv_obj_set_style_bg_color(bubble, lv_color_hex(kColorAccent), LV_PART_MAIN);
                lv_obj_set_style_bg_grad_color(bubble, lv_color_hex(kColorAccentDeep), LV_PART_MAIN);
                lv_obj_set_style_bg_grad_dir(bubble, LV_GRAD_DIR_VER, LV_PART_MAIN);
                lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, LV_PART_MAIN);
            } else {
                // AI：半透明玻璃 + 1px 描边，弱于用户但不与底色糊在一起。
                lv_obj_set_style_border_width(bubble, 1, LV_PART_MAIN);
                lv_obj_set_style_border_color(bubble, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
                lv_obj_set_style_border_opa(bubble, 26, LV_PART_MAIN);
                lv_obj_set_style_bg_color(bubble, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
                lv_obj_set_style_bg_opa(bubble, 16, LV_PART_MAIN);
            }
            // 气泡宽度按文本自适应；流式期间只在“需要变宽 24px 以上”时
            // 才重排，避免每个分片都跳一次宽度；封口时再精确收窄。
            const int32_t width = bubble_width_for_text(item->append.c_str(), is_user);
            if (!is_user) s_bubble_w = width;
            lv_obj_set_width(bubble, width);
            if (is_user) lv_obj_align(bubble, LV_ALIGN_TOP_RIGHT, -kSideMargin, 0);
            else lv_obj_align(bubble, LV_ALIGN_TOP_LEFT, bubble_x, 0);

            lv_obj_t* label = lv_label_create(bubble);
            lv_label_set_text(label, item->append.c_str());
            lv_obj_set_width(label, width - kBubblePadX * 2);
            lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_font(label, &font_puhui_20_4, LV_PART_MAIN);
            lv_obj_set_style_text_color(
                label, lv_color_hex(is_user ? kColorAccentInk : kColorAiBubbleText),
                LV_PART_MAIN);
            lv_obj_set_style_text_line_space(label, 4, LV_PART_MAIN);
            screen_make_input_passive(row);
            if (!is_user) {
                s_text = label;
                s_text_bubble = bubble;
                s_assistant_row = row;
                std::lock_guard<std::mutex> lock(s_text_mutex);
                s_answer = item->append;
                if (item->close_assistant) {
                    s_text = nullptr;
                    s_text_bubble = nullptr;
                    s_assistant_row = nullptr;
                    s_answer.clear();
                }
                s_text_layout_ms = now_ms();
            } else {
                s_user_text_label = label;
                s_user_text_bubble = bubble;
                s_user_row = row;
                std::lock_guard<std::mutex> lock(s_text_mutex);
                s_user_text = item->append;
            }
            trim_messages();
            scroll_chat_to_bottom();
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

#if ORAL_TRACE
void audio_queue_stats(size_t& chunks, size_t& bytes) {
    std::lock_guard<std::mutex> lock(s_audio_mutex);
    chunks = s_audio_chunks.size();
    bytes = s_audio_bytes;
}

// 每 ~20ms 调一次：统计“该出声却没声音”的空窗（播放欠载），并每秒打一行汇总。
void trace_tick() {
    const uint32_t now = now_ms();
    auto& audio = Application::GetInstance().GetAudioService();
    const bool should_play =
        s_ws_ready.load(std::memory_order_acquire) &&
        !s_mic_enabled.load(std::memory_order_acquire) &&
        !s_response_done_pending.load(std::memory_order_acquire);
    size_t chunks = 0;
    size_t bytes = 0;
    audio_queue_stats(chunks, bytes);
    const bool starved = should_play && chunks == 0 && !audio.IsPlaybackPending();

    if (starved) {
        if (t_gap_begin_ms == 0) t_gap_begin_ms = now;
        const uint32_t d = now - t_gap_begin_ms;
        if (d > t_gap_max.load(std::memory_order_relaxed)) {
            t_gap_max.store(d, std::memory_order_relaxed);
        }
    } else if (t_gap_begin_ms != 0) {
        const uint32_t d = now - t_gap_begin_ms;
        t_gap_begin_ms = 0;
        if (d >= 100) {
            ++t_gap_cnt;
            t_gap_ms += d;
            ESP_LOGW(TAG, "[T] gap_end dur=%u ms", d);
        }
    }

    if (now - t_tick_ms.load(std::memory_order_relaxed) < 1000) return;
    t_tick_ms.store(now, std::memory_order_relaxed);
    ESP_LOGI(TAG,
             "[T] tick rxn=%u rxb=%u rxslow=%u rxmax=%u drop=%u qn=%u qkb=%u "
             "gapn=%u gapms=%u gapmax=%u txmax=%u txslow=%u heapint=%u "
             "heapps=%u lfb=%u",
             (unsigned)t_rx_pkts.exchange(0), (unsigned)t_rx_bytes.exchange(0),
             (unsigned)t_rx_slow.exchange(0),
             (unsigned)t_rx_max_dt.exchange(0),
             (unsigned)t_drop_bytes.exchange(0), (unsigned)chunks,
             (unsigned)(bytes / 1024), (unsigned)t_gap_cnt.exchange(0),
             (unsigned)t_gap_ms.exchange(0), (unsigned)t_gap_max.load(),
             (unsigned)t_tx_max_us.exchange(0), (unsigned)t_tx_slow.exchange(0),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
}
#endif

void audio_playback_task(void* /*arg*/) {
    while (true) {
        if (s_stop_requested.load(std::memory_order_acquire)) {
            s_audio_task_running.store(false, std::memory_order_release);
            vTaskDelete(nullptr);
            return;
        }
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
        for (size_t offset = 0; offset < chunk.pcm.size();) {
            if (chunk.generation !=
                s_generation.load(std::memory_order_acquire)) {
                break;
            }
            const size_t remaining = chunk.pcm.size() - offset;
            const size_t samples =
                remaining < kPlaybackSliceSamples
                    ? remaining
                    : kPlaybackSliceSamples;
            while (!audio.PushPcmToPlaybackQueue(
                chunk.pcm.data() + offset, samples)) {
                if (s_stop_requested.load(std::memory_order_acquire) ||
                    chunk.generation !=
                        s_generation.load(std::memory_order_acquire)) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(2));
            }
            if (s_stop_requested.load(std::memory_order_acquire) ||
                chunk.generation !=
                    s_generation.load(std::memory_order_acquire)) {
                break;
            }
            offset += samples;
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
#if ORAL_TRACE
            t_drop_bytes += static_cast<uint32_t>(
                s_audio_chunks.front().pcm.size() * sizeof(int16_t));
#endif
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
    std::lock_guard<std::recursive_mutex> lock(s_ws_mutex);
    if (s_ws) {
        s_ws->Close();
        s_ws.reset();
    }
    s_ws_ready.store(false, std::memory_order_release);
}

void start_session();

void clear_tx_queue() {
    std::lock_guard<std::mutex> lock(s_tx_mutex);
    s_tx_chunks.clear();
}

// 采集侧调用：永不阻塞；队列满时丢最旧的一帧，保证读麦节拍不被网络拖住。
void queue_tx_frame(const int16_t* pcm, size_t samples) {
    if (pcm == nullptr || samples == 0) return;
    std::lock_guard<std::mutex> lock(s_tx_mutex);
    while (s_tx_chunks.size() >= kMaxTxChunks) {
        s_tx_chunks.pop_front();
        s_tx_dropped.fetch_add(1, std::memory_order_relaxed);
    }
    s_tx_chunks.emplace_back(pcm, pcm + samples);
}

// 独立的 WS 发送任务：Send 再慢也只堵自己，不堵麦克风。
void tx_task(void* /*arg*/) {
    s_tx_running.store(true, std::memory_order_release);
    while (!s_tx_stop.load(std::memory_order_acquire)) {
        std::vector<int16_t> frame;
        {
            std::lock_guard<std::mutex> lock(s_tx_mutex);
            if (!s_tx_chunks.empty()) {
                frame = std::move(s_tx_chunks.front());
                s_tx_chunks.pop_front();
            }
        }
        if (frame.empty()) {
            vTaskDelay(pdMS_TO_TICKS(4));
            continue;
        }
        std::lock_guard<std::recursive_mutex> lock(s_ws_mutex);
        if (!s_ws || !s_ws->IsConnected()) break;
#if ORAL_TRACE
        const int64_t t0 = esp_timer_get_time();
        const bool ok =
            s_ws->Send(frame.data(), frame.size() * sizeof(int16_t), true);
        const uint32_t us = static_cast<uint32_t>(esp_timer_get_time() - t0);
        if (us > t_tx_max_us.load(std::memory_order_relaxed)) {
            t_tx_max_us.store(us, std::memory_order_relaxed);
        }
        if (us >= 20000) {
            ++t_tx_slow;
            ESP_LOGW(TAG, "[T] tx_slow us=%u", us);
        }
        if (!ok) break;
#else
        if (!s_ws->Send(frame.data(), frame.size() * sizeof(int16_t), true)) {
            break;
        }
#endif
    }
    s_tx_stop.store(true, std::memory_order_release);
    s_tx_running.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}

void send_ws_text(const char* text) {
    std::lock_guard<std::recursive_mutex> lock(s_ws_mutex);
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
    s_last_audio_ms.store(now_ms(), std::memory_order_relaxed);
#if ORAL_TRACE
    {
        const uint32_t now = now_ms();
        const uint32_t last = t_last_rx_ms.exchange(now, std::memory_order_acq_rel);
        if (last != 0) {
            const uint32_t dt = now - last;
            if (dt > t_rx_max_dt.load(std::memory_order_relaxed)) {
                t_rx_max_dt.store(dt, std::memory_order_relaxed);
            }
            if (dt >= 120) {
                ++t_rx_slow;
                ESP_LOGW(TAG, "[T] rx_slow dt=%u ms len=%u", dt, (unsigned)len);
            }
        }
        ++t_rx_pkts;
        t_rx_bytes += static_cast<uint32_t>(len);
        t_last_audio_ms.store(now, std::memory_order_relaxed);
        const uint32_t rstart = t_rstart_ms.load(std::memory_order_relaxed);
        if (rstart != 0 && t_first_audio_ms.load(std::memory_order_relaxed) == 0) {
            t_first_audio_ms.store(now, std::memory_order_relaxed);
            ESP_LOGI(TAG, "[T] ev=first_audio dt=%u", now - rstart);
        }
    }
#endif
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
        // relay.ready 里带 model/voice，打出来才能确认服务端真的换了模型
        // （allowedModels 不匹配时 relay 会静默回落到默认模型）。
        {
            const cJSON* model_item = cJSON_GetObjectItem(root, "model");
            const cJSON* voice_item = cJSON_GetObjectItem(root, "voice");
            ESP_LOGI(TAG, "relay ready: model=%s voice=%s",
                     cJSON_IsString(model_item) ? model_item->valuestring : "?",
                     cJSON_IsString(voice_item) ? voice_item->valuestring : "?");
        }
#if ORAL_TRACE
        ESP_LOGI(TAG, "[T] ev=ready up=%u", now_ms());
#endif
        s_drop_incoming_audio.store(false, std::memory_order_release);
        s_response_done_pending.store(false, std::memory_order_release);
        s_user_transcript_closed.store(false, std::memory_order_release);
        post_ui(session, State::Listening, "聆听中…", BubbleRole::Assistant, nullptr);
    } else if (std::strcmp(type, "relay.speech_stopped") == 0) {
        s_mic_enabled.store(false, std::memory_order_release);
#if ORAL_TRACE
        ESP_LOGI(TAG, "[T] ev=speech_stopped up=%u", now_ms());
#endif
        // 新一句话开始：上一轮的用户气泡必须封口，否则本轮增量会拼进旧气泡。
        s_user_transcript_closed.store(false, std::memory_order_release);
        s_user_streamed.store(false, std::memory_order_release);
        // 从“用户说完”开始计看门狗：上游若迟迟不给 response.start
        // （例如直接报 invalid_value），麦克风会一直关着，必须能自愈。
        {
            const uint32_t now = now_ms();
            s_last_audio_ms.store(now, std::memory_order_relaxed);
            s_rstart_ms.store(now, std::memory_order_relaxed);
        }
        // 打断时已经建过占位气泡就复用，避免同一轮出现两个空气泡
        if (!s_user_pending.exchange(true, std::memory_order_acq_rel)) {
            post_ui(session, State::Answering, "思考中…", BubbleRole::User, "…");
        }
        s_user_pending_ms.store(now_ms(), std::memory_order_release);
    } else if (std::strcmp(type, "relay.user_transcript.delta") == 0) {
        const cJSON* delta = cJSON_GetObjectItem(root, "delta");
        if (cJSON_IsString(delta) && delta->valuestring != nullptr &&
            !s_user_transcript_closed.load(std::memory_order_acquire)) {
            s_user_streamed.store(true, std::memory_order_release);
            if (s_user_pending.exchange(false, std::memory_order_acq_rel)) {
                // 第一个增量直接把占位符替换掉，保持气泡顺序
                post_ui(session, State::Listening, "聆听中…", BubbleRole::User,
                        delta->valuestring, false, false, false, false, true);
            } else {
                post_ui(session, State::Listening, "聆听中…", BubbleRole::User,
                        delta->valuestring, true);
            }
        }
    } else if (std::strcmp(type, "relay.user_transcript.done") == 0) {
        const cJSON* text = cJSON_GetObjectItem(root, "text");
        s_user_transcript_closed.store(true, std::memory_order_release);
        if (s_user_pending.exchange(false, std::memory_order_acq_rel)) {
            // 没有增量、只给了整句：回填占位气泡
            if (cJSON_IsString(text) && text->valuestring != nullptr &&
                text->valuestring[0] != '\0') {
                post_ui(session, State::Answering, "思考中…", BubbleRole::User,
                        text->valuestring, false, false, false, false, true);
            }
            s_user_streamed.store(false, std::memory_order_release);
        } else if (s_user_streamed.exchange(false, std::memory_order_acq_rel)) {
            // 已经按增量建过气泡，done 只更新状态，避免整句重复插入。
            post_ui(session, State::Answering, "思考中…",
                    BubbleRole::Assistant, nullptr, false, false, false, true);
        } else if (cJSON_IsString(text) && text->valuestring != nullptr &&
                   text->valuestring[0] != '\0') {
            post_ui(session, State::Answering, "思考中…", BubbleRole::User, text->valuestring);
        }
    } else if (std::strcmp(type, "relay.response.start") == 0) {
        s_barge_detection_requested.store(true, std::memory_order_release);
        s_generation.fetch_add(1, std::memory_order_acq_rel);
        s_drop_incoming_audio.store(false, std::memory_order_release);
        s_response_done_pending.store(false, std::memory_order_release);
        s_mic_enabled.store(false, std::memory_order_release);
        s_last_text_seq.store(0, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(s_text_mutex);
            s_stream_text.clear();
            s_response_text.clear();
            s_stream_text_ms = 0;
        }
        post_ui(session, State::Answering, "回答中…", BubbleRole::Assistant,
                nullptr, false, true);
        // 从本轮起点重新计时，避免上一轮的旧时间戳立刻触发 watchdog。
        {
            const uint32_t now = now_ms();
            s_last_audio_ms.store(now, std::memory_order_relaxed);
            s_rstart_ms.store(now, std::memory_order_relaxed);
        }
#if ORAL_TRACE
        {
            const uint32_t now = now_ms();
            const bool was_open = t_turn_open.exchange(true);
            t_rstart_ms.store(now, std::memory_order_relaxed);
            t_first_audio_ms.store(0, std::memory_order_relaxed);
            t_rx_pkts.store(0);
            t_rx_bytes.store(0);
            t_rx_max_dt.store(0);
            t_rx_slow.store(0);
            t_drop_bytes.store(0);
            t_gap_cnt.store(0);
            t_gap_ms.store(0);
            t_gap_max.store(0);
            const uint32_t prev_done = t_rdone_ms.load(std::memory_order_relaxed);
            ESP_LOGI(TAG, "[T] ev=rstart up=%u prev_done_ago=%u ms open=%d",
                     now, prev_done == 0 ? 0 : now - prev_done,
                     was_open ? 1 : 0);
        }
#endif
    } else if (std::strcmp(type, "relay.text.delta") == 0) {
        const cJSON* delta = cJSON_GetObjectItem(root, "delta");
        if (cJSON_IsString(delta) && delta->valuestring != nullptr) {
            const cJSON* seq_item = cJSON_GetObjectItem(root, "seq");
            if (cJSON_IsNumber(seq_item) && seq_item->valuedouble > 0) {
                const uint32_t seq =
                    static_cast<uint32_t>(seq_item->valuedouble);
                uint32_t last = s_last_text_seq.load(std::memory_order_acquire);
                if (seq <= last) {
                    cJSON_Delete(root);
                    return;
                }
                s_last_text_seq.store(seq, std::memory_order_release);
            }
            bool has_visible = false;
            for (const char* p = delta->valuestring; *p != '\0'; ++p) {
                if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
                    has_visible = true;
                    break;
                }
            }
            if (!has_visible) {
                cJSON_Delete(root);
                return;
            }
            {
                std::lock_guard<std::mutex> lock(s_text_mutex);
                s_stream_text += delta->valuestring;
                s_response_text += delta->valuestring;
                if (s_stream_text.size() > kMaxTextBytes) {
                    s_stream_text.erase(0, s_stream_text.size() - kMaxTextBytes);
                }
                if (s_response_text.size() > kMaxTextBytes * 2) {
                    s_response_text.erase(
                        0, s_response_text.size() - kMaxTextBytes * 2);
                }
                if (s_stream_text_ms == 0) s_stream_text_ms = now_ms();
            }
            // 一次回答只用一个气泡：句末/长度/时间只决定“什么时候把缓冲
            // 刷到界面”，不再切气泡。切得太碎会被 12 条消息上限裁掉开头，
            // 长回答看起来就是顺序错乱。
            const bool sentence_end = ends_sentence(delta->valuestring);
            if (sentence_end || s_stream_text.size() >= 96 ||
                now_ms() - s_stream_text_ms >= 160) {
                flush_stream_text(session, false);
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
            // 本轮结束还没等到转写：删掉用户气泡里的 "…" 占位，
            // 否则屏幕上会一直留着一个空白的用户气泡。
            if (s_user_pending.exchange(false, std::memory_order_acq_rel)) {
                post_ui(session, State::Answering, "回答中…", BubbleRole::User,
                        "", false, false, false, false, true);
            }
            s_mic_enabled.store(false, std::memory_order_release);
            s_response_done_pending.store(true, std::memory_order_release);
            s_response_done_at_us.store(esp_timer_get_time(),
                                        std::memory_order_release);
            if (response_says_goodbye()) {
                s_end_after_playback.store(true, std::memory_order_release);
                // 主动结束会话：避免 close 时 OnDisconnected 触发自动重连。
                s_user_exit.store(true, std::memory_order_release);
            }
#if ORAL_TRACE
            {
                const uint32_t now = now_ms();
                const uint32_t rstart = t_rstart_ms.load(std::memory_order_relaxed);
                const uint32_t last_audio =
                    t_last_audio_ms.load(std::memory_order_relaxed);
                size_t chunks = 0;
                size_t bytes = 0;
                audio_queue_stats(chunks, bytes);
                t_rdone_ms.store(now, std::memory_order_relaxed);
                t_turn_open.store(false);
                ESP_LOGI(TAG,
                         "[T] ev=rdone up=%u since_rstart=%u last_audio_ago=%u "
                         "qn=%u qkb=%u pending=%d rxn=%u rxb=%u rxmax=%u "
                         "drop=%u gapn=%u gapms=%u gapmax=%u",
                         now, rstart == 0 ? 0 : now - rstart,
                         last_audio == 0 ? 0 : now - last_audio,
                         (unsigned)chunks, (unsigned)(bytes / 1024),
                         Application::GetInstance()
                                 .GetAudioService()
                                 .IsPlaybackPending()
                             ? 1
                             : 0,
                         (unsigned)t_rx_pkts.load(), (unsigned)t_rx_bytes.load(),
                         (unsigned)t_rx_max_dt.load(),
                         (unsigned)t_drop_bytes.load(),
                         (unsigned)t_gap_cnt.load(), (unsigned)t_gap_ms.load(),
                         (unsigned)t_gap_max.load());
            }
#endif
        }
    } else if (std::strcmp(type, "relay.interrupted") == 0) {
        s_drop_incoming_audio.store(true, std::memory_order_release);
        s_response_done_pending.store(false, std::memory_order_release);
        s_end_after_playback.store(false, std::memory_order_release);
        s_mic_enabled.store(true, std::memory_order_release);
        s_user_transcript_closed.store(false, std::memory_order_release);
#if ORAL_TRACE
        ESP_LOGI(TAG, "[T] ev=interrupted up=%u", now_ms());
        t_turn_open.store(false);
#endif
        // 服务端打断：喇叭刚停，留一小段静音窗避免回采自答
        s_uplink_mute_until_ms.store(now_ms() + 400, std::memory_order_relaxed);
        clear_tx_queue();
        // 打断后进入新一轮聆听：计时从此刻重新开始。
        {
            const uint32_t now = now_ms();
            s_last_audio_ms.store(now, std::memory_order_relaxed);
            s_rstart_ms.store(now, std::memory_order_relaxed);
        }
        post_ui(session, State::Listening, "聆听中…", BubbleRole::Assistant, nullptr);
    } else if (std::strcmp(type, "relay.error") == 0) {
        const cJSON* error = cJSON_GetObjectItem(root, "error");
        const char* detail = cJSON_IsString(error) ? error->valuestring : "服务错误";
        ESP_LOGE(TAG, "relay error: %s", detail);
        char message[256];
        std::snprintf(message, sizeof(message), "错误：%s", detail);
        post_ui(session, State::Error, message, BubbleRole::Assistant, nullptr);
        // 上游报错后这一轮已经废了：立刻恢复聆听，不能把麦克风一直关着
        // （否则界面停在“回答中”，用户说什么都没反应）。
        s_response_done_pending.store(false, std::memory_order_release);
        s_end_after_playback.store(false, std::memory_order_release);
        s_drop_incoming_audio.store(true, std::memory_order_release);
        stop_barge_in_detection();
        {
            const uint32_t now = now_ms();
            s_last_audio_ms.store(now, std::memory_order_relaxed);
            s_rstart_ms.store(now, std::memory_order_relaxed);
        }
        s_mic_enabled.store(true, std::memory_order_release);
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
    std::lock_guard<std::recursive_mutex> lock(s_ws_mutex);
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
#if ORAL_TRACE
        trace_tick();
#endif
        {
            // 卡顿/卡死自检：单次循环耗时超过 1.5s 说明有地方阻塞了
            // （读麦、网络或 AFE），打一条便于定位。
            static uint32_t prev_iter_ms = 0;
            const uint32_t iter_now = now_ms();
            if (prev_iter_ms != 0 && iter_now - prev_iter_ms > 1500) {
                ESP_LOGW(TAG, "loop stall %u ms",
                         (unsigned)(iter_now - prev_iter_ms));
            }
            prev_iter_ms = iter_now;
        }
        if (s_barge_detection_requested.exchange(false,
                                                std::memory_order_acq_rel)) {
            start_barge_in_detection();
        }
        // 占位气泡超时清理：一直没等到转写就删掉 "…"，避免屏幕上留空气泡
        if (s_user_pending.load(std::memory_order_acquire)) {
            const uint32_t since =
                s_user_pending_ms.load(std::memory_order_relaxed);
            if (since != 0 && now_ms() - since > 3000) {
                s_user_pending.store(false, std::memory_order_release);
                s_user_pending_ms.store(0, std::memory_order_release);
                post_ui(session, State::Listening, "聆听中…", BubbleRole::User,
                        "", false, false, false, false, true);
            }
        }
        if (s_interrupt_requested.exchange(false, std::memory_order_acq_rel)) {
            auto& audio = Application::GetInstance().GetAudioService();
            stop_barge_in_detection();
            s_drop_incoming_audio.store(true, std::memory_order_release);
            audio.ClearPlaybackQueue();
            clear_audio_queue();
            s_generation.fetch_add(1, std::memory_order_acq_rel);
            s_response_done_pending.store(false, std::memory_order_release);
            s_end_after_playback.store(false, std::memory_order_release);
            send_ws_json("{\"type\":\"interrupt\"}");
            send_ws_json("{\"type\":\"resume_mic\"}");
            // 与 OralScreen::Interrupt 同样处理：静音窗 + 丢掉已排队的回声帧
            s_uplink_mute_until_ms.store(now_ms() + 600, std::memory_order_relaxed);
            clear_tx_queue();
            s_mic_enabled.store(true, std::memory_order_release);
            post_ui(session, State::Listening, "聆听中…", BubbleRole::Assistant, nullptr);
#if ORAL_TRACE
            ESP_LOGI(TAG, "[T] ev=interrupt up=%u", now_ms());
            t_turn_open.store(false);
#endif
        }
        // 服务端长时间不给 response.done 时，客户端会永远停在“回答中”。
        // 最后一块音频后静默超过阈值，且没有排队/正在播放的音频，就当作本轮
        // 结束：打断 + 重新开麦，避免必须按电源键才能继续。
        if (!s_response_done_pending.load(std::memory_order_acquire) &&
            !s_mic_enabled.load(std::memory_order_acquire) &&
            s_ws_ready.load(std::memory_order_acquire)) {
            const uint32_t now = now_ms();
            const uint32_t last_audio =
                s_last_audio_ms.load(std::memory_order_relaxed);
            const uint32_t rstart = s_rstart_ms.load(std::memory_order_relaxed);
            // 刚开一轮（response.start 之后）必须先给服务端时间出首包，
            // 否则上一轮的旧时间戳会立刻误触发打断。
            const bool turn_old_enough = rstart != 0 && now - rstart >= kTurnIdleTimeoutMs;
            if (turn_old_enough && last_audio != 0 &&
                now - last_audio >= kTurnIdleTimeoutMs &&
                !audio.IsPlaybackPending() && !has_queued_audio()) {
                ESP_LOGW(TAG, "turn watchdog: %u ms silence, resume mic",
                         (unsigned)(now - last_audio));
                audio.ClearPlaybackQueue();
                clear_audio_queue();
                s_generation.fetch_add(1, std::memory_order_acq_rel);
                s_drop_incoming_audio.store(true, std::memory_order_release);
                s_end_after_playback.store(false, std::memory_order_release);
                stop_barge_in_detection();
                send_ws_json("{\"type\":\"interrupt\"}");
                send_ws_json("{\"type\":\"resume_mic\"}");
                s_mic_enabled.store(true, std::memory_order_release);
                post_ui(session, State::Listening, "聆听中…",
                        BubbleRole::Assistant, nullptr);
            }
        }
        if (s_response_done_pending.load(std::memory_order_acquire)) {
            if (audio.IsPlaybackPending() || has_queued_audio()) {
                playback_idle_tick = 0;
            } else {
                if (s_end_after_playback.load(std::memory_order_acquire)) {
                    break;
                }
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
#if ORAL_TRACE
                    ESP_LOGW(TAG, "[T] ev=resume_timeout8s up=%u", now_ms());
                    t_turn_open.store(false);
#endif
                } else if (now - playback_idle_tick >= pdMS_TO_TICKS(250)) {
                    stop_barge_in_detection();
                    s_response_done_pending.store(false, std::memory_order_release);
                    s_mic_enabled.store(true, std::memory_order_release);
                    // 刚播完的尾音可能还在扬声器里，留 150ms 静音窗
                    s_uplink_mute_until_ms.store(now_ms() + 150, std::memory_order_relaxed);
                    send_ws_json("{\"type\":\"resume_mic\"}");
                    post_ui(session, State::Listening, "聆听中…", BubbleRole::Assistant, nullptr);
#if ORAL_TRACE
                    {
                        const uint32_t up = now_ms();
                        const uint32_t done =
                            t_rdone_ms.load(std::memory_order_relaxed);
                        ESP_LOGI(TAG, "[T] ev=resume up=%u since_rdone=%u ms",
                                 up, done == 0 ? 0 : up - done);
                        t_turn_open.store(false);
                    }
#endif
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
        // 静音窗内照常读麦（避免 I2S 缓冲积旧数据），但不上传。
        if (now_ms() < s_uplink_mute_until_ms.load(std::memory_order_relaxed)) {
            continue;
        }
        // 只入队，不在这里 Send：网络阻塞不能拖慢读麦。
        queue_tx_frame(frame.data(), frame.size());
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
#if ORAL_TRACE
        ESP_LOGI(TAG, "[T] ev=mic_on up=%u", now_ms());
#endif
        clear_tx_queue();
        s_tx_stop.store(false, std::memory_order_release);
        if (xTaskCreate(tx_task, "oral_tx", 6144, nullptr, 5, nullptr) != pdPASS) {
            // 建不起发送任务：退化为原来同步发，至少不丢音频。
            s_tx_stop.store(true, std::memory_order_release);
            ESP_LOGE(TAG, "tx task create failed");
        }
        stream_microphone(session);
        s_mic_enabled.store(false, std::memory_order_release);
        s_tx_stop.store(true, std::memory_order_release);
        for (int i = 0; i < 30 && s_tx_running.load(std::memory_order_acquire);
             ++i) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        clear_tx_queue();
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
    if (s_end_after_playback.exchange(false, std::memory_order_acq_rel)) {
        // 说完“再见”不退出页面，也不继续收录音，停在等待唤醒状态。
        s_waiting_wake.store(true, std::memory_order_release);
        s_mic_enabled.store(false, std::memory_order_release);
        s_state.store(State::Idle, std::memory_order_release);
        lv_async_call(
            [](void*) {
                if (s_screen != nullptr && lv_screen_active() == s_screen) {
                    set_status_visual(State::Idle, "等待唤醒…");
                }
            },
            nullptr);
    }
    vTaskDelete(nullptr);
}

void CancelStartRetry() {
    if (s_start_retry_timer != nullptr) {
        lv_timer_delete(s_start_retry_timer);
        s_start_retry_timer = nullptr;
    }
    s_start_retries.store(0, std::memory_order_release);
}

void start_session() {
    if (s_worker_running.load(std::memory_order_acquire)) {
        // 旧 WebSocket 可能还在阻塞退出，只保留一个重试定时器，避免快速
        // 进出页面时每次 Create 都堆一个 100ms timer。
        if (s_start_retry_timer == nullptr &&
            s_start_retries.fetch_add(1, std::memory_order_acq_rel) < 150) {
            const uint32_t session = s_session.load(std::memory_order_acquire);
            s_start_retry_timer = lv_timer_create(
                [](lv_timer_t* timer) {
                    const uint32_t expected_session = static_cast<uint32_t>(
                        reinterpret_cast<uintptr_t>(
                            lv_timer_get_user_data(timer)));
                    s_start_retry_timer = nullptr;
                    lv_timer_delete(timer);
                    if (session_alive(expected_session)) {
                        start_session();
                    }
                },
                100,
                reinterpret_cast<void*>(static_cast<uintptr_t>(session)));
        } else {
            if (s_start_retry_timer == nullptr) {
                s_start_retries.store(0, std::memory_order_release);
                post_ui(s_session.load(), State::Error, "口语会话切换超时",
                        BubbleRole::Assistant, nullptr);
            }
        }
        return;
    }
    s_start_retries.store(0, std::memory_order_release);
    s_waiting_wake.store(false, std::memory_order_release);
    s_state.store(State::Connecting, std::memory_order_release);
    s_session.fetch_add(1, std::memory_order_acq_rel);
    s_stop_requested.store(false, std::memory_order_release);
    s_interrupt_requested.store(false, std::memory_order_release);
    s_ws_ready.store(false, std::memory_order_release);
    s_response_done_pending.store(false, std::memory_order_release);
    s_response_done_at_us.store(0, std::memory_order_release);
    s_barge_detection_requested.store(false, std::memory_order_release);
    s_mic_enabled.store(false, std::memory_order_release);
    s_last_done_response_id.store(0, std::memory_order_release);
    s_last_text_seq.store(0, std::memory_order_release);
    s_user_streamed.store(false, std::memory_order_release);
    s_user_pending.store(false, std::memory_order_release);
    s_end_after_playback.store(false, std::memory_order_release);
    s_drop_incoming_audio.store(false, std::memory_order_release);
    s_text = nullptr;
    s_text_bubble = nullptr;
    s_assistant_row = nullptr;
    s_user_text_label = nullptr;
    s_user_text_bubble = nullptr;
    s_user_row = nullptr;
    clear_audio_queue();
    clear_tx_queue();
    s_tx_stop.store(true, std::memory_order_release);
    s_tx_dropped.store(0, std::memory_order_relaxed);
    s_last_audio_ms.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(s_text_mutex);
        s_answer.clear();
        s_user_text.clear();
        s_response_text.clear();
        s_resample_position = 0;
    }
    s_worker_running.store(true, std::memory_order_release);
    // 这个任务里同时做 DNS 解析、TLS 握手和 WebSocket 升级，
    // 8KB 栈在弱网/证书链较长时余量太小，加到 12KB。
    if (xTaskCreate(worker_task, "oral_ws", 12288, nullptr, 4, &s_worker_task) != pdPASS) {
        s_worker_running.store(false, std::memory_order_release);
        s_worker_task = nullptr;
        post_ui(s_session.load(), State::Error, "启动口语会话失败", BubbleRole::Assistant, nullptr);
    }
}

void stop_session() {
    CancelStartRetry();
    s_user_exit.store(true, std::memory_order_release);
    s_waiting_wake.store(false, std::memory_order_release);
    s_stop_requested.store(true, std::memory_order_release);
    s_reconnect_requested.store(false, std::memory_order_release);
    s_interrupt_requested.store(false, std::memory_order_release);
    s_response_done_pending.store(false, std::memory_order_release);
    s_barge_detection_requested.store(false, std::memory_order_release);
    s_mic_enabled.store(false, std::memory_order_release);
    // WebSocket Send/Close 可能阻塞；LVGL 线程只置退出标志，由 worker 清理。
    s_generation.fetch_add(1, std::memory_order_acq_rel);
    // 立即丢掉未播完的旧语音，避免退出后继续出声、拖慢其他页面。
    clear_audio_queue();
    clear_tx_queue();
    s_tx_stop.store(true, std::memory_order_release);
    Application::GetInstance().GetAudioService().ClearPlaybackQueue();
}

void on_screen_clicked(lv_event_t* /*event*/) {
    // worker 没在跑 = 未连接 / 连不上 / 已断开 / 说完再见，
    // 这时点屏幕都应该重试。之前只认 s_waiting_wake，导致连接失败 3 次后
    // 页面“点不动”，只能退出重进。
    if (s_worker_running.load(std::memory_order_acquire)) {
        return;
    }
    s_waiting_wake.store(false, std::memory_order_release);
    start_session();
}

void exit_to_return_screen() {
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

void on_back(lv_event_t* /*event*/) {
    exit_to_return_screen();
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
    s_status_pill = nullptr;
    s_status_dot = nullptr;
    s_footer = nullptr;
    s_hint = nullptr;
    for (auto*& bar : s_wave_bars) bar = nullptr;
    if (s_wave_timer != nullptr) {
        lv_timer_delete(s_wave_timer);
        s_wave_timer = nullptr;
    }
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
    s_waiting_wake.store(false, std::memory_order_release);

    lv_obj_t* screen = lv_obj_create(nullptr);
    s_screen = screen;
    screen_strip_obj_chrome(screen);
    lv_obj_set_size(screen, kPanelW, kPanelH);
    // 底色：整页竖向渐变，替代原来的纯黑平铺。
    lv_obj_set_style_bg_color(screen, lv_color_hex(kColorBgTop), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(screen, lv_color_hex(kColorBgBottom), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    // 头部不再是独立色带：同色轻微压暗，保证滚动时文字不糊在气泡上。
    lv_obj_t* header = lv_obj_create(screen);
    screen_strip_obj_chrome(header);
    lv_obj_add_flag(header, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(header, kPanelW, kHeaderH);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(kColorBgTop), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(header, 242, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(header, 0, LV_PART_MAIN);

    // 1px 光缝：中点最亮、两端融进底色。LVGL 只有两色渐变，用左右两段拼。
    lv_obj_t* seam_left = lv_obj_create(header);
    screen_strip_obj_chrome(seam_left);
    lv_obj_set_size(seam_left, kPanelW / 2, 1);
    lv_obj_align(seam_left, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(seam_left, lv_color_hex(kColorBgTop), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(seam_left, lv_color_hex(kColorAccent), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(seam_left, LV_GRAD_DIR_HOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(seam_left, 120, LV_PART_MAIN);
    screen_make_input_passive(seam_left);

    lv_obj_t* seam_right = lv_obj_create(header);
    screen_strip_obj_chrome(seam_right);
    lv_obj_set_size(seam_right, kPanelW / 2, 1);
    lv_obj_align(seam_right, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(seam_right, lv_color_hex(kColorAccent), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(seam_right, lv_color_hex(kColorBgTop), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(seam_right, LV_GRAD_DIR_HOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(seam_right, 120, LV_PART_MAIN);
    screen_make_input_passive(seam_right);

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

    lv_obj_set_style_text_letter_space(s_title, 1, LV_PART_MAIN);

    // 状态胶囊：发光圆点 + 文案，颜色跟 State 走（聆听=主色、回答=蓝、失败=红）。
    s_status_pill = lv_obj_create(header);
    screen_strip_obj_chrome(s_status_pill);
    lv_obj_remove_flag(s_status_pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_status_pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_status_pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_status_pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_status_pill, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_status_pill, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(s_status_pill, 13, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s_status_pill, 7, LV_PART_MAIN);
    lv_obj_set_style_radius(s_status_pill, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_status_pill, lv_color_hex(kColorAccent), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_status_pill, 26, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_status_pill, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_status_pill, lv_color_hex(kColorAccent), LV_PART_MAIN);
    lv_obj_set_style_border_opa(s_status_pill, 70, LV_PART_MAIN);
    lv_obj_align_to(s_status_pill, s_title, LV_ALIGN_OUT_RIGHT_MID, 14, 0);

    s_status_dot = lv_obj_create(s_status_pill);
    screen_strip_obj_chrome(s_status_dot);
    lv_obj_remove_flag(s_status_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_status_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_status_dot, 9, 9);
    lv_obj_set_style_radius(s_status_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_status_dot, lv_color_hex(kColorAccent), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_status_dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_status_dot, 10, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(s_status_dot, lv_color_hex(kColorAccent), LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(s_status_dot, 120, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_status_dot, 0, LV_PART_MAIN);

    s_status = lv_label_create(s_status_pill);
    lv_label_set_text(s_status, "连接中…");
    lv_obj_set_style_text_font(s_status, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_status, lv_color_hex(kColorStatusConnecting), LV_PART_MAIN);
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
    // 列表透明，让整页渐变透上来；原来是整块不透明的深色，把底色全挡住了。
    lv_obj_set_style_bg_opa(s_chat, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_flag(s_chat, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_chat, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_chat, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_event_cb(s_chat, on_screen_clicked, LV_EVENT_CLICKED, nullptr);

    // 底部聆听区：波柱 + 一句提示，固定压在列表下方。
    s_footer = lv_obj_create(screen);
    screen_strip_obj_chrome(s_footer);
    lv_obj_remove_flag(s_footer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_footer, kPanelW, kFooterH);
    lv_obj_set_pos(s_footer, 0, kPanelH - kFooterH);
    lv_obj_set_style_bg_opa(s_footer, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_footer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_footer, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_footer, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(s_footer, 22, LV_PART_MAIN);
    lv_obj_add_flag(s_footer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_footer, on_screen_clicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* wave_row = lv_obj_create(s_footer);
    screen_strip_obj_chrome(wave_row);
    lv_obj_remove_flag(wave_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(wave_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(wave_row, 220, 40);
    lv_obj_set_style_bg_opa(wave_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(wave_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wave_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(wave_row, 6, LV_PART_MAIN);
    for (int i = 0; i < 5; ++i) {
        lv_obj_t* bar = lv_obj_create(wave_row);
        screen_strip_obj_chrome(bar);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(bar, 4, 6);
        lv_obj_set_style_radius(bar, 2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, lv_color_hex(kColorAccent), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN);
        s_wave_bars[i] = bar;
    }

    s_hint = lv_label_create(s_footer);
    lv_label_set_text(s_hint, "正在连接…");
    lv_obj_set_style_text_font(s_hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0x7C8698), LV_PART_MAIN);
    screen_make_input_passive(s_hint);

    s_wave_timer = lv_timer_create(wave_timer_cb, 90, nullptr);
    set_status_visual(State::Connecting, "连接中…");

    lv_obj_add_event_cb(screen, on_screen_unloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);
    start_session();
    return screen;
}

void OralScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: oral_screen");
        Application::GetInstance().SetVoiceUiDesired(false);
        // 口语是实时双向语音，WiFi 省电会让下行音频出现周期性卡顿。
        Board::GetInstance().SetPowerSaveMode(false);
        // 口语会话期间不能被首页/待机的空闲计时判为“无操作”而自动关机。
        IdlePower_Detach(IdlePowerSession::Home);
        // 学伴的 TTS 可能还在播放队列里，进口语前必须清掉，否则两路声音叠加。
        Application::GetInstance().GetAudioService().ClearPlaybackQueue();
        clear_audio_queue();
        return;
    }
    ESP_LOGI(TAG, "unload: oral_screen");
    stop_session();
    Board::GetInstance().SetPowerSaveMode(true);
    // 交还空闲计时：从离开口语页重新开始累计。
    IdlePower_Attach(IdlePowerSession::Home, /*reset_activity=*/true);
}

bool OralScreen::IsActive() {
    return s_screen != nullptr && lv_screen_active() == s_screen;
}

void OralScreen::Interrupt() {
    if (!IsActive()) return;
    if (!s_worker_running.load(std::memory_order_acquire)) {
        start_session();
        return;
    }
    auto& audio = Application::GetInstance().GetAudioService();
    s_drop_incoming_audio.store(true, std::memory_order_release);
    audio.ClearPlaybackQueue();
    clear_audio_queue();
    // 打断瞬间扬声器还有余音：上行静音 300ms（覆盖 I2S DMA 余量 + 尾音），
    // 太大会把用户打断后说的头几个字一起吞掉。
    s_uplink_mute_until_ms.store(now_ms() + 300, std::memory_order_relaxed);
    clear_tx_queue();
    // 立刻占一个用户气泡：屏幕马上有反馈，不用等模型 VAD + 转写。
    s_user_pending.store(true, std::memory_order_release);
    s_user_pending_ms.store(now_ms(), std::memory_order_release);
    post_ui(s_session.load(), State::Listening, "聆听中…", BubbleRole::User, "…");
    s_generation.fetch_add(1, std::memory_order_acq_rel);
    s_response_done_pending.store(false, std::memory_order_release);
    s_end_after_playback.store(false, std::memory_order_release);
    s_interrupt_requested.store(true, std::memory_order_release);
    s_mic_enabled.store(true, std::memory_order_release);
    post_ui(s_session.load(), State::Listening, "聆听中…", BubbleRole::Assistant, nullptr);
}

void OralScreen::RequestInterrupt() {
    s_interrupt_requested.store(true, std::memory_order_release);
}
