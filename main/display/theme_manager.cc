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
    Settings s(kSettingsNs, true);
    // 一次性迁移：默认主题从 1 调整为 2，老设备也要生效一次。
    if (!s.GetBool("theme_default_v2", false)) {
        s.SetInt(kKeyThemeId, kDefaultThemeId);
        s.SetBool("theme_default_v2", true);
    }
    int id = s.GetInt(kKeyThemeId, kDefaultThemeId);
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
