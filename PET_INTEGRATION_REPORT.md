# 一丢宠物接入与验证状态

基线：`MetalioClaw4` 本地工作副本 `_mc4_build106`，HEAD `29490fc`，`ca3aa3f` 为其祖先提交。

## §0 仓库事实复核

- 成立：LVGL 9.3.0、IDF `>=5.5.2`、`LV_COLOR_DEPTH=24`、单软件绘制单元、FS/DECODER 开启、`CONFIG_LV_USE_ANIMIMG` 原未启用、EAF `^0.3.0`、FS 字母 `A`、6 类表情映射。
- 不成立/需更新：当前 HEAD 已有一丢宠物屏（ca3aa3f 没有），其资源路径已用 `A:*.spng`；`DeviceStateEventManager::RegisterStateChangeCallback()` 实际是 vector 追加，文档“无反注册、只可注册一次”并非接口限制。
- 未接：设备状态桥接、情绪桥接、幅度输入、EAF 路径验证均仍待办。

## 本次最小接入

- `sdkconfig`：启用 `CONFIG_LV_USE_ANIMIMG=y`。
- `main/display/pet_widget/`：加入 `pet_widget.h/.c`、`pet_assets.h` 骨架；第一阶段只启用待机呼吸、眨眼调度、点按弹跳，占位层默认隐藏，不覆盖现有美术。
- `main/CMakeLists.txt`：编译 `pet_widget.c` 并加入 include 目录。
- `main/display/screen/pet_screen/pet_screen.cc`：旧宠物屏创建该组件；点按/孵化/喂食触发开心弹跳。
- 已定事项保持：识别点小芽、第一阶段不做能量核、画布 512×512。

## V1–V20 状态

V1 待验证：`CONFIG_LV_COLOR_DEPTH=24` 已静态确认；实际 `lv_display_get_color_format()` 和帧缓冲字节数须真机打印。

V2 待验证：`CONFIG_LV_USE_ANIMIMG=y` 与源码引用已就绪；本机无 ESP-IDF，未完成 `idf.py reconfigure && idf.py build`。

V3 待验证：呼吸定时器已接入；全分层图像与 ≥25fps/60s 未验证。

V4 待验证：独立高光层未启用；未做 A/B 帧率对比。

V5 待验证：PPA fallback 未做日志/帧率观测。

V6 待验证：EAF 透明未测试。

V7 待验证：512×512 EAF 居中未测试。

V8 待验证：512/720 EAF 成本对比未测试。

V9 待验证：点按开心已接线；首帧响应 <200ms 未测量。

V10 待验证：PSRAM 三点采样未执行。

V11 待验证：拔 SD 卡 fallback 未执行。

V12 待验证：未接 `DeviceStateEventManager` 桥接，Idle→Listening→Speaking→Idle 未验证。

V13 待验证：仓库未找到幅度源；伪口型也未启用，未定集成方案。

V14 待验证：未封装 GPIO22+LEDC 点按震动。

V15 待验证：宠物屏与待机屏协调未观察。

V16 待验证：EAF 转换器上限未测试。

V17 待验证：resources 子目录未测试；本次未建 `xingzhi-assets/pet/`。

V18 已静态确认：现有固件使用 `A:` FS 字母与 `A:ic_pet_*.spng` 路径；`PET_ASSET_ROOT` 暂设 `A:`，完成边界以真机加载为准，因此仍待验证。

V19 待验证：30 分钟稳定性未执行。

V20 待验证：`dp_delay_ms` 四档 EAF 未测试。

## 真机验证清单（按 README §9 顺序）

1. 安装 ESP-IDF 5.5.x（需含 ESP32-P4 工具链），在 `_mc4_build106` 执行 `idf.py reconfigure`、`idf.py build`；记录 V2 编译结果。`CONFIG_LV_USE_ANIMIMG=y` 已写入 `sdkconfig`。
2. 烧录固件，打开一丢宠物页；记录启动日志与屏幕是否正常，观察现有宠物美术不被遮挡（占位层隐藏）。
3. 串口打印 `lv_display_get_color_format()`，记录 V1 实际格式；有条件时记录 panel 帧缓冲字节数。
4. 静置宠物页，观察呼吸节奏；统计 2.8–6s 内是否触发眨眼调度（当前占位层隐藏，只能看日志/计时，不视为视觉眨眼通过）。
5. 点按宠物，记录从触摸到弹跳首帧的时间（V9，目标 <200ms）与 900ms 后是否复位。
6. 在屏创建前、创建后、动画 60s 后分别打印 `heap_caps_get_free_size(MALLOC_CAP_SPIRAM)`（V10）。
7. 再接 `DeviceStateEventManager` 桥接前，先确认是否已有其他注册；避免重复注册导致多实例响应（V12）。
8. 8 张实验图就位后，按 `pet_assets.h` 文件名放入 `A:` 资源路径，逐张确认眼睛/嘴/小芽图片加载；每次只验证待机、眨眼、点按开心。
9. 开启所有分层图片后测 60s fps（V3），对比高光层开关（V4），观察 PPA fallback 日志（V5）。
10. 再验证 SD 卡缺失（V11）、EAF 透明/尺寸/成本（V6–V8、V16、V20）、resources 子目录与路径（V17–V18）、30 分钟稳定性（V19）、睡眠/待机屏协调（V15）。
11. 幅度源、震动封装、情绪桥接按 V13/V14/文档 §5 单独处理，当前均未实现。

## 验证结论

- 已完成静态复核和最小代码接线；`git diff --check` 通过。
- 未做编译、烧录、真机测试；V1–V20 均不得标记为完成。V18 仅有静态路径证据。
- 8 张实验图、28 张正式图和 8 个完整状态未一次性铺开。
