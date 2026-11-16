#include "english_screen.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "application.h"
#include "assets/lang_config.h"
#include "chat_screen/chat_screen.h"
#include "english_client.h"
#include "english_types.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <font_awesome.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "home_screen/home_screen.h"
#include "i18n.h"
#include "podcast_player.h"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr const char* TAG = "EnglishScreen";
constexpr uint32_t kChoiceFeedbackMs = 650;
constexpr int32_t kPanelSize = 720;
constexpr int32_t kContentX = 24;
constexpr int32_t kContentY = 100;
constexpr int32_t kContentWidth = 672;
constexpr int32_t kContentHeight = 480;
constexpr uint32_t kColorBg = 0x0B1020;
constexpr uint32_t kColorPanel = 0x111827;
constexpr uint32_t kColorCard = 0x182033;
constexpr uint32_t kColorCardPressed = 0x24314D;
constexpr uint32_t kColorAccent = 0x60A5FA;
constexpr uint32_t kColorSuccess = 0x34D399;
constexpr uint32_t kColorWarning = 0xFBBF24;
constexpr uint32_t kColorDanger = 0xF87171;
constexpr uint32_t kColorText = 0xF8FAFC;
constexpr uint32_t kColorSubtle = 0x94A3B8;
constexpr uint32_t kColorChoice = 0xE2E8F0;

enum class RequestKind : uint8_t {
    Tasks,
    Review,
    Result,
};

struct EnglishUi {
    lv_obj_t* screen = nullptr;
    lv_obj_t* content = nullptr;
    lv_obj_t* status = nullptr;
    lv_obj_t* progress = nullptr;
    lv_obj_t* stage_caption = nullptr;
    lv_obj_t* card = nullptr;
    lv_obj_t* card_label = nullptr;
};

struct RequestResult {
    uint32_t session = 0;
    uint32_t generation = 0;
    RequestKind kind = RequestKind::Tasks;
    EnglishTaskList tasks;
    EnglishReview review;
    std::string task_id;
    std::string word_id;
    std::string result;
    std::string error;
    int index = 0;
};

EnglishUi s_ui;
lv_obj_t* s_bound_screen = nullptr;
lv_obj_t* s_return_screen = nullptr;
EnglishTaskList s_tasks;
// 上次成功拉到的任务列表：再次进入时先显示，后台再刷新。
EnglishTaskList s_cached_tasks;
bool s_cache_valid = false;

constexpr const char* kTasksCachePath = "/sdcard/system/english_tasks.txt";

void SaveTasksCache() {
    FILE* file = std::fopen(kTasksCachePath, "w");
    if (file == nullptr) {
        return;
    }
    for (const auto& task : s_tasks) {
        std::fprintf(file, "%s|%s|%s|%d|%d\n", task.id.c_str(),
                     task.task_date.c_str(), task.status.c_str(),
                     task.phrase_count, task.can_review ? 1 : 0);
    }
    std::fclose(file);
}

// 列表写入 SD 后，冷启动首次进入也能先显示上次的任务。
void LoadTasksCache() {
    FILE* file = std::fopen(kTasksCachePath, "r");
    if (file == nullptr) {
        return;
    }
    EnglishTaskList tasks;
    char line[512];
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        std::string text(line);
        while (!text.empty() &&
               (text.back() == '\n' || text.back() == '\r')) {
            text.pop_back();
        }
        if (text.empty()) {
            continue;
        }
        std::string fields[5];
        size_t field = 0;
        size_t start = 0;
        for (size_t i = 0; i <= text.size() && field < 5; ++i) {
            if (i == text.size() || text[i] == '|') {
                if (field < 5) {
                    fields[field] = text.substr(start, i - start);
                    ++field;
                }
                start = i + 1;
            }
        }
        if (field < 5 || fields[0].empty()) {
            continue;
        }
        EnglishTask task;
        task.id = fields[0];
        task.task_date = fields[1];
        task.status = fields[2];
        task.phrase_count = std::atoi(fields[3].c_str());
        task.can_review = fields[4] == "1";
        tasks.push_back(std::move(task));
    }
    std::fclose(file);
    if (!tasks.empty()) {
        s_cached_tasks = std::move(tasks);
        s_cache_valid = true;
    }
}
EnglishReview s_review;
std::string s_current_task_id;
size_t s_current_index = 0;
uint8_t s_stage = 0;
bool s_in_review = false;
bool s_choice_locked = false;
int s_choice_correct = -1;
lv_obj_t* s_choice_buttons[4] = {};
lv_timer_t* s_choice_feedback_timer = nullptr;
std::atomic<bool> s_screen_active{false};
std::atomic<bool> s_busy{false};
std::atomic<int64_t> s_busy_since_us{0};
std::atomic<uint32_t> s_session{0};
std::atomic<uint32_t> s_request_generation{0};

// 复习中断后保留进度：只保留在当前固件进程内，不做跨重启持久化。
std::string s_history_task_id;
EnglishReview s_history_review;
size_t s_history_index = 0;
uint8_t s_history_stage = 0;
bool s_has_history = false;

void SaveReviewProgress() {
    if (!s_in_review || s_current_task_id.empty() ||
        s_review.entries.empty()) {
        return;
    }
    s_history_task_id = s_current_task_id;
    s_history_review = s_review;
    s_history_index = s_current_index;
    s_history_stage = s_stage;
    s_has_history = true;
}

void CancelChoiceFeedback() {
    if (s_choice_feedback_timer != nullptr) {
        lv_timer_delete(s_choice_feedback_timer);
        s_choice_feedback_timer = nullptr;
    }
}

bool RestoreReviewProgress(const std::string& task_id) {
    if (!s_has_history || s_history_task_id != task_id ||
        s_history_review.entries.empty() ||
        s_history_index >= s_history_review.entries.size()) {
        return false;
    }
    s_review = s_history_review;
    s_current_task_id = task_id;
    s_current_index = s_history_index;
    s_stage = s_history_stage;
    s_in_review = true;
    return true;
}

uint32_t CurrentSession() {
    return s_session.load(std::memory_order_relaxed);
}

bool IsCurrentRequest(const RequestResult& result) {
    return s_screen_active.load(std::memory_order_relaxed) &&
           result.session == CurrentSession() &&
           result.generation ==
               s_request_generation.load(std::memory_order_relaxed);
}

void SetStatus(const char* text) {
    if (s_ui.status != nullptr) {
        lv_label_set_text(s_ui.status, text != nullptr ? text : "");
    }
}

void PostSetStatus(const char* text) {
    if (text == nullptr) {
        return;
    }
    lv_async_call(
        [](void* data) {
            std::unique_ptr<std::string> message(
                static_cast<std::string*>(data));
            SetStatus(message->c_str());
        },
        new std::string(text));
}

void ResetContentRefs() {
    CancelChoiceFeedback();
    s_ui.progress = nullptr;
    s_ui.stage_caption = nullptr;
    s_ui.card = nullptr;
    s_ui.card_label = nullptr;
    s_choice_locked = false;
    s_choice_correct = -1;
    std::memset(s_choice_buttons, 0, sizeof(s_choice_buttons));
}

lv_obj_t* CreateButton(lv_obj_t* parent, int32_t x, int32_t y,
                       int32_t width, int32_t height, const char* text,
                       uint32_t color, lv_event_cb_t callback) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_set_size(button, width, height);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_style_radius(button, 18, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(color),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 0, LV_PART_MAIN);
    screen_swipe_back_ignore(button, true);
    if (callback != nullptr) {
        lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
    }

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(0x0B1020),
                                LV_PART_MAIN);
    lv_obj_center(label);
    return button;
}

lv_obj_t* CreateChoiceButton(lv_obj_t* parent, int32_t x, int32_t y,
                             int32_t width, int32_t height, const char* text,
                             lv_event_cb_t callback, uintptr_t index) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_set_size(button, width, height);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_style_radius(button, 20, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(kColorChoice),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(kColorAccent),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 0, LV_PART_MAIN);
    screen_swipe_back_ignore(button, true);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(index));

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, width - 28);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(0x0B1020),
                                LV_PART_MAIN);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_center(label);
    return button;
}

void ConfigurePageContent() {
    lv_obj_set_style_bg_color(s_ui.content,
                              lv_color_hex(kColorPanel), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.content, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_ui.content, 22, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ui.content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_ui.content, 0, LV_PART_MAIN);
    lv_obj_set_style_layout(s_ui.content, LV_LAYOUT_NONE, 0);
    lv_obj_set_scrollbar_mode(s_ui.content, LV_SCROLLBAR_MODE_OFF);
}

void ConfigureListContent() {
    ConfigurePageContent();
    lv_obj_set_style_pad_all(s_ui.content, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_ui.content, 10, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_ui.content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ui.content, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(s_ui.content, LV_SCROLLBAR_MODE_AUTO);
}

void ShowCenteredMessage(const char* text, bool show_back_button,
                         bool compact = false);
void OnBackToListClicked(lv_event_t* /*event*/);
void OnTaskClicked(lv_event_t* event);
void OnCardClicked(lv_event_t* /*event*/);
void OnPronounceClicked(lv_event_t* /*event*/);
void OnChoiceClicked(lv_event_t* event);
void OnRememberedClicked(lv_event_t* /*event*/);
void OnForgottenClicked(lv_event_t* /*event*/);
void StartTasksWorker();
void StartReviewWorker(const std::string& task_id);
void StartSubmitWorker(const std::string& result);
void BuildTaskList();
void BuildReview();
void BuildChoice();
void BuildComplete();
void AdvanceAfterChoiceFeedback();

void AdvanceAfterChoiceFeedback() {
    CancelChoiceFeedback();
    SetStatus(I18n::T("结果已保存"));
    if (s_current_index + 1 >= s_review.entries.size()) {
        BuildComplete();
        return;
    }
    s_current_index++;
    s_stage = 0;
    BuildReview();
}

void ShowCenteredMessage(const char* text, bool show_back_button,
                         bool compact) {
    ResetContentRefs();
    lv_obj_clean(s_ui.content);
    ConfigurePageContent();

    lv_obj_t* label = lv_label_create(s_ui.content);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, kContentWidth - 64);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label,
                               compact ? &font_puhui_20_4 : &font_puhui_30_4,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(label,
                                lv_color_hex(compact ? kColorSubtle : kColorText),
                                LV_PART_MAIN);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_CENTER, 0,
                 show_back_button ? -40 : 0);
    screen_make_input_passive(label);

    if (show_back_button) {
        CreateButton(s_ui.content, 206, 330, 260, 64,
                     I18n::T("返回任务"), kColorAccent,
                     OnBackToListClicked);
    }
}

void BuildTaskList() {
    s_in_review = false;
    ResetContentRefs();
    lv_obj_clean(s_ui.content);
    ConfigureListContent();

    if (s_tasks.empty()) {
        lv_obj_t* hint = lv_label_create(s_ui.content);
        lv_label_set_text(hint, I18n::T("还未收到任务"));
        lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(hint, lv_color_hex(kColorSubtle),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER,
                                    LV_PART_MAIN);
        lv_obj_set_width(hint, kContentWidth - 32);
        lv_obj_align(hint, LV_ALIGN_CENTER, 0, 0);
        screen_make_input_passive(hint);
        return;
    }

    for (size_t index = 0; index < s_tasks.size(); ++index) {
        const EnglishTask& task = s_tasks[index];
        lv_obj_t* row = lv_obj_create(s_ui.content);
        lv_obj_set_size(row, kContentWidth - 8, 92);
        lv_obj_set_style_radius(row, 18, LV_PART_MAIN);
        lv_obj_set_style_bg_color(row, lv_color_hex(kColorCard),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_color(row, lv_color_hex(kColorCardPressed),
                                  LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(row, 16, LV_PART_MAIN);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(
            row, OnTaskClicked, LV_EVENT_CLICKED,
            reinterpret_cast<void*>(static_cast<uintptr_t>(index)));

        char meta_buffer[96];
        std::snprintf(meta_buffer, sizeof(meta_buffer), "%s · %d 个短句",
                      task.task_date.c_str(), task.phrase_count);
        lv_obj_t* meta = lv_label_create(row);
        lv_label_set_text(meta, meta_buffer);
        lv_obj_set_style_text_font(meta, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(meta, lv_color_hex(kColorText),
                                    LV_PART_MAIN);
        lv_obj_align(meta, LV_ALIGN_TOP_LEFT, 0, 0);
        screen_make_input_passive(meta);

        lv_obj_t* state = lv_label_create(row);
        const char* state_text = task.status == "completed"
                                     ? I18n::T("已完成")
                                     : task.status == "in_progress"
                                           ? I18n::T("进行中")
                                           : I18n::T("未开始");
        const bool resumable =
            s_has_history && s_history_task_id == task.id &&
            s_history_index < s_history_review.entries.size();
        if (resumable) {
            state_text = I18n::T("继续复习");
        }
        lv_label_set_text(state, state_text);
        lv_obj_set_style_text_font(state, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(
            state, lv_color_hex(resumable ? kColorAccent
                                    : task.status == "completed"
                                          ? kColorSuccess
                                          : kColorSubtle),
            LV_PART_MAIN);
        lv_obj_align(state, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        screen_make_input_passive(state);

        lv_obj_t* action = lv_label_create(row);
        lv_label_set_text(action,
                          resumable ? I18n::T("继续 >") : I18n::T("复习 >"));
        lv_obj_set_style_text_font(action, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(action, lv_color_hex(kColorAccent),
                                    LV_PART_MAIN);
        lv_obj_align(action, LV_ALIGN_RIGHT_MID, 0, 0);
        screen_make_input_passive(action);
    }
}

void UpdateCard() {
    if (s_ui.card_label == nullptr || s_ui.stage_caption == nullptr ||
        s_ui.progress == nullptr || s_current_index >= s_review.entries.size()) {
        return;
    }
    const EnglishEntry& entry = s_review.entries[s_current_index];
    const char* caption = I18n::T("中文含义");
    const char* text = entry.meaning.c_str();
    if (s_stage == 1) {
        caption = I18n::T("英文短句");
        text = entry.phrase.c_str();
    } else if (s_stage == 2) {
        caption = I18n::T("场景例句");
        text = entry.example.c_str();
    }
    lv_label_set_text(s_ui.stage_caption, caption);
    lv_label_set_text(s_ui.card_label, text);

    char progress[32];
    std::snprintf(progress, sizeof(progress), "%u / %u",
                  static_cast<unsigned>(s_current_index + 1),
                  static_cast<unsigned>(s_review.entries.size()));
    lv_label_set_text(s_ui.progress, progress);
}

void BuildReview() {
    s_in_review = true;
    ResetContentRefs();
    lv_obj_clean(s_ui.content);
    ConfigurePageContent();
    if (s_review.entries.empty()) {
        ShowCenteredMessage(I18n::T("暂无复习短句"), true);
        return;
    }
    const EnglishEntry& entry = s_review.entries[s_current_index];
    if (entry.options.size() >= 2 && entry.options.size() <= 4 &&
        entry.correct_index >= 0 &&
        entry.correct_index < static_cast<int>(entry.options.size())) {
        BuildChoice();
        return;
    }

    s_ui.stage_caption = lv_label_create(s_ui.content);
    lv_obj_set_style_text_font(s_ui.stage_caption, &font_puhui_20_4,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.stage_caption,
                                lv_color_hex(kColorAccent), LV_PART_MAIN);
    lv_obj_align(s_ui.stage_caption, LV_ALIGN_TOP_MID, 0, 8);
    screen_make_input_passive(s_ui.stage_caption);

    s_ui.progress = lv_label_create(s_ui.content);
    lv_obj_set_style_text_font(s_ui.progress, &font_puhui_20_4,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.progress,
                                lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_align(s_ui.progress, LV_ALIGN_TOP_RIGHT, -16, 8);
    screen_make_input_passive(s_ui.progress);

    s_ui.card = lv_obj_create(s_ui.content);
    lv_obj_set_pos(s_ui.card, 16, 54);
    lv_obj_set_size(s_ui.card, kContentWidth - 32, 300);
    lv_obj_set_style_radius(s_ui.card, 28, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.card, lv_color_hex(kColorCard),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.card, lv_color_hex(kColorCardPressed),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_width(s_ui.card, 0, LV_PART_MAIN);
    lv_obj_add_flag(s_ui.card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_ui.card, OnCardClicked, LV_EVENT_CLICKED,
                        nullptr);
    screen_swipe_back_ignore(s_ui.card, true);

    s_ui.card_label = lv_label_create(s_ui.card);
    lv_obj_set_width(s_ui.card_label, kContentWidth - 96);
    lv_label_set_long_mode(s_ui.card_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_ui.card_label, &font_puhui_30_4,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.card_label,
                                lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_ui.card_label, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_obj_center(s_ui.card_label);
    screen_make_input_passive(s_ui.card_label);

    lv_obj_t* hint = lv_label_create(s_ui.content);
    lv_label_set_text(hint, I18n::T("轻触卡片翻面"));
    lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(kColorSubtle),
                                LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 366);
    screen_make_input_passive(hint);

    CreateButton(s_ui.content, 40, 402, 280, 64,
                 I18n::T("还需努力"), kColorWarning,
                 OnForgottenClicked);
    CreateButton(s_ui.content, 352, 402, 280, 64,
                 I18n::T("已经掌握"), kColorSuccess,
                 OnRememberedClicked);
    UpdateCard();
}

void BuildChoice() {
    const EnglishEntry& entry = s_review.entries[s_current_index];
    s_choice_locked = false;
    s_choice_correct = entry.correct_index;

    s_ui.stage_caption = lv_label_create(s_ui.content);
    lv_label_set_text(s_ui.stage_caption, I18n::T("选出正确的中文意思"));
    lv_obj_set_style_text_font(s_ui.stage_caption, &font_puhui_20_4,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.stage_caption,
                                lv_color_hex(kColorAccent), LV_PART_MAIN);
    lv_obj_align(s_ui.stage_caption, LV_ALIGN_TOP_LEFT, 16, 8);
    screen_make_input_passive(s_ui.stage_caption);

    s_ui.progress = lv_label_create(s_ui.content);
    lv_obj_set_style_text_font(s_ui.progress, &font_puhui_20_4,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.progress,
                                lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_align(s_ui.progress, LV_ALIGN_TOP_RIGHT, -16, 8);
    screen_make_input_passive(s_ui.progress);
    char progress[32];
    std::snprintf(progress, sizeof(progress), "%u / %u",
                  static_cast<unsigned>(s_current_index + 1),
                  static_cast<unsigned>(s_review.entries.size()));
    lv_label_set_text(s_ui.progress, progress);

    s_ui.card = lv_obj_create(s_ui.content);
    lv_obj_set_pos(s_ui.card, 16, 52);
    lv_obj_set_size(s_ui.card, kContentWidth - 32, 210);
    lv_obj_set_style_radius(s_ui.card, 26, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.card, lv_color_hex(kColorCard),
                              LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ui.card, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_ui.card, 0, LV_PART_MAIN);

    s_ui.card_label = lv_label_create(s_ui.card);
    lv_obj_set_width(s_ui.card_label, kContentWidth - 200);
    lv_label_set_long_mode(s_ui.card_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_ui.card_label, entry.phrase.c_str());
    lv_obj_set_style_text_font(s_ui.card_label, &font_puhui_30_4,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.card_label,
                                lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_ui.card_label, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_obj_align(s_ui.card_label, LV_ALIGN_LEFT_MID, 20,
                 entry.phonetic.empty() ? 0 : -16);
    screen_make_input_passive(s_ui.card_label);

    if (!entry.phonetic.empty()) {
        lv_obj_t* phonetic = lv_label_create(s_ui.card);
        lv_obj_set_width(phonetic, kContentWidth - 200);
        lv_label_set_long_mode(phonetic, LV_LABEL_LONG_WRAP);
        lv_label_set_text(phonetic, entry.phonetic.c_str());
        lv_obj_set_style_text_font(phonetic, &font_puhui_20_4,
                                   LV_PART_MAIN);
        lv_obj_set_style_text_color(phonetic, lv_color_hex(kColorSubtle),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_align(phonetic, LV_TEXT_ALIGN_CENTER,
                                    LV_PART_MAIN);
        lv_obj_align(phonetic, LV_ALIGN_LEFT_MID, 20, 38);
        screen_make_input_passive(phonetic);
    }

    const std::string pronounce_label =
        std::string(FONT_AWESOME_VOLUME_HIGH) + " " + I18n::T("发音");
    CreateButton(s_ui.card, 526, 76, 92, 56, pronounce_label.c_str(),
                 kColorAccent, OnPronounceClicked);

    for (size_t i = 0; i < entry.options.size(); ++i) {
        const int32_t x = static_cast<int32_t>(16 + (i % 2) * 328);
        const int32_t y = static_cast<int32_t>(278 + (i / 2) * 96);
        s_choice_buttons[i] = CreateChoiceButton(
            s_ui.content, x, y, 312, 84, entry.options[i].c_str(),
            OnChoiceClicked, i);
    }
}

void BuildComplete() {
    CancelChoiceFeedback();
    s_in_review = false;
    ShowCenteredMessage(
        I18n::T("本次复习完成\n未掌握的短句已加入成长记录"), true);
    SetStatus(I18n::T("复习完成"));
}

void PostRequestResult(RequestResult* result) {
    if (lv_async_call(
            [](void* data) {
                std::unique_ptr<RequestResult> resolved(
                    static_cast<RequestResult*>(data));
                if (!IsCurrentRequest(*resolved)) {
                    // 同一页面里被判过期的请求也要交还 busy，
                    // 否则后续刷新会一直被拒，永远停在“正在刷新任务”。
                    if (resolved->session == CurrentSession()) {
                        s_busy.store(false, std::memory_order_release);
                    }
                    return;
                }
                s_busy.store(false, std::memory_order_release);
                if (!resolved->error.empty()) {
                    if (resolved->kind == RequestKind::Result) {
                        s_choice_locked = false;
                    }
                    SetStatus(resolved->error.c_str());
                    return;
                }
                switch (resolved->kind) {
                    case RequestKind::Tasks:
                        s_tasks = std::move(resolved->tasks);
                        s_cached_tasks = s_tasks;
                        s_cache_valid = true;
                        SaveTasksCache();
                        BuildTaskList();
                        SetStatus(s_tasks.empty()
                                      ? I18n::T("还未收到任务")
                                      : I18n::T("选择任务开始复习"));
                        break;
                    case RequestKind::Review:
                        s_review = std::move(resolved->review);
                        s_current_task_id = resolved->task_id;
                        s_current_index = 0;
                        s_stage = 0;
                        BuildReview();
                        if (s_review.entries.empty()) {
                            SetStatus(I18n::T("暂无复习短句"));
                        } else {
                            SetStatus(s_review.label.empty()
                                          ? I18n::T("轻触卡片翻面")
                                          : s_review.label.c_str());
                        }
                        break;
                    case RequestKind::Result:
                        PostSetStatus(I18n::T("结果已保存"));
                        break;
                }
            },
            result) != LV_RESULT_OK) {
        s_busy.store(false, std::memory_order_release);
        delete result;
    }
}

bool BeginRequest(RequestResult*& context) {
    bool expected = false;
    if (!s_busy.compare_exchange_strong(expected, true)) {
        const int64_t since =
            s_busy_since_us.load(std::memory_order_relaxed);
        // 看门狗：请求卡死超过 5 秒后允许新的请求接管，
        // 避免网络异常时页面永久停在加载中。
        if (since != 0 &&
            esp_timer_get_time() - since < 5LL * 1000 * 1000) {
            delete context;
            return false;
        }
        s_busy.store(true, std::memory_order_release);
    }
    s_busy_since_us.store(esp_timer_get_time(), std::memory_order_relaxed);
    context->session = CurrentSession();
    context->generation =
        s_request_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    return true;
}

BaseType_t CreateWorkerTask(TaskFunction_t function, const char* name,
                            uint32_t stack_depth, void* arg) {
#if CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
    BaseType_t result =
        xTaskCreateWithCaps(function, name, stack_depth, arg, 4, nullptr,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    BaseType_t result =
        xTaskCreate(function, name, stack_depth, arg, 4, nullptr);
#endif
    if (result != pdPASS) {
        ESP_LOGE(TAG, "create %s failed: internal=%u psram=%u", name,
                 static_cast<unsigned>(
                     heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(
                     heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    }
    return result;
}

void FinishWorkerTask() {
#if CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
    vTaskDeleteWithCaps(nullptr);
#else
    vTaskDelete(nullptr);
#endif
}

void StartTasksWorker() {
    auto* context = new RequestResult{};
    context->kind = RequestKind::Tasks;
    if (!BeginRequest(context)) {
        SetStatus(I18n::T("上一请求还在处理，请稍候"));
        return;
    }
    if (CreateWorkerTask(
            [](void* arg) {
                auto* result = static_cast<RequestResult*>(arg);
                result->tasks = EnglishClient::FetchTasks(result->error);
                PostRequestResult(result);
                FinishWorkerTask();
            },
            "english_tasks", 8192, context) != pdPASS) {
        s_busy.store(false, std::memory_order_release);
        delete context;
        SetStatus(I18n::T("加载任务失败"));
    }
}

void StartReviewWorker(const std::string& task_id) {
    auto* context = new RequestResult{};
    context->kind = RequestKind::Review;
    context->task_id = task_id;
    if (!BeginRequest(context)) {
        SetStatus(I18n::T("上一请求还在处理，请稍候"));
        return;
    }
    if (CreateWorkerTask(
            [](void* arg) {
                auto* result = static_cast<RequestResult*>(arg);
                result->review =
                    EnglishClient::FetchReview(result->task_id, result->error);
                PostRequestResult(result);
                FinishWorkerTask();
            },
            "english_review", 8192, context) != pdPASS) {
        s_busy.store(false, std::memory_order_release);
        delete context;
        SetStatus(I18n::T("加载复习内容失败"));
    }
}

void StartSubmitWorker(const std::string& result) {
    if (s_current_task_id.empty() ||
        s_current_index >= s_review.entries.size()) {
        return;
    }
    auto* context = new RequestResult{};
    context->kind = RequestKind::Result;
    context->task_id = s_current_task_id;
    context->word_id = s_review.entries[s_current_index].word_id;
    context->result = result;
    context->index = static_cast<int>(s_current_index);
    if (!BeginRequest(context)) {
        return;
    }
    SetStatus(I18n::T("正在保存结果..."));
    if (CreateWorkerTask(
            [](void* arg) {
                auto* request = static_cast<RequestResult*>(arg);
                const bool ok = EnglishClient::SubmitResult(
                    request->task_id, request->word_id, request->result,
                    request->error);
                if (!ok && request->error.empty()) {
                    request->error = "保存结果失败";
                }
                if (!ok) {
                    PostSetStatus(request->error.c_str());
                }
                PostRequestResult(request);
                FinishWorkerTask();
            },
            "english_result", 8192, context) != pdPASS) {
        s_busy.store(false, std::memory_order_release);
        delete context;
        SetStatus(I18n::T("保存结果失败"));
    }
}

void OnTaskClicked(lv_event_t* event) {
    const auto index = static_cast<size_t>(reinterpret_cast<uintptr_t>(
        lv_event_get_user_data(event)));
    if (index >= s_tasks.size()) {
        return;
    }
    const std::string task_id = s_tasks[index].id;
    if (RestoreReviewProgress(task_id)) {
        BuildReview();
        SetStatus(I18n::T("已恢复上次复习进度"));
        return;
    }
    s_review = EnglishReview{};
    s_current_index = 0;
    s_stage = 0;
    ShowCenteredMessage(I18n::T("正在加载复习内容..."), false, true);
    SetStatus(I18n::T("正在加载复习内容..."));
    StartReviewWorker(task_id);
}

void OnCardClicked(lv_event_t* /*event*/) {
    s_stage = (s_stage + 1) % 3;
    UpdateCard();
}

void OnPronounceClicked(lv_event_t* /*event*/) {
    if (s_current_index >= s_review.entries.size()) {
        return;
    }
    std::string error;
    const EnglishEntry& entry = s_review.entries[s_current_index];
    const std::string pronounce_text =
        entry.phrase.empty() ? entry.term : entry.phrase;
    const std::string url = EnglishClient::PronunciationUrl(pronounce_text, error);
    if (url.empty()) {
        SetStatus(error.c_str());
        return;
    }
    PodcastPlayer::SetEventCallback(nullptr, nullptr);
    if (!PodcastPlayer::Start(url, error)) {
        SetStatus(error.c_str());
    }
}

void OnChoiceClicked(lv_event_t* event) {
    if (s_choice_locked || s_current_index >= s_review.entries.size()) {
        return;
    }
    const EnglishEntry& entry = s_review.entries[s_current_index];
    if (s_choice_correct < 0 || s_choice_correct >= 4 ||
        s_choice_correct >= static_cast<int>(entry.options.size())) {
        return;
    }
    const int selected = static_cast<int>(reinterpret_cast<uintptr_t>(
        lv_event_get_user_data(event)));
    if (selected < 0 || selected >= static_cast<int>(entry.options.size())) {
        return;
    }

    s_choice_locked = true;
    const bool was_last = s_current_index + 1 >= s_review.entries.size();
    lv_obj_t* selected_button =
        static_cast<lv_obj_t*>(lv_event_get_target(event));
    lv_obj_t* correct_button =
        s_choice_correct < 4 ? s_choice_buttons[s_choice_correct] : nullptr;
    PodcastPlayer::Stop();
    if (selected == s_choice_correct) {
        lv_obj_set_style_bg_color(selected_button,
                                  lv_color_hex(kColorSuccess), LV_PART_MAIN);
        StartSubmitWorker("remembered");
    } else {
        lv_obj_set_style_bg_color(selected_button,
                                  lv_color_hex(kColorDanger), LV_PART_MAIN);
        if (correct_button != nullptr) {
            lv_obj_set_style_bg_color(correct_button,
                                      lv_color_hex(kColorSuccess),
                                      LV_PART_MAIN);
        }
        StartSubmitWorker("forgotten");
    }
    (void)was_last;
    s_choice_feedback_timer = lv_timer_create(
        [](lv_timer_t* timer) {
            lv_timer_delete(timer);
            AdvanceAfterChoiceFeedback();
        },
        kChoiceFeedbackMs, nullptr);
    lv_timer_set_repeat_count(s_choice_feedback_timer, 1);
}

void OnBackToListClicked(lv_event_t* /*event*/) {
    SaveReviewProgress();
    s_review = EnglishReview{};
    s_current_task_id.clear();
    s_current_index = 0;
    s_stage = 0;
    BuildTaskList();
    SetStatus(s_tasks.empty() ? I18n::T("还未收到任务")
                              : I18n::T("选择任务开始复习"));
}

void OnRememberedClicked(lv_event_t* /*event*/) {
    StartSubmitWorker("remembered");
}

void OnForgottenClicked(lv_event_t* /*event*/) {
    StartSubmitWorker("forgotten");
}

void ReturnHome() {
    s_session.fetch_add(1, std::memory_order_acq_rel);
    s_request_generation.fetch_add(1, std::memory_order_acq_rel);
    s_screen_active.store(false, std::memory_order_release);

    lv_obj_t* old_screen = lv_screen_active();
    lv_obj_t* target = s_return_screen;
    s_return_screen = nullptr;
    if (target == nullptr || target == old_screen) {
        target = HomeScreen::Create();
    }
    lv_screen_load(target);
    if (target == ChatScreen::CurrentScreen()) {
        ChatScreen::ResumeAfterTemporaryLeave();
    }
    if (old_screen != nullptr && old_screen != target) {
        lv_obj_delete_async(old_screen);
    }
}

// 复习中先回任务列表，列表页再点才离开英语页。
void HandleBack() {
    if (s_in_review) {
        SaveReviewProgress();
        s_review = EnglishReview{};
        s_current_task_id.clear();
        s_current_index = 0;
        s_stage = 0;
        BuildTaskList();
        SetStatus(I18n::T("选择任务开始复习"));
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
    CancelChoiceFeedback();
    s_bound_screen = nullptr;
    PodcastPlayer::Stop();
    s_screen_active.store(false, std::memory_order_release);
    s_busy.store(false, std::memory_order_release);
    s_session.fetch_add(1, std::memory_order_acq_rel);
    s_request_generation.fetch_add(1, std::memory_order_acq_rel);
    s_tasks.clear();
    s_review = EnglishReview{};
    s_current_task_id.clear();
    s_current_index = 0;
    s_stage = 0;
    s_in_review = false;
    s_ui = EnglishUi{};
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
    lv_label_set_text(title, I18n::T("一丢英语"));
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(kColorText),
                                LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);
    screen_make_input_passive(title);
}

}  // namespace

lv_obj_t* EnglishScreen::Create(lv_obj_t* return_screen) {
    lv_obj_t* screen = lv_obj_create(nullptr);
    lv_obj_set_size(screen, kPanelSize, kPanelSize);
    lv_obj_set_style_bg_color(screen, lv_color_hex(kColorBg),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    s_ui = EnglishUi{};
    s_ui.screen = screen;
    s_return_screen = return_screen;
    s_tasks.clear();
    s_review = EnglishReview{};
    s_current_task_id.clear();
    s_current_index = 0;
    s_stage = 0;
    s_in_review = false;

    BuildHeader(screen);

    s_ui.content = lv_obj_create(screen);
    lv_obj_set_pos(s_ui.content, kContentX, kContentY);
    lv_obj_set_size(s_ui.content, kContentWidth, kContentHeight);
    lv_obj_set_style_bg_color(s_ui.content,
                              lv_color_hex(kColorPanel), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.content, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_ui.content, 22, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ui.content, 0, LV_PART_MAIN);

    s_ui.status = lv_label_create(screen);
    lv_label_set_text(s_ui.status, I18n::T("正在加载任务..."));
    lv_obj_set_width(s_ui.status, 320);
    lv_label_set_long_mode(s_ui.status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_ui.status, &font_puhui_20_4,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.status, lv_color_hex(kColorSubtle),
                                LV_PART_MAIN);
    lv_obj_set_style_text_align(s_ui.status, LV_TEXT_ALIGN_RIGHT,
                                LV_PART_MAIN);
    lv_obj_align(s_ui.status, LV_ALIGN_TOP_RIGHT, -24, 60);
    screen_make_input_passive(s_ui.status);

    s_bound_screen = screen;
    lv_obj_add_event_cb(screen, OnScreenUnloaded,
                        LV_EVENT_SCREEN_UNLOADED, nullptr);
    screen_attach_swipe_back(screen, OnSwipeBack);
    return screen;
}

void EnglishScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: english_screen");
        Application::GetInstance().SetVoiceUiDesired(false);
        s_screen_active.store(true, std::memory_order_release);
        s_busy.store(false, std::memory_order_release);
        s_session.fetch_add(1, std::memory_order_acq_rel);
        s_request_generation.fetch_add(1, std::memory_order_acq_rel);
        s_tasks.clear();
        s_review = EnglishReview{};
        s_current_task_id.clear();
        s_current_index = 0;
        s_stage = 0;
        s_in_review = false;
        if (!s_cache_valid) {
            LoadTasksCache();
        }
        if (s_cache_valid) {
            s_tasks = s_cached_tasks;
            BuildTaskList();
            SetStatus(I18n::T("正在刷新任务..."));
        } else {
            BuildTaskList();
            SetStatus(I18n::T("正在加载任务..."));
        }
        StartTasksWorker();
    } else {
        ESP_LOGI(TAG, "unload: english_screen");
        s_screen_active.store(false, std::memory_order_release);
        s_busy.store(false, std::memory_order_release);
        s_session.fetch_add(1, std::memory_order_acq_rel);
        s_request_generation.fetch_add(1, std::memory_order_acq_rel);
    }
}
