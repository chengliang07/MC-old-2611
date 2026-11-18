#include "pet2_screen.h"

#include "esp_log.h"
#include "i18n.h"
#include "pet_widget.h"

namespace {

constexpr const char* TAG = "Pet2Screen";
constexpr int32_t kPanelSize = 720;

lv_obj_t* s_screen = nullptr;
lv_obj_t* s_return_screen = nullptr;
lv_obj_t* s_widget = nullptr;

void ReturnHome()
{
    lv_obj_t* old_screen = lv_screen_active();
    lv_obj_t* target = s_return_screen;
    s_return_screen = nullptr;
    if (target == nullptr || target == old_screen) {
        target = lv_obj_create(nullptr);
        lv_obj_set_size(target, kPanelSize, kPanelSize);
        lv_obj_set_style_bg_color(target, lv_color_hex(0x0B1020), LV_PART_MAIN);
    }
    s_widget = nullptr;
    lv_screen_load(target);
    if (old_screen != nullptr && old_screen != target) {
        lv_obj_delete_async(old_screen);
    }
}

void OnBack(lv_event_t* /*event*/)
{
    ReturnHome();
}

}  // namespace

lv_obj_t* Pet2Screen::Create(lv_obj_t* return_screen)
{
    lv_obj_t* screen = lv_obj_create(nullptr);
    lv_obj_set_size(screen, kPanelSize, kPanelSize);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0B1020), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    s_screen = screen;
    s_return_screen = return_screen;

    lv_obj_t* title = lv_label_create(screen);
    lv_label_set_text(title, I18n::T("一丢宠物2"));
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 18);
    screen_make_input_passive(title);

    s_widget = pet_widget_create_simple(screen);
    ESP_LOGI(TAG, "created pet2 screen");

    screen_attach_swipe_back(screen, ReturnHome);
    lv_obj_add_event_cb(screen, OnBack, LV_EVENT_KEY, nullptr);
    return screen;
}

void Pet2Screen::LifecycleCallback(screen_lifecycle_event_t event)
{
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: pet2_screen");
    } else {
        ESP_LOGI(TAG, "unload: pet2_screen");
    }
}
