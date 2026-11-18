/*
 * pet_widget.h —— 一丢宠物 LVGL 组件（代码骨架）
 *
 * 目标：Metalio Claw4，ESP32-P4，720×720，ESP-IDF + LVGL 9.3。
 * 配套文档：《一丢宠物-分层清单与帧表.md》，图层名、帧名、时序常量与该文档一致。
 *
 * 【骨架声明】本文件与 pet_widget.c 构成可接手的结构骨架，作者未在
 * ESP-IDF 环境中编译验证，不得直接声称可编译通过。集成前必读 README.md
 * 中的「编译前必查项」，并逐项完成分层文档 §9 的真机验证清单。
 *
 * 线程模型：
 *   - 本组件的所有 lv_obj_t 操作只能在 LVGL 线程（esp_lv_adapter 任务）内执行。
 *   - 跨线程的状态/音量请求请用 pet_widget_request_state() /
 *     pet_widget_request_audio_level()，它们只写待处理槽，由组件内部的
 *     LVGL 定时器在 LVGL 线程内消费（见 pet_widget.c）。
 *   - v1 只支持单个宠物组件实例（待处理槽是全局的）。
 *
 * 识别点：头顶小芽（用户已选定，2026-11-18）。能量核方案未选定，
 * 第一阶段不实现，本组件不提供能量核接口（预留见 pet_assets.h 的 FUTURE 区）。
 */

#ifndef PET_WIDGET_H
#define PET_WIDGET_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 状态枚举（与分层文档 §5/§6 一致）                                    */
/* ------------------------------------------------------------------ */
typedef enum {
    PET_STATE_IDLE = 0,      /* 待机：呼吸 + 眨眼 + 张望 */
    PET_STATE_LISTENING,     /* 倾听：注视 + 前倾 */
    PET_STATE_THINKING,      /* 思考：眼珠游移 + 歪头 + 小芽快摆 */
    PET_STATE_SPEAKING,      /* 说话：3 档口型轮换 */
    PET_STATE_HAPPY,         /* 开心：瞬时态，约 900ms 后回基础态 */
    PET_STATE_CURIOUS,       /* 好奇：瞬时态，最长 3s 后回待机 */
    PET_STATE_SLEEP,         /* 睡觉：闭眼 + 慢呼吸 + Z 标 */
    PET_STATE_OFFLINE,       /* 离线：疲倦眼 + 平直嘴 + 离线标 */
    PET_STATE_COUNT
} pet_state_t;

/* 小芽成长档位（与分层文档 §4.8 一致；第一阶段只用 0 档） */
#define PET_SPROUT_STAGE_SPROUT   0u  /* 嫩芽：两片小叶 + 短茎 */
#define PET_SPROUT_STAGE_LEAVES   1u  /* 两叶展开（未来） */
#define PET_SPROUT_STAGE_BLOOM    2u  /* 开花（未来） */
#define PET_SPROUT_STAGE_COUNT    3u

/* ------------------------------------------------------------------ */
/* 时序常量（与分层文档 §5 一致；调参只改这里，不改结构）                 */
/* 单位：毫秒。                                                          */
/* ------------------------------------------------------------------ */
#define PET_TIMING_BLINK_MIN_MS        2800u   /* 眨眼间隔随机下限 */
#define PET_TIMING_BLINK_MAX_MS        6000u   /* 眨眼间隔随机上限 */
#define PET_TIMING_BLINK_LISTEN_MIN_MS 5000u   /* 倾听时眨眼间隔下限 */
#define PET_TIMING_BLINK_LISTEN_MAX_MS 8000u   /* 倾听时眨眼间隔上限 */
#define PET_TIMING_BLINK_SEQ_MS        200u    /* 眨眼序列总时长 */
#define PET_TIMING_BREATH_MS           3600u   /* 呼吸周期（待机） */
#define PET_TIMING_BREATH_SLEEP_MS     5200u   /* 呼吸周期（睡觉） */
#define PET_TIMING_BREATH_OFFLINE_MS   6000u   /* 呼吸周期（离线） */
#define PET_TIMING_LOOK_MIN_MS         8000u   /* 张望间隔随机下限 */
#define PET_TIMING_LOOK_MAX_MS         15000u  /* 张望间隔随机上限 */
#define PET_TIMING_MOUTH_TICK_MS       100u    /* 说话口型刷新间隔 */
#define PET_TIMING_MOUTH_SILENCE_MS    250u    /* 静默锁定闭口的阈值 */
#define PET_TIMING_MOUTH_NOINPUT_MS    300u    /* 无幅度输入则进入伪口型循环 */
#define PET_TIMING_HAPPY_MS            900u    /* 开心瞬时态时长 */
#define PET_TIMING_HAPPY_SHORT_MS      450u    /* 开心短版（唤醒时） */
#define PET_TIMING_CURIOUS_MS          3000u   /* 好奇瞬时态最长时长 */
#define PET_TIMING_TAP_DEBOUNCE_MS     1500u   /* 点按防连点间隔 */
#define PET_TIMING_APPLY_MS            50u     /* 待处理槽消费间隔 */
#define PET_TIMING_THINK_TIMEOUT_MS    10000u  /* 思考态防卡死超时 */

/* 口型幅度阈值（0-100，见分层文档 §5.4） */
#define PET_MOUTH_LEVEL_HIGH           66u
#define PET_MOUTH_LEVEL_MID            33u

/* ------------------------------------------------------------------ */
/* 回调                                                                */
/* ------------------------------------------------------------------ */
typedef struct pet_widget_callbacks {
    /* 点按宠物时调用（LVGL 线程）。返回后组件继续默认行为（开心/唤醒）。 */
    void (*on_tap)(void *user_data);
    /* 从睡觉态被点按唤醒时调用（LVGL 线程）。 */
    void (*on_wake)(void *user_data);
    /* 基础态变化时调用（LVGL 线程）。瞬时态的进入/退出不触发。 */
    void (*on_state_changed)(pet_state_t old_state, pet_state_t new_state,
                             void *user_data);
    void *user_data;
} pet_widget_callbacks_t;

typedef struct pet_widget_config {
    lv_obj_t *parent;                /* 父对象（一般是宠物屏 scr），必填 */
    pet_state_t initial_state;       /* 初始基础态，一般 PET_STATE_IDLE */
    bool sprout_enabled;             /* 是否显示小芽（默认 true，正式识别层） */
    uint8_t sprout_stage;            /* 小芽成长档位，默认 PET_SPROUT_STAGE_SPROUT */
    pet_widget_callbacks_t callbacks;
} pet_widget_config_t;

/* 用默认值填充 config（调用方再按需改字段）。 */
void pet_widget_config_init(pet_widget_config_t *config, lv_obj_t *parent);

lv_obj_t *pet_widget_create_simple(lv_obj_t *parent);

/* ------------------------------------------------------------------ */
/* 创建 / 销毁（LVGL 线程内调用）                                       */
/* ------------------------------------------------------------------ */

/* 创建宠物组件。返回 512×512 的容器对象（在 parent 内居中）。
 * 图层全部预分配；资源路径见 pet_assets.h（文件须已就位，否则对应层空白，
 * LVGL 会打日志——集成时先用占位图验证结构，再换正式美术资源）。 */
lv_obj_t *pet_widget_create(const pet_widget_config_t *config);

/* 销毁组件：删除定时器/动画并释放内部上下文。
 * 也可以直接 lv_obj_delete(widget)，DELETE 事件里会做同样的清理。 */
void pet_widget_destroy(lv_obj_t *widget);

/* ------------------------------------------------------------------ */
/* 状态控制                                                            */
/* ------------------------------------------------------------------ */

/* 直接设置基础态（只能在 LVGL 线程内调用）。
 * 传入 HAPPY/CURIOUS 时按瞬时态处理（记住当前基础态，超时自动恢复）。 */
void pet_widget_set_state(lv_obj_t *widget, pet_state_t state);

/* 跨线程请求基础态（任意线程可调用，只写待处理槽，不碰 LVGL）。
 * 由组件内部定时器在 LVGL 线程内消费。v1 只支持单个实例。 */
void pet_widget_request_state(pet_state_t state);

/* 触发瞬时态（LVGL 线程内调用）。瞬时态进行中再次触发会被忽略。 */
void pet_widget_trigger_happy(lv_obj_t *widget);
void pet_widget_trigger_curious(lv_obj_t *widget);

/* 查询当前基础态（瞬时态进行中返回被记住的基础态）。 */
pet_state_t pet_widget_get_state(lv_obj_t *widget);

/* 离线快捷接口：offline=true → OFFLINE；false → 按当前设备状态回待机。
 * TODO(集成)：false 时应按 Application::GetDeviceState() 恢复对应态，
 * 而不是无条件回 IDLE——见 README.md 的状态机接线说明。 */
void pet_widget_set_offline(lv_obj_t *widget, bool offline);

/* ------------------------------------------------------------------ */
/* 说话口型驱动                                                         */
/* ------------------------------------------------------------------ */

/* 喂入音量幅度（0-100），只能在 LVGL 线程内调用。
 * 幅度来源 TODO：仓库内未找到 TTS/播放幅度回调（见分层文档 §9 V13）。
 * 在幅度源接通前，说话态会用伪口型循环（见 PET_TIMING_MOUTH_NOINPUT_MS）。 */
void pet_widget_feed_audio_level(lv_obj_t *widget, uint8_t level_0_100);

/* 跨线程喂入音量幅度（任意线程可调用，只写待处理槽）。
 * 注意：100ms 级的频繁跨线程喂入意义不大，集成方应在 LVGL 线程侧
 * 汇总后再喂入，或直接用 lv_async_call 汇总。 */
void pet_widget_request_audio_level(uint8_t level_0_100);

/* 通知说话开始/结束（LVGL 线程内调用）。开始时启动口型定时器，
 * 结束时停定时器并复位嘴形。一般由 set_state(SPEAKING/IDLE) 间接触发，
 * 仅在需要独立于状态机的口型控制时才直接调用。 */
void pet_widget_speaking_begin(lv_obj_t *widget);
void pet_widget_speaking_end(lv_obj_t *widget);

/* ------------------------------------------------------------------ */
/* 小芽（正式识别层）                                                   */
/* ------------------------------------------------------------------ */

/* 显示/隐藏小芽（默认显示）。隐藏只用于调试或特殊状态，一般不调用。 */
void pet_widget_set_sprout_enabled(lv_obj_t *widget, bool enabled);

/* 设置小芽成长档位（0-2）。档位资源见 pet_assets.h；档位切换时
 * 建议先触发一次开心动画再换图（调用方负责顺序）。 */
void pet_widget_set_sprout_stage(lv_obj_t *widget, uint8_t stage);

uint8_t pet_widget_get_sprout_stage(lv_obj_t *widget);

#ifdef __cplusplus
}
#endif

#endif /* PET_WIDGET_H */
