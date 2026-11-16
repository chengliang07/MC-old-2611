#include "theme_manager.h"

#include "settings.h"

namespace ThemeManager {

namespace {

// NVS namespace 复用 "ui" —— 后续别的 UI 偏好（深色 / 字号 / ...）也可以
// 挂这个命名空间下，不需要单独建一个。键名加了显式前缀避免冲突。
constexpr const char* kSettingsNs = "ui";
constexpr const char* kKeyThemeId = "theme_id";

}  // namespace

int GetCurrentThemeId() {
    // 只读打开：这个函数在 LVGL 线程调用，READWRITE 的 nvs_open 会同步写
    // Flash，栈在 PSRAM 时会触发 cache 断言并重启。
    Settings reader(kSettingsNs, false);
    const bool migrated = reader.GetBool("theme_default_v2", false);
    int id = reader.GetInt(kKeyThemeId, kDefaultThemeId);
    if (id < kMinThemeId || id > kMaxThemeId) {
        id = kDefaultThemeId;
    }
    // 一次性迁移：默认主题从 1 调整为 2，老设备也要生效一次。
    // 写入交给 NVS 专用任务，调用线程不碰 Flash。
    if (!migrated) {
        Settings writer(kSettingsNs, true);
        writer.SetInt(kKeyThemeId, kDefaultThemeId);
        writer.SetBool("theme_default_v2", true);
        return kDefaultThemeId;
    }
    return id;
}

void SetCurrentThemeId(int id) {
    if (id < kMinThemeId || id > kMaxThemeId) {
        return;
    }
    Settings s(kSettingsNs, true);
    s.SetInt(kKeyThemeId, id);
}

}  // namespace ThemeManager
