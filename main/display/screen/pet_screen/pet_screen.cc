#include "pet_screen.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "application.h"
#include "assets/lang_config.h"
#include "chat_screen/chat_screen.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "home_screen/home_screen.h"
#include "i18n.h"
#include "pet_client.h"
#include "pet_types.h"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr const char* TAG = "PetScreen";
constexpr int32_t kPanelSize = 720;
constexpr uint32_t kColorBg = 0x0B1020;
constexpr uint32_t kColorPanel = 0x111827;
constexpr uint32_t kColorCard = 0x182033;
constexpr uint32_t kColorCardPressed = 0x24314D;
constexpr uint32_t kColorAccent = 0x60A5FA;
constexpr uint32_t kColorSuccess = 0x34D399;
constexpr uint32_t kColorWarning = 0xFBBF24;
constexpr uint32_t kColorText = 0xF8FAFC;
constexpr uint32_t kColorSubtle = 0x94A3B8;
constexpr uint32_t kColorDisabled = 0x475569;

enum class RequestKind : uint8_t {
    Load,
    Feed,
    Hatch,
};

struct PetUi {
    lv_obj_t* screen = nullptr;
    lv_obj_t* art = nullptr;
    lv_obj_t* body = nullptr;
    lv_obj_t* bubble = nullptr;
    lv_obj_t* bubble_card = nullptr;
    lv_obj_t* stats = nullptr;
    lv_obj_t* stats_toggle_label = nullptr;
    lv_obj_t* food_value = nullptr;
    lv_obj_t* satiety_value = nullptr;
    lv_obj_t* level_value = nullptr;
    lv_obj_t* action_btn = nullptr;
    lv_obj_t* action_label = nullptr;
    lv_obj_t* status = nullptr;
    lv_timer_t* idle_timer = nullptr;
};

struct RequestResult {
    uint32_t session = 0;
    uint32_t generation = 0;
    RequestKind kind = RequestKind::Load;
    bool success = false;
    PetState state;
    std::string error;
    std::string code;
};

PetUi s_ui;
lv_obj_t* s_bound_screen = nullptr;
lv_obj_t* s_return_screen = nullptr;
PetState s_state;
bool s_has_state = false;
std::string s_stage;
int32_t s_body_base_x = 0;
int32_t s_body_base_y = 0;
uint32_t s_last_tap_ms = 0;
std::atomic<bool> s_screen_active{false};
std::atomic<bool> s_busy{false};
std::atomic<uint32_t> s_session{0};
std::atomic<uint32_t> s_request_generation{0};

void StartIdleMotion();

uint32_t CurrentSession() {
    return s_session.load(std::memory_order_relaxed);
}

bool IsCurrentRequest(const RequestResult& result) {
    return s_screen_active.load(std::memory_order_relaxed) &&
           result.session == CurrentSession() &&
           result.generation ==
               s_request_generation.load(std::memory_order_relaxed);
}

lv_obj_t* CreateBox(lv_obj_t* parent, int32_t x, int32_t y, int32_t width,
                    int32_t height, uint32_t color, int32_t radius) {
    lv_obj_t* box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, width, height);
    lv_obj_set_style_bg_color(box, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(box, radius, LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(box, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(box, 0, LV_PART_MAIN);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    return box;
}

lv_obj_t* CreateText(lv_obj_t* parent, int32_t x, int32_t y,
                     const char* text, uint32_t color,
                     const lv_font_t* font) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text != nullptr ? text : "");
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return label;
}

void DrawEgg(lv_obj_t* group) {
    CreateBox(group, 40, 20, 120, 170, 0xFFF3C4, 60);
    CreateBox(group, 65, 43, 18, 14, 0xFFFFFF, 7);
    CreateText(group, 76, 70, "/", 0x8A6A2F, &font_puhui_20_4);
    CreateText(group, 92, 96, "\\", 0x8A6A2F, &font_puhui_20_4);
    CreateText(group, 76, 122, "/", 0x8A6A2F, &font_puhui_20_4);
}

void DrawChicken(lv_obj_t* group, const char* stage) {
    const bool adult = std::strcmp(stage, "adult") == 0;
    const bool teen = std::strcmp(stage, "teen") == 0;

    CreateBox(group, 28, 100, 64, 84, 0xFACC15, 32);
    CreateBox(group, 44, 122, 52, 76, 0xFDE047, 26);

    const int body_x = adult ? 48 : teen ? 58 : 70;
    const int body_y = adult ? 28 : teen ? 38 : 48;
    const int body_w = adult ? 204 : teen ? 184 : 160;
    const int body_h = adult ? 184 : teen ? 166 : 150;
    CreateBox(group, body_x, body_y, body_w, body_h, 0xFDE047,
              body_h / 2);

    const int wing_y = body_y + (adult ? 58 : teen ? 52 : 48);
    CreateBox(group, body_x - 24, wing_y, 54, 82, 0xFACC15, 27);
    CreateBox(group, body_x + body_w - 30, wing_y, 54, 82, 0xFACC15, 27);

    const int eye_y = body_y + (adult ? 58 : teen ? 52 : 46);
    const int eye_gap = adult ? 52 : teen ? 48 : 42;
    const int eye_x = body_x + body_w / 2 - eye_gap / 2 - 7;
    CreateBox(group, eye_x, eye_y, 14, 18, 0x111827, 7);
    CreateBox(group, eye_x + eye_gap, eye_y, 14, 18, 0x111827, 7);
    CreateBox(group, eye_x + 4, eye_y + 4, 5, 5, 0xFFFFFF, 3);
    CreateBox(group, eye_x + eye_gap + 4, eye_y + 4, 5, 5, 0xFFFFFF, 3);

    const int beak_y = eye_y + 28;
    CreateBox(group, body_x + body_w / 2 - 15, beak_y, 30, 18, 0xFB923C,
              9);

    const int blush_y = eye_y + 34;
    CreateBox(group, eye_x - 18, blush_y, 18, 10, 0xFCA5A5, 5);
    CreateBox(group, eye_x + eye_gap + 14, blush_y, 18, 10, 0xFCA5A5, 5);

    CreateBox(group, body_x + body_w / 2 - 46, body_y - 14, 28, 28,
              0xF87171, 14);
    CreateBox(group, body_x + body_w / 2 - 14, body_y - 24, 28, 28,
              0xF87171, 14);
    CreateBox(group, body_x + body_w / 2 + 18, body_y - 14, 28, 28,
              0xF87171, 14);

    const int foot_y = body_y + body_h - 2;
    CreateBox(group, body_x + body_w / 2 - 46, foot_y, 38, 9, 0xFB923C, 4);
    CreateBox(group, body_x + body_w / 2 + 8, foot_y, 38, 9, 0xFB923C, 4);
}

void BuildPetArt(const char* stage) {
    if (s_ui.art == nullptr) {
        return;
    }
    if (s_ui.body != nullptr) {
        lv_anim_delete(s_ui.body, nullptr);
    }
    lv_obj_clean(s_ui.art);
    s_ui.body = nullptr;

    const bool egg = stage == nullptr || std::strcmp(stage, "egg") == 0;
    lv_obj_t* group = lv_obj_create(s_ui.art);
    lv_obj_remove_style_all(group);
    lv_obj_set_style_bg_opa(group, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(group, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(group, 0, LV_PART_MAIN);
    lv_obj_add_flag(group, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_remove_flag(group, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(group, LV_OBJ_FLAG_SCROLLABLE);

    if (egg) {
        lv_obj_set_size(group, 200, 230);
        s_body_base_x = 230;
        s_body_base_y = 64;
        lv_obj_set_pos(group, s_body_base_x, s_body_base_y);
        lv_obj_set_style_transform_pivot_x(group, 100, LV_PART_MAIN);
        lv_obj_set_style_transform_pivot_y(group, 115, LV_PART_MAIN);
        lv_obj_set_style_transform_scale(group, 480, LV_PART_MAIN);
        DrawEgg(group);
    } else {
        lv_obj_set_size(group, 300, 240);
        const int scale = std::strcmp(stage, "adult") == 0
                              ? 400
                              : std::strcmp(stage, "teen") == 0 ? 360 : 320;
        s_body_base_x = 180;
        s_body_base_y = std::strcmp(stage, "adult") == 0
                            ? 68
                            : std::strcmp(stage, "teen") == 0 ? 52 : 34;
        lv_obj_set_pos(group, s_body_base_x, s_body_base_y);
        DrawChicken(group, stage);
        lv_obj_set_style_transform_pivot_x(group, 150, LV_PART_MAIN);
        lv_obj_set_style_transform_pivot_y(group, 120, LV_PART_MAIN);
        lv_obj_set_style_transform_scale(group, scale, LV_PART_MAIN);
    }

    s_ui.body = group;
    screen_make_input_passive(s_ui.art);
    if (s_screen_active.load(std::memory_order_relaxed)) {
        StartIdleMotion();
    }
}

void SetStatus(const char* text) {
    if (s_ui.status != nullptr) {
        lv_label_set_text(s_ui.status, text != nullptr ? text : "");
    }
}

void SetButtonColor(lv_obj_t* button, uint32_t color) {
    if (button == nullptr) {
        return;
    }
    lv_obj_set_style_bg_color(button, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(color),
                              LV_PART_MAIN | LV_STATE_PRESSED);
}

void UpdateActionButton() {
    if (s_ui.action_btn == nullptr || s_ui.action_label == nullptr) {
        return;
    }

    lv_obj_t* button = s_ui.action_btn;
    lv_obj_t* label = s_ui.action_label;
    bool enabled = false;
    uint32_t color = kColorDisabled;
    uint32_t label_color = kColorSubtle;
    char text[48];

    if (s_busy.load(std::memory_order_relaxed)) {
        std::snprintf(text, sizeof(text), "%s", I18n::T("请稍候..."));
    } else if (!s_has_state) {
        std::snprintf(text, sizeof(text), "%s", I18n::T("重试加载"));
        enabled = true;
        color = kColorAccent;
        label_color = kColorBg;
    } else if (!s_state.hatched) {
        std::snprintf(text, sizeof(text), "%s (%d/%d)",
                      I18n::T("孵化"), s_state.food_balance,
                      s_state.hatch_food_required);
        enabled = s_state.food_balance >= s_state.hatch_food_required;
        color = enabled ? kColorSuccess : kColorDisabled;
        label_color = enabled ? kColorBg : kColorSubtle;
    } else if (s_state.food_balance < 1) {
        std::snprintf(text, sizeof(text), "%s", I18n::T("粮食不足"));
    } else if (s_state.satiety >= 90) {
        std::snprintf(text, sizeof(text), "%s", I18n::T("已经吃饱"));
    } else {
        std::snprintf(text, sizeof(text), "%s %s",
                      s_state.dormant ? I18n::T("唤醒") : I18n::T("喂食"),
                      I18n::T("-1 粮"));
        enabled = true;
        color = kColorSuccess;
        label_color = kColorBg;
    }

    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(label_color),
                                LV_PART_MAIN);
    SetButtonColor(button, color);
    if (enabled) {
        lv_obj_remove_state(button, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(button, LV_STATE_DISABLED);
    }
}

const char* MoodText(const PetState& state) {
    if (!state.hatched) {
        return I18n::T("复习英语，攒够 3 粮就能孵化");
    }
    if (state.dormant) {
        return I18n::T("我睡着了，喂点粮食叫醒我");
    }
    if (state.satiety < 30) {
        return I18n::T("肚子咕咕叫，想吃饭");
    }
    return nullptr;
}

void SetState(const PetState& state) {
    s_state = state;
    s_has_state = true;

    if (s_ui.food_value != nullptr) {
        char text[32];
        std::snprintf(text, sizeof(text), "%d/%d", state.food_balance,
                      state.max_food);
        lv_label_set_text(s_ui.food_value, text);
    }
    if (s_ui.satiety_value != nullptr) {
        char text[32];
        std::snprintf(text, sizeof(text), "%d%%", state.satiety);
        lv_label_set_text(s_ui.satiety_value, text);
    }
    if (s_ui.level_value != nullptr) {
        char text[32];
        if (state.hatched) {
            std::snprintf(text, sizeof(text), "Lv.%d", state.level);
        } else {
            std::snprintf(text, sizeof(text), "%s", I18n::T("蛋"));
        }
        lv_label_set_text(s_ui.level_value, text);
    }
    if (s_ui.bubble != nullptr && s_ui.bubble_card != nullptr) {
        const char* mood = MoodText(state);
        if (mood != nullptr && mood[0] != '\0') {
            lv_label_set_text(s_ui.bubble, mood);
            lv_obj_remove_flag(s_ui.bubble_card, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_ui.bubble_card, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (state.stage != s_stage) {
        s_stage = state.stage;
        BuildPetArt(s_stage.c_str());
    }
    UpdateActionButton();
}

void StopIdleTimer() {
    if (s_ui.idle_timer != nullptr) {
        lv_timer_delete(s_ui.idle_timer);
        s_ui.idle_timer = nullptr;
    }
    if (s_ui.body != nullptr) {
        lv_anim_delete(s_ui.body, nullptr);
    }
}

bool IsEggStage() {
    return s_stage.empty() || s_stage == "egg";
}

void AnimSetX(void* var, int32_t value) {
    lv_obj_set_x(static_cast<lv_obj_t*>(var), s_body_base_x + value);
}

void AnimSetY(void* var, int32_t value) {
    lv_obj_set_y(static_cast<lv_obj_t*>(var), s_body_base_y + value);
}

void AnimSetSpin(void* var, int32_t value) {
    lv_obj_set_style_transform_rotation(static_cast<lv_obj_t*>(var), value,
                                        LV_PART_MAIN);
}

void StartIdleMotion() {
    lv_obj_t* body = s_ui.body;
    if (body == nullptr) {
        return;
    }

    lv_anim_delete(body, nullptr);
    lv_obj_set_x(body, s_body_base_x);
    lv_obj_set_y(body, s_body_base_y);
    lv_obj_set_style_transform_rotation(body, 0, LV_PART_MAIN);

    lv_anim_t anim;
    if (IsEggStage()) {
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, body);
        lv_anim_set_exec_cb(&anim, AnimSetSpin);
        lv_anim_set_values(&anim, -70, 70);
        lv_anim_set_duration(&anim, 900);
        lv_anim_set_playback_duration(&anim, 900);
        lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
        lv_anim_start(&anim);
        return;
    }

    lv_anim_init(&anim);
    lv_anim_set_var(&anim, body);
    lv_anim_set_exec_cb(&anim, AnimSetX);
    lv_anim_set_values(&anim, -36, 36);
    lv_anim_set_duration(&anim, 1100);
    lv_anim_set_playback_duration(&anim, 1100);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_start(&anim);

    lv_anim_init(&anim);
    lv_anim_set_var(&anim, body);
    lv_anim_set_exec_cb(&anim, AnimSetY);
    lv_anim_set_values(&anim, 0, -8);
    lv_anim_set_duration(&anim, 280);
    lv_anim_set_playback_duration(&anim, 280);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_start(&anim);

    lv_anim_init(&anim);
    lv_anim_set_var(&anim, body);
    lv_anim_set_exec_cb(&anim, AnimSetSpin);
    lv_anim_set_values(&anim, -28, 28);
    lv_anim_set_duration(&anim, 1100);
    lv_anim_set_playback_duration(&anim, 1100);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_start(&anim);
}

void RestartIdleAsync(void* arg) {
    auto* pet = static_cast<lv_obj_t*>(arg);
    if (pet == s_ui.body && s_screen_active.load(std::memory_order_relaxed)) {
        StartIdleMotion();
    }
}

void PlayPetRoll() {
    lv_obj_t* body = s_ui.body;
    if (body == nullptr) {
        return;
    }
    lv_anim_delete(body, nullptr);

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, body);
    lv_anim_set_user_data(&anim, body);
    lv_anim_set_exec_cb(&anim, AnimSetSpin);
    lv_anim_set_values(&anim, 0, 3600);
    lv_anim_set_duration(&anim, IsEggStage() ? 700 : 820);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_set_completed_cb(&anim, [](lv_anim_t* finished) {
        auto* pet = static_cast<lv_obj_t*>(lv_anim_get_user_data(finished));
        if (pet == nullptr) {
            return;
        }
        lv_obj_set_style_transform_rotation(pet, 0, LV_PART_MAIN);
        if (pet == s_ui.body &&
            s_screen_active.load(std::memory_order_relaxed)) {
            lv_async_call(RestartIdleAsync, pet);
        }
    });
    lv_anim_start(&anim);
}

void StartIdleTimer() {
    StartIdleMotion();
}

void OnBackClicked(lv_event_t* /*event*/);
void OnStudyClicked(lv_event_t* /*event*/);
void OnActionClicked(lv_event_t* /*event*/);
void OnPetClicked(lv_event_t* /*event*/);
void OnStatsClicked(lv_event_t* /*event*/);
void OnSwipeBack();
void OnScreenUnloaded(lv_event_t* event);
void OnScreenDeleted(lv_event_t* event);
void StartWorker(RequestKind kind);

lv_obj_t* CreateButton(lv_obj_t* parent, int32_t x, int32_t y,
                       int32_t width, int32_t height, const char* text,
                       uint32_t color, lv_event_cb_t callback,
                       lv_obj_t** label_out) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_radius(button, 18, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 0, LV_PART_MAIN);
    SetButtonColor(button, color);
    screen_swipe_back_ignore(button, true);
    if (callback != nullptr) {
        lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
    }

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text != nullptr ? text : "");
    lv_obj_set_style_text_font(label, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(kColorBg),
                                LV_PART_MAIN);
    lv_obj_center(label);
    if (label_out != nullptr) {
        *label_out = label;
    }
    return button;
}

void BuildHeader(lv_obj_t* screen) {
    lv_obj_t* back = lv_button_create(screen);
    lv_obj_set_size(back, 58, 58);
    lv_obj_set_pos(back, 22, 14);
    lv_obj_set_style_radius(back, 18, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x1B2740),
                              LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(back, 0, LV_PART_MAIN);
    screen_swipe_back_ignore(back, true);
    lv_obj_add_event_cb(back, OnBackClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* back_label = lv_label_create(back);
    lv_label_set_text(back_label, "<");
    lv_obj_set_style_text_font(back_label, &font_puhui_30_4,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(back_label, lv_color_hex(kColorText),
                                LV_PART_MAIN);
    lv_obj_center(back_label);

    lv_obj_t* title = lv_label_create(screen);
    lv_label_set_text(title, I18n::T("一丢宠物"));
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(kColorText),
                                LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);
    screen_make_input_passive(title);
}

void BuildStatRow(lv_obj_t* parent, int32_t y, const char* caption,
                  lv_obj_t** value_out) {
    lv_obj_t* caption_label = lv_label_create(parent);
    lv_label_set_text(caption_label, caption);
    lv_obj_set_style_text_font(caption_label, &font_puhui_20_4,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(caption_label,
                                lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_align(caption_label, LV_ALIGN_TOP_LEFT, 18, y + 8);

    lv_obj_t* value = lv_label_create(parent);
    lv_label_set_text(value, "-");
    lv_obj_set_style_text_font(value, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(value, lv_color_hex(kColorText),
                                LV_PART_MAIN);
    lv_obj_align(value, LV_ALIGN_TOP_RIGHT, -18, y);
    if (value_out != nullptr) {
        *value_out = value;
    }
    lv_obj_remove_flag(caption_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(value, LV_OBJ_FLAG_CLICKABLE);
}

void BuildStatsPanel(lv_obj_t* screen) {
    lv_obj_t* panel = lv_obj_create(screen);
    lv_obj_remove_style_all(panel);
    lv_obj_set_pos(panel, 470, 84);
    lv_obj_set_size(panel, 230, 202);
    lv_obj_set_style_bg_color(panel, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, 22, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x2A3A5A),
                                  LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 0, LV_PART_MAIN);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);

    BuildStatRow(panel, 14, I18n::T("粮食"), &s_ui.food_value);
    BuildStatRow(panel, 76, I18n::T("饱腹"), &s_ui.satiety_value);
    BuildStatRow(panel, 138, I18n::T("等级"), &s_ui.level_value);

    s_ui.stats = panel;
    screen_make_input_passive(panel);
}

void BuildContent(lv_obj_t* screen) {
    lv_obj_t* stage = lv_obj_create(screen);
    lv_obj_remove_style_all(stage);
    lv_obj_set_pos(stage, 20, 84);
    lv_obj_set_size(stage, 680, 372);
    lv_obj_set_style_bg_color(stage, lv_color_hex(kColorPanel),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(stage, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(stage, 28, LV_PART_MAIN);
    lv_obj_set_style_border_width(stage, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(stage, 0, LV_PART_MAIN);
    lv_obj_add_flag(stage, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_remove_flag(stage, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(stage, LV_OBJ_FLAG_SCROLLABLE);

    s_ui.art = lv_obj_create(stage);
    lv_obj_remove_style_all(s_ui.art);
    lv_obj_set_pos(s_ui.art, 10, 8);
    lv_obj_set_size(s_ui.art, 660, 352);
    lv_obj_set_style_bg_opa(s_ui.art, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ui.art, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_ui.art, 0, LV_PART_MAIN);
    lv_obj_add_flag(s_ui.art, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_remove_flag(s_ui.art, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_ui.art, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* hit = lv_obj_create(stage);
    lv_obj_remove_style_all(hit);
    lv_obj_set_pos(hit, 70, 8);
    lv_obj_set_size(hit, 540, 356);
    lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(hit, 0, LV_PART_MAIN);
    lv_obj_remove_flag(hit, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(hit, OnPetClicked, LV_EVENT_CLICKED, nullptr);

    s_ui.bubble_card = lv_obj_create(screen);
    lv_obj_t* bubble_card = s_ui.bubble_card;
    lv_obj_remove_style_all(bubble_card);
    lv_obj_set_pos(bubble_card, 60, 464);
    lv_obj_set_size(bubble_card, 600, 54);
    lv_obj_set_style_bg_color(bubble_card, lv_color_hex(0x1B2740),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bubble_card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bubble_card, 20, LV_PART_MAIN);
    lv_obj_set_style_border_width(bubble_card, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bubble_card, 0, LV_PART_MAIN);
    lv_obj_remove_flag(bubble_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(bubble_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(bubble_card, LV_OBJ_FLAG_HIDDEN);

    s_ui.bubble = lv_label_create(bubble_card);
    lv_label_set_text(s_ui.bubble, "");
    lv_label_set_long_mode(s_ui.bubble, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_ui.bubble, 560);
    lv_obj_set_style_text_font(s_ui.bubble, &font_puhui_20_4,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.bubble, lv_color_hex(kColorText),
                                LV_PART_MAIN);
    lv_obj_set_style_text_align(s_ui.bubble, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_obj_center(s_ui.bubble);
    screen_make_input_passive(s_ui.bubble);

    BuildStatsPanel(screen);
    CreateButton(screen, 590, 16, 108, 56, I18n::T("属性"), kColorAccent,
                 OnStatsClicked, &s_ui.stats_toggle_label);

    s_ui.action_btn = CreateButton(
        screen, 40, 528, 310, 72, I18n::T("孵化"), kColorSuccess,
        OnActionClicked, &s_ui.action_label);
    CreateButton(screen, 370, 528, 310, 72, I18n::T("去复习"),
                 kColorAccent, OnStudyClicked, nullptr);

    s_ui.status = lv_label_create(screen);
    lv_label_set_text(s_ui.status, I18n::T("加载中..."));
    lv_obj_set_width(s_ui.status, 640);
    lv_label_set_long_mode(s_ui.status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_ui.status, &font_puhui_20_4,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(s_ui.status, lv_color_hex(kColorSubtle),
                                LV_PART_MAIN);
    lv_obj_set_style_text_align(s_ui.status, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_obj_align(s_ui.status, LV_ALIGN_TOP_MID, 0, 614);
    screen_make_input_passive(s_ui.status);
}

bool BeginRequest(RequestResult*& context) {
    bool expected = false;
    if (!s_busy.compare_exchange_strong(expected, true)) {
        delete context;
        return false;
    }
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

const char* ErrorText(const RequestResult& result) {
    if (result.code == "E_EGG_NOT_HATCHED") {
        return I18n::T("宠物还在蛋里，请先孵化");
    }
    if (result.code == "E_NOT_ENOUGH_FOOD_FOR_HATCH") {
        return I18n::T("粮食不足，无法孵化");
    }
    if (result.code == "E_ALREADY_HATCHED") {
        return I18n::T("宠物已经孵化");
    }
    if (result.code == "E_NO_FOOD") {
        return I18n::T("粮食不足，去复习英语吧");
    }
    if (result.code == "E_NOT_HUNGRY") {
        return I18n::T("宠物还不饿");
    }
    if (!result.error.empty()) {
        return result.error.c_str();
    }
    return I18n::T("宠物服务暂时不可用");
}

void PostRequestResult(RequestResult* result) {
    if (lv_async_call(
            [](void* data) {
                std::unique_ptr<RequestResult> resolved(
                    static_cast<RequestResult*>(data));
                if (!IsCurrentRequest(*resolved)) {
                    return;
                }
                s_busy.store(false, std::memory_order_release);
                if (!resolved->success) {
                    SetStatus(ErrorText(*resolved));
                    UpdateActionButton();
                    return;
                }

                SetState(resolved->state);
                if (resolved->kind == RequestKind::Hatch) {
                    SetStatus(I18n::T("破壳啦！"));
                } else if (resolved->kind == RequestKind::Feed) {
                    SetStatus(I18n::T("好吃！饱腹 +25"));
                } else {
                    SetStatus("");
                }
            },
            result) != LV_RESULT_OK) {
        s_busy.store(false, std::memory_order_release);
        delete result;
    }
}

void StartWorker(RequestKind kind) {
    auto* context = new RequestResult{};
    context->kind = kind;
    if (!BeginRequest(context)) {
        return;
    }

    if (kind == RequestKind::Load) {
        SetStatus(I18n::T("加载中..."));
    } else if (kind == RequestKind::Feed) {
        SetStatus(I18n::T("正在喂食..."));
    } else {
        SetStatus(I18n::T("正在孵化..."));
    }
    UpdateActionButton();

    if (CreateWorkerTask(
            [](void* arg) {
                auto* result = static_cast<RequestResult*>(arg);
                if (result->kind == RequestKind::Load) {
                    result->success =
                        PetClient::Fetch(result->state, result->error);
                } else if (result->kind == RequestKind::Feed) {
                    result->success = PetClient::Feed(
                        result->state, result->error, result->code);
                } else {
                    result->success = PetClient::Hatch(
                        result->state, result->error, result->code);
                }
                PostRequestResult(result);
                FinishWorkerTask();
            },
            "pet_request", 8192, context) != pdPASS) {
        s_busy.store(false, std::memory_order_release);
        delete context;
        SetStatus(I18n::T("宠物请求启动失败"));
        UpdateActionButton();
    }
}

void OnActionClicked(lv_event_t* /*event*/) {
    if (s_busy.load(std::memory_order_relaxed)) {
        return;
    }
    if (!s_has_state) {
        StartWorker(RequestKind::Load);
        return;
    }
    if (!s_state.hatched) {
        if (s_state.food_balance >= s_state.hatch_food_required) {
            StartWorker(RequestKind::Hatch);
        }
        return;
    }
    if (s_state.food_balance >= 1 && s_state.satiety < 90) {
        StartWorker(RequestKind::Feed);
    }
}

void OnPetClicked(lv_event_t* /*event*/) {
    const uint32_t now = lv_tick_get();
    if (now - s_last_tap_ms < 400) {
        return;
    }
    s_last_tap_ms = now;
    Application::GetInstance().PlaySound(Lang::Sounds::OGG_POPUP);
    PlayPetRoll();
}

void OnStatsClicked(lv_event_t* /*event*/) {
    if (s_ui.stats == nullptr) {
        return;
    }
    const bool show = lv_obj_has_flag(s_ui.stats, LV_OBJ_FLAG_HIDDEN);
    if (show) {
        lv_obj_remove_flag(s_ui.stats, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ui.stats, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_ui.stats_toggle_label != nullptr) {
        lv_label_set_text(s_ui.stats_toggle_label,
                          show ? I18n::T("收起") : I18n::T("属性"));
    }
}

void OnStudyClicked(lv_event_t* /*event*/) {
    SetStatus(I18n::T("正在打开一丢英语..."));
    HomeScreen::RequestOpenEnglishFromCurrentScreen();
}

void ReturnHome() {
    s_session.fetch_add(1, std::memory_order_acq_rel);
    s_request_generation.fetch_add(1, std::memory_order_acq_rel);
    s_screen_active.store(false, std::memory_order_release);
    StopIdleTimer();

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

void OnBackClicked(lv_event_t* /*event*/) {
    ReturnHome();
}

void OnSwipeBack() {
    ReturnHome();
}

void OnScreenUnloaded(lv_event_t* event) {
    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(event));
    if (target != s_bound_screen) {
        return;
    }
    s_screen_active.store(false, std::memory_order_release);
    s_busy.store(false, std::memory_order_release);
    s_session.fetch_add(1, std::memory_order_acq_rel);
    s_request_generation.fetch_add(1, std::memory_order_acq_rel);
    StopIdleTimer();
}

void OnScreenDeleted(lv_event_t* event) {
    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(event));
    if (target != s_ui.screen) {
        return;
    }
    StopIdleTimer();
    s_ui = PetUi{};
    s_bound_screen = nullptr;
}

}  // namespace

lv_obj_t* PetScreen::Create(lv_obj_t* return_screen) {
    lv_obj_t* screen = lv_obj_create(nullptr);
    lv_obj_set_size(screen, kPanelSize, kPanelSize);
    lv_obj_set_style_bg_color(screen, lv_color_hex(kColorBg),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    s_ui = PetUi{};
    s_ui.screen = screen;
    s_return_screen = return_screen;
    s_has_state = false;
    s_state = PetState{};
    s_stage.clear();

    BuildHeader(screen);
    BuildContent(screen);
    BuildPetArt("egg");
    UpdateActionButton();

    s_bound_screen = screen;
    lv_obj_add_event_cb(screen, OnScreenUnloaded,
                        LV_EVENT_SCREEN_UNLOADED, nullptr);
    lv_obj_add_event_cb(screen, OnScreenDeleted, LV_EVENT_DELETE, nullptr);
    screen_attach_swipe_back(screen, OnSwipeBack);
    return screen;
}

void PetScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: pet_screen");
        Application::GetInstance().SetVoiceUiDesired(false);
        s_screen_active.store(true, std::memory_order_release);
        s_busy.store(false, std::memory_order_release);
        s_session.fetch_add(1, std::memory_order_acq_rel);
        s_request_generation.fetch_add(1, std::memory_order_acq_rel);
        StartIdleTimer();
        if (s_has_state) {
            SetState(s_state);
        }
        StartWorker(RequestKind::Load);
    } else {
        ESP_LOGI(TAG, "unload: pet_screen");
        s_screen_active.store(false, std::memory_order_release);
        s_busy.store(false, std::memory_order_release);
        s_session.fetch_add(1, std::memory_order_acq_rel);
        s_request_generation.fetch_add(1, std::memory_order_acq_rel);
        StopIdleTimer();
    }
}
