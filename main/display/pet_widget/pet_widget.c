/* pet_widget.c —— 一丢宠物 LVGL 组件（按文档骨架整理的最小实现）
 * 当前仅接入 8 张实验图对应的待机、眨眼、点按开心三个效果。
 * 其余状态、资源与 V1–V20 验证项保持 TODO，不得视为完成。
 */

#include "pet_widget.h"
#include "pet_assets.h"

#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_log.h"
#define PET_LOGI(...) ESP_LOGI("pet_widget", __VA_ARGS__)
#define PET_LOGW(...) ESP_LOGW("pet_widget", __VA_ARGS__)
#else
#define PET_LOGI(...) do { } while (0)
#define PET_LOGW(...) do { } while (0)
#endif

#if !LV_USE_ANIMIMG
#error "pet_widget requires LV_USE_ANIMIMG. Enable CONFIG_LV_USE_ANIMIMG=y."
#endif

#define PET_CANVAS_SIZE      512
#define PET_BODY_X           0
#define PET_BODY_Y           0
#define PET_EYES_X           104
#define PET_EYES_Y           86
#define PET_EYES_W           248
#define PET_EYES_H           56
#define PET_MOUTH_X          180
#define PET_MOUTH_Y          196
#define PET_MOUTH_W          96
#define PET_MOUTH_H          72
#define PET_SPROUT_X         216
#define PET_SPROUT_Y         0
#define PET_SPROUT_W         80
#define PET_SPROUT_H         64

#define PET_ZOOM_NORMAL      256
#define PET_ZOOM_BREATH_PEAK 264

static const void *kEyesOpenSrc[]   = { PET_EYES_OPEN };
static const void *kEyesBlinkSrc[]  = {
    PET_EYES_HALF, PET_EYES_CLOSED, PET_EYES_HALF,
};
static const void *kEyesHappySrc[]  = { PET_EYES_HAPPY };
static const void *kMouthSmileSrc[] = { PET_MOUTH_SMILE };
static const void *kMouthHappySrc[] = { PET_MOUTH_OPEN_BIG };

typedef struct pet_ctx {
    lv_obj_t *container;
    lv_obj_t *eyes;
    lv_obj_t *mouth;
    lv_obj_t *sprout;
    pet_widget_callbacks_t callbacks;
    lv_timer_t *timer;
    uint32_t next_blink;
    uint32_t transient_end;
    uint8_t breath_phase;
    int16_t bounce_y;
    uint8_t state;
    bool destroyed;
} pet_ctx_t;

static pet_ctx_t *pet_ctx_of(lv_obj_t *widget)
{
    return widget == NULL ? NULL : (pet_ctx_t *)lv_obj_get_user_data(widget);
}

static void pet_animimg_set(lv_obj_t *img, const void *const *src,
                            uint8_t count, uint32_t duration_ms)
{
    lv_animimg_set_src(img, src, count);
    lv_animimg_set_duration(img, duration_ms);
    lv_animimg_set_repeat_count(img, 1);
    lv_animimg_start(img);
}

static void pet_apply_transform(pet_ctx_t *ctx)
{
    const int32_t zoom = PET_ZOOM_NORMAL +
        (PET_ZOOM_BREATH_PEAK - PET_ZOOM_NORMAL) * ctx->breath_phase / 100;
    const int32_t y = -3 * ctx->breath_phase / 100 - ctx->bounce_y;
    lv_obj_set_style_transform_scale(ctx->container, (uint16_t)zoom,
                                     LV_PART_MAIN);
    lv_obj_set_pos(ctx->eyes, PET_EYES_X, PET_EYES_Y + y);
    lv_obj_set_pos(ctx->mouth, PET_MOUTH_X, PET_MOUTH_Y + y);
    lv_obj_set_pos(ctx->sprout, PET_SPROUT_X, PET_SPROUT_Y + y);
}

static void pet_breath_exec(void *var, int32_t value)
{
    pet_ctx_t *ctx = (pet_ctx_t *)var;
    ctx->breath_phase = (uint8_t)value;
    pet_apply_transform(ctx);
}

static void pet_bounce_exec(void *var, int32_t value)
{
    pet_ctx_t *ctx = (pet_ctx_t *)var;
    ctx->bounce_y = (int16_t)value;
    pet_apply_transform(ctx);
}

static void pet_bounce_done(lv_anim_t *anim)
{
    pet_ctx_t *ctx = (pet_ctx_t *)lv_anim_get_user_data(anim);
    if (ctx != NULL) {
        ctx->bounce_y = 0;
    }
}

static void pet_start_breath(pet_ctx_t *ctx)
{
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, ctx);
    lv_anim_set_exec_cb(&anim, pet_breath_exec);
    lv_anim_set_values(&anim, 0, 100);
    lv_anim_set_duration(&anim, PET_TIMING_BREATH_MS / 2);
    lv_anim_set_playback_duration(&anim, PET_TIMING_BREATH_MS / 2);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_start(&anim);
}

static void pet_start_bounce(pet_ctx_t *ctx)
{
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, ctx);
    lv_anim_set_user_data(&anim, ctx);
    lv_anim_set_exec_cb(&anim, pet_bounce_exec);
    lv_anim_set_values(&anim, 0, 18);
    lv_anim_set_duration(&anim, 180);
    lv_anim_set_playback_duration(&anim, 160);
    lv_anim_set_repeat_count(&anim, 1);
    lv_anim_set_completed_cb(&anim, pet_bounce_done);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_start(&anim);
}

static void pet_set_happy(pet_ctx_t *ctx)
{
    /* 8 张实验图阶段占位层保持隐藏，点按只验证弹跳时序。 */
    (void)kEyesHappySrc;
    (void)kMouthHappySrc;
    ctx->transient_end = lv_tick_get() + PET_TIMING_HAPPY_MS;
    pet_start_bounce(ctx);
}

static void pet_set_idle(pet_ctx_t *ctx)
{
    /* 占位层隐藏，不切图。 */
    (void)kEyesOpenSrc;
    (void)kMouthSmileSrc;
    ctx->transient_end = 0;
}

static void pet_do_blink(pet_ctx_t *ctx)
{
    /* 8 张实验图阶段占位层保持隐藏；眨眼调度先验证定时器节奏。 */
    (void)kEyesBlinkSrc;
    ctx->next_blink = lv_tick_get() +
        (uint32_t)(PET_TIMING_BLINK_MIN_MS +
                   rand() % (PET_TIMING_BLINK_MAX_MS -
                             PET_TIMING_BLINK_MIN_MS + 1));
}

static void pet_timer_cb(lv_timer_t *timer)
{
    pet_ctx_t *ctx = (pet_ctx_t *)lv_timer_get_user_data(timer);
    const uint32_t now = lv_tick_get();
    if (ctx == NULL || ctx->destroyed) {
        return;
    }
    if (ctx->transient_end != 0 && now >= ctx->transient_end) {
        pet_set_idle(ctx);
    }
    if (ctx->transient_end == 0 && now >= ctx->next_blink) {
        pet_do_blink(ctx);
    }
}

static void pet_container_event_cb(lv_event_t *event)
{
    lv_obj_t *widget = (lv_obj_t *)lv_event_get_target(event);
    pet_ctx_t *ctx = pet_ctx_of(widget);
    if (lv_event_get_code(event) == LV_EVENT_DELETE) {
        if (ctx != NULL) {
            ctx->destroyed = true;
            lv_anim_delete(ctx, NULL);
            if (ctx->timer != NULL) {
                lv_timer_delete(ctx->timer);
            }
            lv_free(ctx);
            lv_obj_set_user_data(widget, NULL);
        }
        return;
    }
    if (ctx == NULL || lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    if (ctx->callbacks.on_tap != NULL) {
        ctx->callbacks.on_tap(ctx->callbacks.user_data);
    }
    pet_set_happy(ctx);
}

void pet_widget_config_init(pet_widget_config_t *config, lv_obj_t *parent)
{
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->parent = parent;
    config->initial_state = PET_STATE_IDLE;
    config->sprout_enabled = true;
    config->sprout_stage = PET_SPROUT_STAGE_SPROUT;
}

lv_obj_t *pet_widget_create(const pet_widget_config_t *config)
{
    if (config == NULL || config->parent == NULL) {
        PET_LOGW("pet_widget_create: bad config");
        return NULL;
    }
    pet_ctx_t *ctx = (pet_ctx_t *)lv_malloc(sizeof(*ctx));
    if (ctx == NULL) {
        PET_LOGW("pet_widget_create: no memory");
        return NULL;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->callbacks = config->callbacks;
    ctx->state = PET_STATE_IDLE;
    ctx->next_blink = lv_tick_get() + PET_TIMING_BLINK_MIN_MS;

    lv_obj_t *container = lv_obj_create(config->parent);
    ctx->container = container;
    lv_obj_set_size(container, PET_CANVAS_SIZE, PET_CANVAS_SIZE);
    lv_obj_center(container);
    lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(container, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(container, 0, LV_PART_MAIN);
    lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(container, ctx);

    ctx->eyes = lv_animimg_create(container);
    lv_obj_set_pos(ctx->eyes, PET_EYES_X, PET_EYES_Y);
    lv_obj_set_size(ctx->eyes, PET_EYES_W, PET_EYES_H);
    pet_animimg_set(ctx->eyes, kEyesOpenSrc, 1, 100);
    lv_obj_add_flag(ctx->eyes, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(ctx->eyes, LV_OBJ_FLAG_CLICKABLE);

    ctx->mouth = lv_animimg_create(container);
    lv_obj_set_pos(ctx->mouth, PET_MOUTH_X, PET_MOUTH_Y);
    lv_obj_set_size(ctx->mouth, PET_MOUTH_W, PET_MOUTH_H);
    pet_animimg_set(ctx->mouth, kMouthSmileSrc, 1, 100);
    lv_obj_add_flag(ctx->mouth, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(ctx->mouth, LV_OBJ_FLAG_CLICKABLE);

    ctx->sprout = lv_image_create(container);
    lv_obj_set_pos(ctx->sprout, PET_SPROUT_X, PET_SPROUT_Y);
    lv_obj_set_size(ctx->sprout, PET_SPROUT_W, PET_SPROUT_H);
    lv_image_set_src(ctx->sprout, PET_SPROUT_STAGES[0]);
    lv_obj_add_flag(ctx->sprout, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(ctx->sprout, LV_OBJ_FLAG_CLICKABLE);
    if (!config->sprout_enabled) {
        lv_obj_add_flag(ctx->sprout, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_add_event_cb(container, pet_container_event_cb, LV_EVENT_ALL, NULL);
    ctx->timer = lv_timer_create(pet_timer_cb, PET_TIMING_MOUTH_TICK_MS, ctx);
    pet_start_breath(ctx);
    pet_apply_transform(ctx);
    PET_LOGI("created; placeholder layers hidden; assets=%s", PET_ASSET_ROOT);
    return container;
}

lv_obj_t *pet_widget_create_simple(lv_obj_t *parent)
{
    pet_widget_config_t config;
    pet_widget_config_init(&config, parent);
    return pet_widget_create(&config);
}

void pet_widget_destroy(lv_obj_t *widget)
{
    if (widget != NULL) {
        lv_obj_delete(widget);
    }
}

void pet_widget_trigger_happy(lv_obj_t *widget)
{
    pet_ctx_t *ctx = pet_ctx_of(widget);
    if (ctx != NULL && !ctx->destroyed) {
        pet_set_happy(ctx);
    }
}

pet_state_t pet_widget_get_state(lv_obj_t *widget)
{
    pet_ctx_t *ctx = pet_ctx_of(widget);
    return ctx == NULL ? PET_STATE_IDLE : (pet_state_t)ctx->state;
}

void pet_widget_set_state(lv_obj_t *widget, pet_state_t state)
{
    pet_ctx_t *ctx = pet_ctx_of(widget);
    if (ctx == NULL || ctx->destroyed) {
        return;
    }
    if (state == PET_STATE_HAPPY) {
        pet_set_happy(ctx);
    } else {
        ctx->state = state;
        pet_set_idle(ctx);
    }
}

/* 以下接口仅保留编译兼容；接入与验证状态见分层文档 §9。 */
void pet_widget_request_state(pet_state_t state) { (void)state; }
void pet_widget_trigger_curious(lv_obj_t *widget) { (void)widget; }
void pet_widget_set_offline(lv_obj_t *widget, bool offline)
{
    (void)widget;
    (void)offline;
}
void pet_widget_feed_audio_level(lv_obj_t *widget, uint8_t level_0_100)
{
    (void)widget;
    (void)level_0_100;
}
void pet_widget_request_audio_level(uint8_t level_0_100) { (void)level_0_100; }
void pet_widget_speaking_begin(lv_obj_t *widget) { (void)widget; }
void pet_widget_speaking_end(lv_obj_t *widget) { (void)widget; }
void pet_widget_set_sprout_enabled(lv_obj_t *widget, bool enabled)
{
    pet_ctx_t *ctx = pet_ctx_of(widget);
    if (ctx != NULL && ctx->sprout != NULL) {
        if (enabled) {
            lv_obj_remove_flag(ctx->sprout, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ctx->sprout, LV_OBJ_FLAG_HIDDEN);
        }
    }
}
void pet_widget_set_sprout_stage(lv_obj_t *widget, uint8_t stage)
{
    pet_ctx_t *ctx = pet_ctx_of(widget);
    if (ctx != NULL && ctx->sprout != NULL && stage < PET_SPROUT_STAGE_COUNT) {
        lv_image_set_src(ctx->sprout, PET_SPROUT_STAGES[stage]);
    }
}
uint8_t pet_widget_get_sprout_stage(lv_obj_t *widget)
{
    (void)widget;
    return PET_SPROUT_STAGE_SPROUT;
}
