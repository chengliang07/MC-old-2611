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
#include "pet_widget.h"
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
    lv_obj_t* left_eye = nullptr;
    lv_obj_t* right_eye = nullptr;
    lv_obj_t* mouth = nullptr;
    lv_obj_t* left_brow = nullptr;
    lv_obj_t* right_brow = nullptr;
    lv_obj_t* core = nullptr;
    lv_obj_t* antenna_tip = nullptr;
    lv_obj_t* mark = nullptr;
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
    lv_timer_t* blink_timer = nullptr;
    lv_obj_t* pet_widget = nullptr;
    bool use_image_art = true;
};

enum class PetMood : uint8_t {
    Idle,
    Sleep,
    Curious,
    Think,
    Happy,
    Celebrate,
    Listen,
};

const char* PetImagePath(PetMood mood, bool egg) {
    (void)mood;
    if (egg) {
        return "A:ic_pet_egg.spng";
    }
    switch (mood) {
        case PetMood::Sleep:
            return "A:ic_pet_sleep.spng";
        case PetMood::Curious:
            return "A:ic_pet_curious.spng";
        case PetMood::Think:
            return "A:ic_pet_think.spng";
        case PetMood::Happy:
        case PetMood::Celebrate:
            return "A:ic_pet_happy.spng";
        case PetMood::Listen:
            return "A:ic_pet_listening.spng";
        case PetMood::Idle:
        default:
            return "A:ic_pet_idle.spng";
    }
}

const char* BlinkImagePath(uint8_t step) {
    return step == 0 ? "A:ic_pet_blink_half.spng" : "A:ic_pet_blink.spng";
}

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
uint8_t s_idle_phase = 0;
uint8_t s_blink_step = 0;
PetMood s_mood = PetMood::Idle;
PetMood s_base_mood = PetMood::Idle;
int32_t s_eye_cx[2] = {0, 0};
int32_t s_eye_cy = 0;
int32_t s_eye_w = 30;
int32_t s_eye_h = 38;
int32_t s_mouth_cx = 150;
int32_t s_mouth_cy = 150;
int32_t s_brow_cy = 90;
bool s_has_brows = false;
std::atomic<bool> s_screen_active{false};
std::atomic<bool> s_busy{false};
std::atomic<uint32_t> s_session{0};
std::atomic<uint32_t> s_request_generation{0};

void StartIdleMotion();
bool IsEggStage();

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
    CreateBox(group, 40, 20, 120, 170, 0xFFF7E8, 60);
    CreateBox(group, 64, 42, 18, 14, 0xFFFFFF, 7);
    CreateBox(group, 86, 82, 14, 14, 0xBFE8DF, 7);
    CreateBox(group, 62, 120, 10, 10, 0xBFE8DF, 5);
    CreateBox(group, 108, 136, 12, 12, 0xFBBF24, 6);
}

void CenterAt(lv_obj_t* obj, int32_t cx, int32_t cy) {
    if (obj == nullptr) {
        return;
    }
    lv_obj_align(obj, LV_ALIGN_TOP_LEFT,
                 cx - lv_obj_get_width(obj) / 2,
                 cy - lv_obj_get_height(obj) / 2);
}

// 原创「知识小生命」：圆润短小的软体生物，胸口的知识核心随成长变大。
void DrawCreature(lv_obj_t* group, const char* stage) {
    const bool adult = std::strcmp(stage, "adult") == 0;
    const bool teen = std::strcmp(stage, "teen") == 0;

    int body_x = 80, body_y = 78, body_w = 140, body_h = 132;
    int eye_gap = 56, eye_w = 28, eye_h = 36, eye_y = 130;
    int mouth_y = 160, brow_y = 100, brow_w = 24;
    bool brows = true, arms = false, antenna = false;
    int core_d = 14, core_y = 190, glow_d = 22;
    int belly_x = 115, belly_y = 172, belly_w = 70, belly_h = 36;
    int foot_w = 30, foot_h = 12, foot_y = 204, foot_gap = 56;
    int arm_w = 0, arm_h = 0, arm_y = 0;

    if (teen) {
        body_x = 65; body_y = 58; body_w = 170; body_h = 158;
        eye_gap = 60; eye_w = 30; eye_h = 38; eye_y = 118;
        mouth_y = 148; brow_y = 92; brow_w = 28; arms = true;
        core_d = 24; core_y = 194; glow_d = 36;
        belly_x = 100; belly_y = 168; belly_w = 100; belly_h = 52;
        foot_w = 34; foot_h = 13; foot_y = 212; foot_gap = 60;
        arm_w = 30; arm_h = 54; arm_y = 132; antenna = true;
    } else if (adult) {
        body_x = 45; body_y = 45; body_w = 210; body_h = 178;
        eye_gap = 64; eye_w = 32; eye_h = 40; eye_y = 108;
        mouth_y = 142; brow_y = 82; brow_w = 30; arms = true;
        core_d = 34; core_y = 194; glow_d = 50;
        belly_x = 95; belly_y = 168; belly_w = 110; belly_h = 58;
        foot_w = 40; foot_h = 14; foot_y = 216; foot_gap = 64;
        arm_w = 34; arm_h = 62; arm_y = 126; antenna = true;
    }

    const uint32_t body_color = 0x5EC8B0;
    const uint32_t shade_color = 0x46A893;

    if (arms) {
        CreateBox(group, body_x - arm_w + 8, arm_y, arm_w, arm_h,
                  shade_color, arm_w / 2);
        CreateBox(group, body_x + body_w - 8, arm_y, arm_w, arm_h,
                  shade_color, arm_w / 2);
    }

    CreateBox(group, body_x, body_y, body_w, body_h, body_color,
              body_h / 2);
    CreateBox(group, 150 - foot_gap / 2 - foot_w / 2, foot_y, foot_w,
              foot_h, shade_color, foot_h / 2);
    CreateBox(group, 150 + foot_gap / 2 - foot_w / 2, foot_y, foot_w,
              foot_h, shade_color, foot_h / 2);

    CreateBox(group, belly_x, belly_y, belly_w, belly_h, 0xFFF7E8,
              belly_h / 2);

    if (antenna) {
        CreateBox(group, 148, adult ? 18 : 34, 4, adult ? 28 : 24,
                  shade_color, 2);
        s_ui.antenna_tip =
            CreateBox(group, 150 - (adult ? 7 : 6),
                      adult ? 7 : 22, adult ? 14 : 12, adult ? 14 : 12,
                      0xFBBF24, LV_RADIUS_CIRCLE);
        if (adult) {
            CreateBox(group, 126, 28, 6, 6, 0xFBBF24, 3);
            CreateBox(group, 168, 28, 6, 6, 0xFBBF24, 3);
        }
    }

    lv_obj_t* glow = CreateBox(group, 150 - glow_d / 2, core_y - glow_d / 2,
                               glow_d, glow_d, 0xFBBF24, glow_d / 2);
    lv_obj_set_style_bg_opa(glow, LV_OPA_30, LV_PART_MAIN);
    lv_obj_remove_flag(glow, LV_OBJ_FLAG_CLICKABLE);

    s_ui.core = CreateBox(group, 150 - core_d / 2, core_y - core_d / 2,
                          core_d, core_d, 0xFBBF24, core_d / 2);
    lv_obj_set_style_transform_pivot_x(s_ui.core, core_d / 2,
                                       LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(s_ui.core, core_d / 2,
                                       LV_PART_MAIN);
    CreateBox(s_ui.core, 3, 4, core_d / 4, core_d / 4, 0xFFFFFF,
              core_d / 8);

    s_eye_cx[0] = 150 - eye_gap / 2;
    s_eye_cx[1] = 150 + eye_gap / 2;
    s_eye_cy = eye_y;
    s_eye_w = eye_w;
    s_eye_h = eye_h;
    s_mouth_cx = 150;
    s_mouth_cy = mouth_y;
    s_brow_cy = brow_y;
    s_has_brows = brows;

    s_ui.left_eye = CreateBox(group, 0, 0, eye_w, eye_h, 0x1F2937,
                              eye_w / 2);
    CenterAt(s_ui.left_eye, s_eye_cx[0], s_eye_cy);
    CreateBox(s_ui.left_eye, 6, 7, 8, 8, 0xFFFFFF, 4);
    s_ui.right_eye = CreateBox(group, 0, 0, eye_w, eye_h, 0x1F2937,
                               eye_w / 2);
    CenterAt(s_ui.right_eye, s_eye_cx[1], s_eye_cy);
    CreateBox(s_ui.right_eye, 6, 7, 8, 8, 0xFFFFFF, 4);

    if (brows) {
        s_ui.left_brow = CreateBox(group, 0, 0, brow_w, 6, 0x1F2937, 3);
        CenterAt(s_ui.left_brow, s_eye_cx[0], brow_y);
        s_ui.right_brow = CreateBox(group, 0, 0, brow_w, 6, 0x1F2937, 3);
        CenterAt(s_ui.right_brow, s_eye_cx[1], brow_y);
    }

    s_ui.mouth = CreateBox(group, 0, 0, 18, 9, 0x1F2937, 4);
    CenterAt(s_ui.mouth, s_mouth_cx, s_mouth_cy);

    s_ui.mark = CreateText(group, 236, 40, "", 0xFBBF24, &font_puhui_30_4);
    lv_obj_add_flag(s_ui.mark, LV_OBJ_FLAG_HIDDEN);
}

void ApplyFace(PetMood mood) {
    s_mood = mood;
    if (s_ui.pet_widget != nullptr) {
        switch (mood) {
            case PetMood::Sleep:
                pet_widget_set_state(s_ui.pet_widget, PET_STATE_SLEEP);
                break;
            case PetMood::Think:
                pet_widget_set_state(s_ui.pet_widget, PET_STATE_THINKING);
                break;
            case PetMood::Listen:
                pet_widget_set_state(s_ui.pet_widget, PET_STATE_LISTENING);
                break;
            case PetMood::Happy:
            case PetMood::Celebrate:
                pet_widget_trigger_happy(s_ui.pet_widget);
                break;
            default:
                pet_widget_set_state(s_ui.pet_widget, PET_STATE_IDLE);
                break;
        }
        return;
    }
    if (s_ui.use_image_art) {
        if (s_ui.body == nullptr) {
            return;
        }
        lv_image_set_src(s_ui.body, PetImagePath(mood, IsEggStage()));
        return;
    }

    int eye_w = s_eye_w;
    int eye_h = s_eye_h;
    int eye_dy = 0;
    int mouth_w = 18;
    int mouth_h = 9;
    int mouth_dy = 0;
    int brow_l_dy = 0;
    int brow_r_dy = 0;
    bool show_brows = s_has_brows;
    const char* mark = "";

    switch (mood) {
        case PetMood::Sleep:
            eye_w += 4;
            eye_h = 6;
            mouth_w = 12;
            mouth_h = 4;
            show_brows = false;
            mark = "Z";
            break;
        case PetMood::Curious:
            eye_w += 2;
            eye_h += 6;
            eye_dy = -2;
            mouth_w = 12;
            mouth_h = 8;
            brow_l_dy = -2;
            brow_r_dy = -6;
            break;
        case PetMood::Think:
            eye_h -= 8;
            eye_dy = -6;
            mouth_w = 14;
            mouth_h = 6;
            brow_l_dy = -8;
            mark = "?";
            break;
        case PetMood::Happy:
            eye_h = 15;
            eye_dy = 2;
            mouth_w = 28;
            mouth_h = 14;
            mouth_dy = 2;
            brow_l_dy = -4;
            brow_r_dy = -4;
            break;
        case PetMood::Celebrate:
            eye_h -= 4;
            mouth_w = 30;
            mouth_h = 16;
            mouth_dy = 2;
            brow_l_dy = -8;
            brow_r_dy = -8;
            mark = "!";
            break;
        case PetMood::Listen:
            eye_w += 4;
            eye_h += 2;
            mouth_w = 14;
            mouth_h = 8;
            break;
        default:
            break;
    }

    lv_obj_t* eyes[2] = {s_ui.left_eye, s_ui.right_eye};
    for (int i = 0; i < 2; ++i) {
        lv_obj_set_size(eyes[i], eye_w, eye_h);
        lv_obj_set_style_radius(eyes[i], eye_w / 2, LV_PART_MAIN);
        CenterAt(eyes[i], s_eye_cx[i], s_eye_cy + eye_dy);
    }

    if (s_ui.mouth != nullptr) {
        lv_obj_set_size(s_ui.mouth, mouth_w, mouth_h);
        lv_obj_set_style_radius(s_ui.mouth, mouth_h / 2, LV_PART_MAIN);
        CenterAt(s_ui.mouth, s_mouth_cx, s_mouth_cy + mouth_dy);
    }

    lv_obj_t* brows[2] = {s_ui.left_brow, s_ui.right_brow};
    const int brow_dy[2] = {brow_l_dy, brow_r_dy};
    for (int i = 0; i < 2; ++i) {
        if (brows[i] == nullptr) {
            continue;
        }
        if (show_brows) {
            lv_obj_remove_flag(brows[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(brows[i], LV_OBJ_FLAG_HIDDEN);
        }
        CenterAt(brows[i], s_eye_cx[i], s_brow_cy + brow_dy[i]);
    }

    if (s_ui.mark != nullptr) {
        lv_label_set_text(s_ui.mark, mark);
        if (mark[0] == '\0') {
            lv_obj_add_flag(s_ui.mark, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_ui.mark, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void BuildPetArt(const char* stage) {
    if (s_ui.art == nullptr) {
        return;
    }
    if (s_ui.body != nullptr) {
        lv_anim_delete(s_ui.body, nullptr);
    }
    if (s_ui.core != nullptr) {
        lv_anim_delete(s_ui.core, nullptr);
    }
    lv_obj_clean(s_ui.art);
    s_ui.body = nullptr;
    s_ui.left_eye = nullptr;
    s_ui.right_eye = nullptr;
    s_ui.mouth = nullptr;
    s_ui.left_brow = nullptr;
    s_ui.right_brow = nullptr;
    s_ui.core = nullptr;
    s_ui.antenna_tip = nullptr;
    s_ui.mark = nullptr;
    s_has_brows = false;

    const bool egg = stage == nullptr || std::strcmp(stage, "egg") == 0;
    lv_obj_t* group = s_ui.use_image_art ? lv_image_create(s_ui.art)
                                         : lv_obj_create(s_ui.art);
    lv_obj_remove_style_all(group);
    lv_obj_set_style_bg_opa(group, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(group, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(group, 0, LV_PART_MAIN);
    lv_obj_add_flag(group, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_remove_flag(group, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(group, LV_OBJ_FLAG_SCROLLABLE);

    if (s_ui.use_image_art) {
        lv_image_set_src(group, PetImagePath(s_base_mood, egg));
        lv_obj_set_size(group, 360, 360);
        s_body_base_x = 150;
        // 只上移宠物本体，保持上一版的面板尺寸和缩放不变。
        s_body_base_y = -40;
        lv_obj_set_pos(group, s_body_base_x, s_body_base_y);
        if (!egg) {
            const int scale = std::strcmp(stage, "adult") == 0
                                  ? 216
                                  : std::strcmp(stage, "teen") == 0 ? 204 : 192;
            lv_obj_set_style_transform_pivot_x(group, 180, LV_PART_MAIN);
            lv_obj_set_style_transform_pivot_y(group, 180, LV_PART_MAIN);
            lv_obj_set_style_transform_scale(group, scale, LV_PART_MAIN);
        }
    } else {
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
                                ? 58
                                : std::strcmp(stage, "teen") == 0 ? 51 : 22;
            lv_obj_set_pos(group, s_body_base_x, s_body_base_y);
            DrawCreature(group, stage);
            lv_obj_set_style_transform_pivot_x(group, 150, LV_PART_MAIN);
            lv_obj_set_style_transform_pivot_y(group, 120, LV_PART_MAIN);
            lv_obj_set_style_transform_scale(group, scale, LV_PART_MAIN);
        }
    }

    s_ui.body = group;
    screen_make_input_passive(s_ui.art);
    ApplyFace(s_base_mood);
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

    const PetMood mood = state.dormant
                             ? PetMood::Sleep
                             : state.satiety < 30 ? PetMood::Think
                                                  : PetMood::Idle;
    const bool mood_changed = mood != s_base_mood;
    s_base_mood = mood;
    if (state.stage != s_stage) {
        s_stage = state.stage;
        BuildPetArt(s_stage.c_str());
    } else {
        ApplyFace(mood);
    }
    if (mood_changed && s_screen_active.load(std::memory_order_relaxed)) {
        StartIdleMotion();
    }
    UpdateActionButton();
}

void StopIdleTimer() {
    if (s_ui.idle_timer != nullptr) {
        lv_timer_delete(s_ui.idle_timer);
        s_ui.idle_timer = nullptr;
    }
    if (s_ui.blink_timer != nullptr) {
        lv_timer_delete(s_ui.blink_timer);
        s_ui.blink_timer = nullptr;
    }
    s_blink_step = 0;
    if (s_ui.body != nullptr) {
        lv_anim_delete(s_ui.body, nullptr);
    }
    if (s_ui.core != nullptr) {
        lv_anim_delete(s_ui.core, nullptr);
    }
}

bool IsEggStage() {
    return s_stage.empty() || s_stage == "egg";
}

void AnimSetY(void* var, int32_t value) {
    lv_obj_set_y(static_cast<lv_obj_t*>(var), s_body_base_y + value);
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

    if (s_mood == PetMood::Think) {
        // 思考状态保持静态，减少持续重绘。
        return;
    }

    // 普通待机只保留一种轻微上下呼吸，位移不超过 2 像素。
    const bool slow = IsEggStage() || s_mood == PetMood::Sleep;
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, body);
    lv_anim_set_exec_cb(&anim, AnimSetY);
    lv_anim_set_values(&anim, 0, -2);
    lv_anim_set_duration(&anim, slow ? 2200 : 1500);
    lv_anim_set_playback_duration(&anim, slow ? 2200 : 1500);
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
    lv_anim_set_exec_cb(&anim, AnimSetY);
    lv_anim_set_values(&anim, 0, -10);
    lv_anim_set_duration(&anim, 160);
    lv_anim_set_playback_duration(&anim, 160);
    lv_anim_set_playback_delay(&anim, 40);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_set_completed_cb(&anim, [](lv_anim_t* finished) {
        auto* pet = static_cast<lv_obj_t*>(lv_anim_get_user_data(finished));
        if (pet == nullptr) {
            return;
        }
        lv_obj_set_y(pet, s_body_base_y);
        if (pet == s_ui.body &&
            s_screen_active.load(std::memory_order_relaxed)) {
            ApplyFace(s_base_mood);
            lv_async_call(RestartIdleAsync, pet);
        }
    });
    lv_anim_start(&anim);
}

void StartIdleTimer() {
    StartIdleMotion();
    if (s_ui.idle_timer == nullptr && !s_ui.use_image_art) {
        auto on_idle_tick = [](lv_timer_t*) {
            if (!s_screen_active.load(std::memory_order_relaxed) ||
                s_ui.left_eye == nullptr ||
                s_base_mood != PetMood::Idle ||
                s_mood == PetMood::Happy ||
                s_mood == PetMood::Celebrate) {
                return;
            }
            if (!Application::GetInstance().GetAudioService().IsIdle()) {
                ApplyFace(PetMood::Listen);
                return;
            }
            s_idle_phase = (s_idle_phase + 1) % 3;
            ApplyFace(s_idle_phase == 1
                          ? PetMood::Curious
                          : s_idle_phase == 2 ? PetMood::Think
                                              : PetMood::Idle);
        };
        s_ui.idle_timer = lv_timer_create(on_idle_tick, 6000, nullptr);
        if (s_ui.idle_timer != nullptr) {
            lv_timer_set_repeat_count(s_ui.idle_timer,
                                      LV_ANIM_REPEAT_INFINITE);
        }
    }
    if (s_ui.blink_timer == nullptr) {
        auto on_blink_tick = [](lv_timer_t* timer) {
            if (!s_screen_active.load(std::memory_order_relaxed) ||
                s_ui.body == nullptr || IsEggStage() ||
                s_mood == PetMood::Sleep || s_mood == PetMood::Happy ||
                s_mood == PetMood::Celebrate) {
                s_blink_step = 0;
                lv_timer_set_period(timer, 4500);
                return;
            }
            if (s_blink_step == 0) {
                lv_image_set_src(s_ui.body, BlinkImagePath(0));
                s_blink_step = 1;
                lv_timer_set_period(timer, 90);
            } else if (s_blink_step == 1) {
                lv_image_set_src(s_ui.body, BlinkImagePath(1));
                s_blink_step = 2;
                lv_timer_set_period(timer, 90);
            } else {
                s_blink_step = 0;
                lv_image_set_src(s_ui.body,
                                 PetImagePath(s_mood, IsEggStage()));
                lv_timer_set_period(timer, 4500);
            }
        };
        s_ui.blink_timer = lv_timer_create(on_blink_tick, 4500, nullptr);
    }
}

void OnBackClicked(lv_event_t* /*event*/);
void OnStudyClicked(lv_event_t* /*event*/);
void OnActionClicked(lv_event_t* /*event*/);
void OnPetClicked(lv_event_t* /*event*/);
void OnStatsClicked(lv_event_t* /*event*/);
void OnSwipeBack();
void OnScreenUnloaded(lv_event_t* event);
void OnScreenDeleted(lv_event_t* event);
void StartWorker(RequestKind kind, bool silent = false);

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
    lv_label_set_text(s_ui.status, "");
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
                    ApplyFace(PetMood::Celebrate);
                    PlayPetRoll();
                } else if (resolved->kind == RequestKind::Feed) {
                    SetStatus(I18n::T("好吃！饱腹 +25"));
                    ApplyFace(PetMood::Happy);
                    PlayPetRoll();
                } else {
                    SetStatus("");
                }
            },
            result) != LV_RESULT_OK) {
        s_busy.store(false, std::memory_order_release);
        delete result;
    }
}

void StartWorker(RequestKind kind, bool silent) {
    auto* context = new RequestResult{};
    context->kind = kind;
    if (!BeginRequest(context)) {
        return;
    }

    if (kind == RequestKind::Load) {
        // 有缓存时静默刷新，避免重新进入页面先闪一下“加载中”。
        if (!s_has_state) {
            SetStatus(I18n::T("加载中..."));
        }
    } else if (kind == RequestKind::Feed && !silent) {
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
        // 先本地结算并给出反馈，网络请求放到后台，避免点按后干等。
        PetState optimistic = s_state;
        optimistic.food_balance -= 1;
        const int fed_satiety = optimistic.satiety + 25;
        optimistic.satiety = fed_satiety > 100 ? 100 : fed_satiety;
        SetState(optimistic);
        SetStatus(I18n::T("好吃！饱腹 +25"));
        ApplyFace(PetMood::Happy);
        PlayPetRoll();
        Application::GetInstance().PlaySound(Lang::Sounds::OGG_POPUP);
        StartWorker(RequestKind::Feed, true);
    }
}

void OnPetClicked(lv_event_t* /*event*/) {
    const uint32_t now = lv_tick_get();
    if (now - s_last_tap_ms < 400) {
        return;
    }
    s_last_tap_ms = now;
    Application::GetInstance().PlaySound(Lang::Sounds::OGG_POPUP);
    if (s_ui.pet_widget != nullptr) {
        pet_widget_trigger_happy(s_ui.pet_widget);
        return;
    }
    ApplyFace(PetMood::Happy);
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
    // 保留进程内最近一次成功获取的状态，重新进入时先显示缓存，
    // 再由 LifecycleCallback 异步刷新服务端数据。
    if (!s_has_state) {
        s_stage.clear();
    }

    BuildHeader(screen);
    BuildContent(screen);
    BuildPetArt(s_stage.empty() ? "egg" : s_stage.c_str());
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
            SetStatus("");
        } else {
            StartWorker(RequestKind::Load);
        }
    } else {
        ESP_LOGI(TAG, "unload: pet_screen");
        s_screen_active.store(false, std::memory_order_release);
        s_busy.store(false, std::memory_order_release);
        s_session.fetch_add(1, std::memory_order_acq_rel);
        s_request_generation.fetch_add(1, std::memory_order_acq_rel);
        StopIdleTimer();
    }
}
