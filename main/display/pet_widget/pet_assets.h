/*
 * pet_assets.h —— 一丢宠物图层资源表（约定文件，骨架）
 *
 * 本文件定义图层与帧的资源命名约定，与《一丢宠物-分层清单与帧表.md》
 * §4/§5 一一对应。美术按文档出图后，文件须放到 PET_ASSET_ROOT 指向的位置。
 *
 * 【重要】以下路径目前只是约定，正式资源尚未生成。先用占位图验证结构。
 */

#ifndef PET_ASSETS_H
#define PET_ASSETS_H

#ifdef __cplusplus
extern "C" {
#endif

/* 资源根目录：当前固件已确认 resources 分区 FS 字母为 'A'。 */
#ifndef PET_ASSET_ROOT
#define PET_ASSET_ROOT "A:"
#endif

static const char *const PET_IMG_BODY = PET_ASSET_ROOT "pet_body.png";

static const char *const PET_EYES_OPEN    = PET_ASSET_ROOT "pet_eyes_open.png";
static const char *const PET_EYES_HALF    = PET_ASSET_ROOT "pet_eyes_half.png";
static const char *const PET_EYES_CLOSED  = PET_ASSET_ROOT "pet_eyes_closed.png";
static const char *const PET_EYES_HAPPY   = PET_ASSET_ROOT "pet_eyes_happy.png";
static const char *const PET_EYES_WIDE    = PET_ASSET_ROOT "pet_eyes_wide.png";
static const char *const PET_EYES_TIRED   = PET_ASSET_ROOT "pet_eyes_tired.png";

static const char *const PET_EYES_BLINK_FRAMES[] = {
    PET_EYES_HALF, PET_EYES_CLOSED, PET_EYES_HALF,
};
#define PET_EYES_BLINK_FRAME_COUNT \
    (sizeof(PET_EYES_BLINK_FRAMES) / sizeof(PET_EYES_BLINK_FRAMES[0]))

static const char *const PET_IMG_HIGHLIGHT = PET_ASSET_ROOT "pet_highlight.png";

static const char *const PET_BROWS_NORMAL = PET_ASSET_ROOT "pet_brows_normal.png";
static const char *const PET_BROWS_RAISED = PET_ASSET_ROOT "pet_brows_raised.png";
static const char *const PET_BROWS_THINK  = PET_ASSET_ROOT "pet_brows_think.png";
static const char *const PET_BROWS_HAPPY  = PET_ASSET_ROOT "pet_brows_happy.png";

static const char *const PET_MOUTH_SMILE        = PET_ASSET_ROOT "pet_mouth_smile.png";
static const char *const PET_MOUTH_OPEN_BIG     = PET_ASSET_ROOT "pet_mouth_open_big.png";
static const char *const PET_MOUTH_O_SMALL      = PET_ASSET_ROOT "pet_mouth_o_small.png";
static const char *const PET_MOUTH_SPEAK_OPEN   = PET_ASSET_ROOT "pet_mouth_speak_open.png";
static const char *const PET_MOUTH_SPEAK_MID    = PET_ASSET_ROOT "pet_mouth_speak_mid.png";
static const char *const PET_MOUTH_SPEAK_CLOSED = PET_ASSET_ROOT "pet_mouth_speak_closed.png";
static const char *const PET_MOUTH_FLAT         = PET_ASSET_ROOT "pet_mouth_flat.png";
static const char *const PET_MOUTH_SLEEP        = PET_ASSET_ROOT "pet_mouth_sleep.png";

static const char *const PET_MOUTH_SPEAK_CYCLE[] = {
    PET_MOUTH_SPEAK_OPEN, PET_MOUTH_SPEAK_MID,
    PET_MOUTH_SPEAK_CLOSED, PET_MOUTH_SPEAK_MID,
};
#define PET_MOUTH_SPEAK_CYCLE_COUNT \
    (sizeof(PET_MOUTH_SPEAK_CYCLE) / sizeof(PET_MOUTH_SPEAK_CYCLE[0]))

static const char *const PET_IMG_ARM_L = PET_ASSET_ROOT "pet_arm_l.png";
static const char *const PET_IMG_ARM_R = PET_ASSET_ROOT "pet_arm_r.png";

static const char *const PET_SPROUT_STAGES[] = {
    PET_ASSET_ROOT "pet_sprout_stage0.png",
    PET_ASSET_ROOT "pet_sprout_stage1.png",
    PET_ASSET_ROOT "pet_sprout_stage2.png",
};
#define PET_SPROUT_STAGE_COUNT \
    (sizeof(PET_SPROUT_STAGES) / sizeof(PET_SPROUT_STAGES[0]))

static const char *const PET_BADGE_ZZZ     = PET_ASSET_ROOT "pet_badge_zzz.png";
static const char *const PET_BADGE_OFFLINE = PET_ASSET_ROOT "pet_badge_offline.png";

#if 0 /* FUTURE: energy core, NOT selected. Do not enable in v1. */
static const char *const PET_IMG_CORE = PET_ASSET_ROOT "pet_core.png";
#endif

#ifdef __cplusplus
}
#endif

#endif /* PET_ASSETS_H */
