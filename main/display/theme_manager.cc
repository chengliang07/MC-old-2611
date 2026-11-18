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
    // 启动/重建主屏阶段只读 NVS，禁止在 LVGL 线程写 Flash。
    Settings reader(kSettingsNs, false);
    int id = reader.GetInt(kKeyThemeId, kDefaultThemeId);
    if (id < kMinThemeId || id > kMaxThemeId) {
        id = kDefaultThemeId;
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
