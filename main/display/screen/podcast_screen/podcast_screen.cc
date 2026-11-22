#include "podcast_screen.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "application.h"
#include "chat_screen/chat_screen.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "home_screen/home_screen.h"
#include "i18n.h"
#include "podcast_client.h"
#include "podcast_player.h"
#include "podcast_types.h"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr const char* TAG = "PodcastScreen";
constexpr int32_t kPanelSize = 720;
constexpr int32_t kHeaderHeight = 84;
constexpr int32_t kListX = 24;
constexpr int32_t kListY = 102;
constexpr int32_t kListWidth = 672;
constexpr int32_t kListHeight = 396;
constexpr uint32_t kColorBg = 0x0B1020;
constexpr uint32_t kColorPanel = 0x111827;
constexpr uint32_t kColorCard = 0x182033;
constexpr uint32_t kColorCardPressed = 0x24314D;
constexpr uint32_t kColorAccent = 0x60A5FA;
constexpr uint32_t kColorText = 0xF8FAFC;
constexpr uint32_t kColorSubtle = 0x94A3B8;

struct PodcastUi {
    lv_obj_t* screen = nullptr;
    lv_obj_t* list = nullptr;
    lv_obj_t* status = nullptr;
    lv_obj_t* pause_btn = nullptr;
    lv_obj_t* pause_label = nullptr;
    lv_obj_t* stop_btn = nullptr;
    lv_obj_t* progress = nullptr;
    lv_obj_t* time_label = nullptr;
    lv_obj_t* speed_btn = nullptr;
    lv_obj_t* speed_label = nullptr;
    lv_timer_t* progress_timer = nullptr;
};

struct LoadResult {
    uint32_t session = 0;
    PodcastList items;
    std::string error;
};

struct ResolveResult {
    uint32_t session = 0;
    uint32_t request_generation = 0;
    std::string podcast_id;
    std::string title;
    std::string error;
};

struct PlayerUiEvent {
    uint32_t session = 0;
    PodcastPlayerEvent event = PodcastPlayerEvent::Error;
    std::string detail;
};

PodcastUi s_ui;
lv_obj_t* s_bound_screen = nullptr;
lv_obj_t* s_return_screen = nullptr;
PodcastList s_items;
// 上次成功拉到的节目列表：再次进入先本地渲染，后台只做刷新。
PodcastList s_cached_items;

constexpr const char* kListCachePath = "/sdcard/system/podcast_list.txt";
std::atomic<bool> s_url_prefetching{false};

// 进入列表时后台预取前两集的签名地址，点击时直接命中客户端的播放地址缓存。
void StartPlayUrlPrefetch() {
    std::vector<std::string> ids;
    for (const auto& item : s_items) {
        if (ids.size() >= 6) {
            break;
        }
        ids.push_back(item.id);
    }
    if (ids.empty()) {
        return;
    }
    if (s_url_prefetching.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    auto* context = new std::vector<std::string>(std::move(ids));
    if (xTaskCreate(
            [](void* arg) {
                auto* pending =
                    static_cast<std::vector<std::string>*>(arg);
                for (const auto& id : *pending) {
                    std::string error;
                    PodcastClient::ResolvePlayUrl(id, error);
                }
                delete pending;
                s_url_prefetching.store(false, std::memory_order_release);
                vTaskDelete(nullptr);
            },
            "podcast_url_prefetch", 6144, context, 4, nullptr) != pdPASS) {
        delete context;
        s_url_prefetching.store(false, std::memory_order_release);
    }
}

void SaveListCache() {
    FILE* file = std::fopen(kListCachePath, "w");
    if (file == nullptr) {
        return;
    }
    for (const auto& item : s_items) {
        std::fprintf(file, "%s|%s|%d|%d\n", item.id.c_str(),
                     item.title.c_str(), item.duration_seconds,
                     item.completed ? 1 : 0);
    }
    std::fclose(file);
}

// 列表落盘后，冷启动第一次进入也能立刻看到节目。
void LoadListCache() {
    if (!s_cached_items.empty()) {
        return;
    }
    FILE* file = std::fopen(kListCachePath, "r");
    if (file == nullptr) {
        return;
    }
    PodcastList items;
    char line[512];
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        std::string text(line);
        while (!text.empty() &&
               (text.back() == '\n' || text.back() == '\r')) {
            text.pop_back();
        }
        const size_t first = text.find('|');
        if (first == std::string::npos) {
            continue;
        }
        const size_t second = text.find('|', first + 1);
        if (second == std::string::npos) {
            continue;
        }
        const size_t third = text.find('|', second + 1);
        if (third == std::string::npos) {
            continue;
        }
        PodcastItem item;
        item.id = text.substr(0, first);
        item.title = text.substr(first + 1, second - first - 1);
        item.duration_seconds = std::atoi(
            text.substr(second + 1, third - second - 1).c_str());
        item.completed = text.substr(third + 1) == "1";
        if (!item.id.empty()) {
            items.push_back(std::move(item));
        }
    }
    std::fclose(file);
    s_cached_items = std::move(items);
}
std::string s_current_title;
std::atomic<bool> s_screen_active{false};
std::atomic<bool> s_loading{false};
std::atomic<uint32_t> s_session{0};
std::atomic<uint32_t> s_request_generation{0};
std::atomic<bool> s_preparing_play{false};
bool s_playing = false;
int s_progress_max = 1;
bool s_paused = false;

void SetStatusDirect(const char* text);

uint32_t CurrentSession() {
    return s_session.load(std::memory_order_relaxed);
}

bool IsCurrentSession(uint32_t session) {
    return s_screen_active.load(std::memory_order_relaxed) &&
           session == CurrentSession();
}

void FormatDuration(int seconds, char* buffer, size_t size) {
    if (seconds <= 0) {
        std::snprintf(buffer, size, "--:--");
        return;
    }
    const int hours = seconds / 3600;
    const int minutes = (seconds % 3600) / 60;
    const int secs = seconds % 60;
    if (hours > 0) {
        std::snprintf(buffer, size, "%d:%02d:%02d", hours, minutes, secs);
    } else {
        std::snprintf(buffer, size, "%d:%02d", minutes, secs);
    }
}

void UpdateControls(bool visible, bool playing) {
    if (s_ui.pause_btn == nullptr || s_ui.stop_btn == nullptr ||
        s_ui.pause_label == nullptr) {
        return;
    }
    lv_obj_t* extra[] = {s_ui.progress, s_ui.time_label, s_ui.speed_btn};
    if (visible) {
        lv_obj_remove_flag(s_ui.pause_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_ui.stop_btn, LV_OBJ_FLAG_HIDDEN);
        for (lv_obj_t* obj : extra) {
            if (obj != nullptr) {
                lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
            }
        }
    } else {
        lv_obj_add_flag(s_ui.pause_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ui.stop_btn, LV_OBJ_FLAG_HIDDEN);
        for (lv_obj_t* obj : extra) {
            if (obj != nullptr) {
                lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    lv_label_set_text(s_ui.pause_label,
                      I18n::T(playing ? "暂停" : "继续"));
}

void UpdateTimeLabel(int position, int duration) {
    if (s_ui.time_label == nullptr) {
        return;
    }
    char left[24] = {};
    char right[24] = {};
    if (position <= 0) {
        std::snprintf(left, sizeof(left), "0:00");
    } else {
        FormatDuration(position, left, sizeof(left));
    }
    FormatDuration(duration, right, sizeof(right));
    char text[56] = {};
    std::snprintf(text, sizeof(text), "%s / %s", left, right);
    lv_label_set_text(s_ui.time_label, text);
}

void OnProgressTimer(lv_timer_t* /*timer*/) {
    if (!s_screen_active.load(std::memory_order_relaxed) ||
        s_ui.progress == nullptr) {
        return;
    }
    if (lv_obj_has_state(s_ui.progress, LV_STATE_PRESSED)) {
        return;  // 用户正在拖动，不覆盖滑块
    }
    const int duration = PodcastPlayer::GetDurationSeconds();
    const int position = PodcastPlayer::GetPositionSeconds();
    if (duration > 0 && s_progress_max != duration) {
        // duration_seconds 为 0 的节目会在开播后由比特率估算出量程。
        s_progress_max = duration;
        lv_slider_set_range(s_ui.progress, 0, duration);
    }
    lv_slider_set_value(s_ui.progress, position, LV_ANIM_OFF);
    UpdateTimeLabel(position, duration);
}

void OnProgressReleased(lv_event_t* event) {
    if (s_ui.progress == nullptr ||
        lv_event_get_target(event) != s_ui.progress) {
        return;
    }
    const int target = lv_slider_get_value(s_ui.progress);
    std::string error;
    if (!PodcastPlayer::Seek(target, error)) {
        SetStatusDirect(error.empty() ? I18n::T("暂不支持拖动")
                                      : error.c_str());
        return;
    }
    UpdateTimeLabel(target, PodcastPlayer::GetDurationSeconds());
    SetStatusDirect(I18n::T("正在跳转..."));
}

void OnSpeedClicked(lv_event_t* /*event*/) {
    static constexpr float kSpeeds[] = {0.5f, 0.75f, 1.0f, 1.25f, 1.5f};
    constexpr size_t kSpeedCount = sizeof(kSpeeds) / sizeof(kSpeeds[0]);
    const float current = PodcastPlayer::GetSpeed();
    size_t index = 0;
    for (size_t i = 0; i < kSpeedCount; ++i) {
        if (current <= kSpeeds[i] + 0.01f) {
            index = i;
            break;
        }
    }
    const float speed = kSpeeds[(index + 1) % kSpeedCount];
    PodcastPlayer::SetSpeed(speed);
    if (s_ui.speed_label != nullptr) {
        char text[16] = {};
        std::snprintf(text, sizeof(text), "%gx", static_cast<double>(speed));
        lv_label_set_text(s_ui.speed_label, text);
    }
}

void SetStatusDirect(const char* text) {
    if (s_ui.status != nullptr) {
        lv_label_set_text(s_ui.status, text != nullptr ? text : "");
    }
}

lv_obj_t* CreateActionButton(lv_obj_t* parent, int32_t x, const char* text,
                             lv_event_cb_t callback) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_set_size(button, 150, 56);
    lv_obj_set_pos(button, x, 628);
    lv_obj_set_style_radius(button, 18, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x25324A),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x34466A),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 0, LV_PART_MAIN);
    screen_swipe_back_ignore(button, true);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(kColorText),
                                LV_PART_MAIN);
    lv_obj_center(label);
    return button;
}

void OnPodcastClicked(lv_event_t* event) {
    const auto index = static_cast<size_t>(reinterpret_cast<uintptr_t>(
        lv_event_get_user_data(event)));
    if (index >= s_items.size()) {
        return;
    }
    const PodcastItem item = s_items[index];
    s_current_title = item.title;
    PodcastPlayer::SetDuration(item.duration_seconds);
    s_progress_max = item.duration_seconds > 0 ? item.duration_seconds : 1;
    if (s_ui.progress != nullptr) {
        lv_slider_set_range(s_ui.progress, 0, s_progress_max);
        lv_slider_set_value(s_ui.progress, 0, LV_ANIM_OFF);
    }
    UpdateTimeLabel(0, item.duration_seconds);
    const uint32_t request_generation =
        s_request_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    const uint32_t session = CurrentSession();
    PodcastPlayer::Stop();
    s_playing = false;
    s_paused = false;
    s_preparing_play.store(true, std::memory_order_release);
    UpdateControls(false, false);
    SetStatusDirect(I18n::T("正在准备播放..."));

    auto* context = new ResolveResult{};
    context->session = session;
    context->request_generation = request_generation;
    context->podcast_id = item.id;
    context->title = item.title;
    if (xTaskCreate(
            [](void* arg) {
                auto* result = static_cast<ResolveResult*>(arg);
                std::string error;
                const PodcastPlayInfo play =
                    PodcastClient::ResolvePlayUrl(result->podcast_id, error);
                if (!play.ok) {
                    result->error =
                        play.error.empty() ? error : play.error;
                } else if (
                    result->session != CurrentSession() ||
                    result->request_generation !=
                        s_request_generation.load(
                            std::memory_order_relaxed)) {
                    result->error = "播放请求已取消";
                } else if (!PodcastPlayer::Start(play.url, error)) {
                    result->error = error;
                }
                if (lv_async_call(
                    [](void* data) {
                        std::unique_ptr<ResolveResult> resolved(
                            static_cast<ResolveResult*>(data));
                        if (!IsCurrentSession(resolved->session)) {
                            return;
                        }
                        if (!resolved->error.empty() &&
                            resolved->error != "播放请求已取消") {
                            s_preparing_play.store(false,
                                                   std::memory_order_release);
                            SetStatusDirect(resolved->error.c_str());
                            UpdateControls(false, false);
                        }
                    },
                    result) != LV_RESULT_OK) {
                    delete result;
                }
                vTaskDelete(nullptr);
            },
            "podcast_play_url", 8192, context, 4, nullptr) != pdPASS) {
        delete context;
        SetStatusDirect(I18n::T("创建播放请求失败"));
    }
}

void RebuildList() {
    if (s_ui.list == nullptr) {
        return;
    }
    lv_obj_clean(s_ui.list);

    if (s_items.empty()) {
        lv_obj_t* hint = lv_label_create(s_ui.list);
        lv_label_set_text(hint, I18n::T("暂无节目"));
        lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(hint, lv_color_hex(kColorSubtle),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER,
                                    LV_PART_MAIN);
        lv_obj_set_width(hint, kListWidth - 32);
        lv_obj_align(hint, LV_ALIGN_CENTER, 0, 0);
        screen_make_input_passive(hint);
        return;
    }

    for (size_t index = 0; index < s_items.size(); ++index) {
        const PodcastItem& item = s_items[index];
        lv_obj_t* row = lv_obj_create(s_ui.list);
        lv_obj_set_size(row, kListWidth - 8, 88);
        lv_obj_set_style_radius(row, 18, LV_PART_MAIN);
        lv_obj_set_style_bg_color(row, lv_color_hex(kColorCard),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_color(row, lv_color_hex(kColorCardPressed),
                                  LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(row, 16, LV_PART_MAIN);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(
            row, OnPodcastClicked, LV_EVENT_CLICKED,
            reinterpret_cast<void*>(static_cast<uintptr_t>(index)));

        lv_obj_t* title = lv_label_create(row);
        lv_label_set_text(title, item.title.c_str());
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_obj_set_width(title, 470);
        lv_obj_set_style_text_font(title, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(title, lv_color_hex(kColorText),
                                    LV_PART_MAIN);
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);
        screen_make_input_passive(title);

        char duration[24];
        FormatDuration(item.duration_seconds, duration, sizeof(duration));
        lv_obj_t* meta = lv_label_create(row);
        lv_label_set_text(meta, item.completed
                                    ? I18n::T("已完成")
                                    : I18n::T("未完成"));
        lv_obj_set_style_text_font(meta, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(
            meta, lv_color_hex(item.completed ? 0x4ADE80 : kColorSubtle),
            LV_PART_MAIN);
        lv_obj_align(meta, LV_ALIGN_TOP_RIGHT, 0, 0);
        screen_make_input_passive(meta);

        lv_obj_t* duration_label = lv_label_create(row);
        lv_label_set_text(duration_label, duration);
        lv_obj_set_style_text_font(duration_label, &font_puhui_20_4,
                                   LV_PART_MAIN);
        lv_obj_set_style_text_color(duration_label,
                                    lv_color_hex(kColorSubtle),
                                    LV_PART_MAIN);
        lv_obj_align(duration_label, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
        screen_make_input_passive(duration_label);
    }
}

void ApplyLoadResult(void* data) {
    std::unique_ptr<LoadResult> result(static_cast<LoadResult*>(data));
    if (!IsCurrentSession(result->session)) {
        return;
    }
    s_loading.store(false, std::memory_order_release);
    if (!result->error.empty()) {
        // 后台刷新失败时保留已有列表，不打断正在看的节目。
        if (s_items.empty()) {
            SetStatusDirect(result->error.c_str());
        }
        return;
    }
    s_items = std::move(result->items);
    s_cached_items = s_items;
    SaveListCache();
    if (s_items.empty()) {
        SetStatusDirect(I18n::T("暂无节目"));
    } else if (!s_preparing_play.load(std::memory_order_acquire)) {
        SetStatusDirect(I18n::T("选择节目开始收听"));
    }
    RebuildList();
    StartPlayUrlPrefetch();
}

void StartLoadWorker() {
    bool expected = false;
    if (!s_loading.compare_exchange_strong(expected, true)) {
        return;
    }
    const uint32_t session = CurrentSession();
    if (s_items.empty()) {
        SetStatusDirect(I18n::T("正在加载节目..."));
    }
    auto* context = new LoadResult{};
    context->session = session;
    if (xTaskCreate(
            [](void* arg) {
                auto* result = static_cast<LoadResult*>(arg);
                std::string error;
                result->items = PodcastClient::FetchPodcasts(error);
                result->error = std::move(error);
                if (lv_async_call(ApplyLoadResult, result) != LV_RESULT_OK) {
                    s_loading.store(false, std::memory_order_release);
                    delete result;
                }
                vTaskDelete(nullptr);
            },
            "podcast_list", 8192, context, 4, nullptr) != pdPASS) {
        s_loading.store(false, std::memory_order_release);
        delete context;
        SetStatusDirect(I18n::T("加载节目失败"));
    }
}

void ApplyPlayerEvent(void* data) {
    std::unique_ptr<PlayerUiEvent> event(
        static_cast<PlayerUiEvent*>(data));
    if (!IsCurrentSession(event->session)) {
        return;
    }
    switch (event->event) {
        case PodcastPlayerEvent::Playing:
            s_preparing_play.store(false, std::memory_order_release);
            s_playing = true;
            s_paused = false;
            UpdateControls(true, true);
            SetStatusDirect(s_current_title.empty()
                                ? I18n::T("正在播放")
                                : s_current_title.c_str());
            break;
        case PodcastPlayerEvent::Paused:
            s_preparing_play.store(false, std::memory_order_release);
            s_paused = true;
            UpdateControls(true, false);
            SetStatusDirect(I18n::T("已暂停"));
            break;
        case PodcastPlayerEvent::Finished:
            s_preparing_play.store(false, std::memory_order_release);
            s_playing = false;
            s_paused = false;
            UpdateControls(false, false);
            SetStatusDirect(I18n::T("播放结束"));
            break;
        case PodcastPlayerEvent::Error:
            s_preparing_play.store(false, std::memory_order_release);
            s_playing = false;
            s_paused = false;
            UpdateControls(false, false);
            SetStatusDirect(event->detail.empty()
                                ? I18n::T("播放失败")
                                : event->detail.c_str());
            break;
    }
}

void OnPlayerEvent(PodcastPlayerEvent event, const char* detail,
                   void* /*ctx*/) {
    if (!s_screen_active.load(std::memory_order_relaxed)) {
        return;
    }
    auto* message = new PlayerUiEvent{};
    message->session = CurrentSession();
    message->event = event;
    if (detail != nullptr) {
        message->detail = detail;
    }
    if (lv_async_call(ApplyPlayerEvent, message) != LV_RESULT_OK) {
        delete message;
    }
}

void OnPauseClicked(lv_event_t* /*event*/) {
    if (s_playing && !s_paused) {
        if (PodcastPlayer::Pause()) {
            s_paused = true;
            UpdateControls(true, false);
            SetStatusDirect(I18n::T("已暂停"));
        }
    } else if (PodcastPlayer::Resume()) {
        s_paused = false;
        UpdateControls(true, true);
        SetStatusDirect(s_current_title.empty()
                            ? I18n::T("正在播放")
                            : s_current_title.c_str());
    }
}

void OnStopClicked(lv_event_t* /*event*/) {
    s_request_generation.fetch_add(1, std::memory_order_acq_rel);
    s_preparing_play.store(false, std::memory_order_release);
    PodcastPlayer::Stop();
    s_playing = false;
    s_paused = false;
    UpdateControls(false, false);
    SetStatusDirect(I18n::T("已停止"));
}

void ReturnHome() {
    s_session.fetch_add(1, std::memory_order_acq_rel);
    s_request_generation.fetch_add(1, std::memory_order_acq_rel);
    s_preparing_play.store(false, std::memory_order_release);
    s_screen_active.store(false, std::memory_order_release);
    PodcastPlayer::Stop();
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

// 播放中先停回节目列表，列表页再点才离开播客页。
void HandleBack() {
    if (s_playing || s_paused) {
        s_preparing_play.store(false, std::memory_order_release);
        PodcastPlayer::Stop();
        s_playing = false;
        s_paused = false;
        s_current_title.clear();
        UpdateControls(false, false);
        RebuildList();
        SetStatusDirect(I18n::T("选择节目开始播放"));
        return;
    }
    ReturnHome();
}

void OnBackClicked(lv_event_t* /*event*/) {
    HandleBack();
}

void OnSwipeBack() {
    HandleBack();
}

void OnScreenUnloaded(lv_event_t* event) {
    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(event));
    if (target != s_bound_screen) {
        return;
    }
    s_bound_screen = nullptr;
    s_screen_active.store(false, std::memory_order_release);
    s_session.fetch_add(1, std::memory_order_acq_rel);
    s_request_generation.fetch_add(1, std::memory_order_acq_rel);
    s_preparing_play.store(false, std::memory_order_release);
    PodcastPlayer::SetEventCallback(nullptr, nullptr);
    PodcastPlayer::Stop();
    s_current_title.clear();
    s_playing = false;
    s_paused = false;
    if (s_ui.progress_timer != nullptr) {
        lv_timer_delete(s_ui.progress_timer);
    }
    s_ui = PodcastUi{};
}

void BuildHeader(lv_obj_t* screen) {
    lv_obj_t* back = lv_button_create(screen);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, 72, 72);
    lv_obj_set_pos(back, 16, 22);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back, lv_color_hex(0xFFFFFF),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back, LV_OPA_20, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back, 0, LV_PART_MAIN);
    screen_swipe_back_ignore(back, true);
    lv_obj_add_event_cb(back, OnBackClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* back_icon = lv_image_create(back);
    lv_image_set_src(back_icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(back_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(back_icon);

    lv_obj_t* title = lv_label_create(screen);
    lv_label_set_text(title, I18n::T("一丢播客"));
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(kColorText),
                                LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);
    screen_make_input_passive(title);
}

}  // namespace

lv_obj_t* PodcastScreen::Create(lv_obj_t* return_screen) {
    lv_obj_t* screen = lv_obj_create(nullptr);
    lv_obj_set_size(screen, kPanelSize, kPanelSize);
    lv_obj_set_style_bg_color(screen, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    s_ui.screen = screen;
    s_return_screen = return_screen;

    BuildHeader(screen);

    lv_obj_t* list = lv_obj_create(screen);
    s_ui.list = list;
    lv_obj_set_pos(list, kListX, kListY);
    lv_obj_set_size(list, kListWidth, kListHeight);
    lv_obj_set_style_bg_color(list, lv_color_hex(kColorPanel),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(list, 22, LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list, 10, LV_PART_MAIN);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    s_ui.status = lv_label_create(screen);
    lv_label_set_text(s_ui.status, I18n::T("正在加载节目..."));
    lv_obj_set_width(s_ui.status, 640);
    lv_label_set_long_mode(s_ui.status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_ui.status, &font_puhui_20_4,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.status, lv_color_hex(kColorSubtle),
                                LV_PART_MAIN);
    lv_obj_set_style_text_align(s_ui.status, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_obj_align(s_ui.status, LV_ALIGN_TOP_MID, 0, 506);
    screen_make_input_passive(s_ui.status);

    s_ui.progress = lv_slider_create(screen);
    lv_obj_set_size(s_ui.progress, 552, 12);
    lv_obj_set_pos(s_ui.progress, 74, 548);
    s_progress_max = 1;
    lv_slider_set_range(s_ui.progress, 0, 1);
    lv_slider_set_value(s_ui.progress, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_ui.progress, lv_color_hex(0x1E293B),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.progress, lv_color_hex(kColorAccent),
                              LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_ui.progress, lv_color_hex(kColorText),
                              LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s_ui.progress, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_ui.progress, 0, LV_PART_KNOB);
    screen_swipe_back_ignore(s_ui.progress, true);
    lv_obj_add_event_cb(s_ui.progress, OnProgressReleased, LV_EVENT_RELEASED,
                        nullptr);

    s_ui.time_label = lv_label_create(screen);
    lv_label_set_text(s_ui.time_label, "0:00 / --:--");
    lv_obj_set_style_text_font(s_ui.time_label, &font_puhui_20_4,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.time_label, lv_color_hex(kColorSubtle),
                                LV_PART_MAIN);
    lv_obj_align(s_ui.time_label, LV_ALIGN_TOP_RIGHT, -40, 566);
    screen_make_input_passive(s_ui.time_label);

    s_ui.speed_btn = lv_button_create(screen);
    lv_obj_set_size(s_ui.speed_btn, 110, 56);
    lv_obj_set_pos(s_ui.speed_btn, 556, 628);
    lv_obj_set_style_radius(s_ui.speed_btn, 18, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.speed_btn, lv_color_hex(0x25324A),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.speed_btn, lv_color_hex(0x34466A),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(s_ui.speed_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ui.speed_btn, 0, LV_PART_MAIN);
    screen_swipe_back_ignore(s_ui.speed_btn, true);
    lv_obj_add_event_cb(s_ui.speed_btn, OnSpeedClicked, LV_EVENT_CLICKED,
                        nullptr);
    s_ui.speed_label = lv_label_create(s_ui.speed_btn);
    lv_label_set_text(s_ui.speed_label, "1x");
    lv_obj_set_style_text_font(s_ui.speed_label, &font_puhui_20_4,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.speed_label, lv_color_hex(kColorText),
                                LV_PART_MAIN);
    lv_obj_center(s_ui.speed_label);

    s_ui.progress_timer = lv_timer_create(OnProgressTimer, 500, nullptr);

    s_ui.pause_label = nullptr;
    s_ui.pause_btn =
        CreateActionButton(screen, 180, I18n::T("暂停"), OnPauseClicked);
    s_ui.pause_label =
        lv_obj_get_child(s_ui.pause_btn, 0);
    s_ui.stop_btn =
        CreateActionButton(screen, 390, I18n::T("停止"), OnStopClicked);
    UpdateControls(false, false);

    LoadListCache();
    if (!s_cached_items.empty()) {
        s_items = s_cached_items;
        RebuildList();
        SetStatusDirect(I18n::T("正在刷新节目..."));
    }

    s_bound_screen = screen;
    lv_obj_add_event_cb(screen, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);
    screen_attach_swipe_back(screen, OnSwipeBack);
    return screen;
}

void PodcastScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: podcast_screen");
        Application::GetInstance().SetVoiceUiDesired(false);
        s_screen_active.store(true, std::memory_order_release);
        s_session.fetch_add(1, std::memory_order_acq_rel);
        s_request_generation.fetch_add(1, std::memory_order_acq_rel);
        s_loading.store(false, std::memory_order_release);
        s_playing = false;
        s_paused = false;
        s_preparing_play.store(false, std::memory_order_release);
        PodcastPlayer::SetEventCallback(OnPlayerEvent, nullptr);
        UpdateControls(false, false);
        if (!s_items.empty()) {
            RebuildList();
            StartPlayUrlPrefetch();
        }
        StartLoadWorker();
    } else {
        ESP_LOGI(TAG, "unload: podcast_screen");
        s_screen_active.store(false, std::memory_order_release);
        s_session.fetch_add(1, std::memory_order_acq_rel);
        s_request_generation.fetch_add(1, std::memory_order_acq_rel);
        PodcastPlayer::Stop();
    }
}
