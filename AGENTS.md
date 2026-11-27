# Project Notes

## ESP32-P4 local flashing

- Preferred local flash command uses the verified venv Python (see "Verified local build" below); `uvx esptool` still works. Do not invent other Python paths.
- Example:
  `& 'C:\Users\shuocheng\.local\bin\uvx.exe' esptool --chip esp32p4 --port COM12 --baud 921600 write_flash 0x0 <merged-binary.bin>`
- `C:\Users\shuocheng\.local\bin\esptool.exe` is a broken uv trampoline and must not be used.

## Firmware build policy

- **烧录前必须先汇总所有模块分支（固定动作，不许只烧自己的改动）**：
  1. 先把本窗口的改动**提交**（工作区有未提交文件时 git 会拒绝合并，且不会有任何提示）
  2. 逐个检查 `english / oral / pet / podcast`：`git -C ../wt-<m> log --oneline main..HEAD`
  3. 有已提交内容的就 `git merge --no-ff feat/<m>`；有冲突先解决再继续
  4. **合并后必须验证**：`git merge-base --is-ancestor <被合并的提交> HEAD`，并确认输出里有 `Merge made by`
  5. 只合并**已提交**的内容；分支里还在改（未提交）的部分等它提交后再带
  6. 编译烧录后，在报告里列出本次带进来的各分支提交，让用户知道设备上有什么
  7. **编译完成后、烧录之前再扫一次分支**：模块窗口可能正好在这 1~2 分钟里提交。
     若发现新提交，重新合并并重编，不要直接烧（2026-11-27 已因此烧早一次，
     导致口语窗口的新诊断代码不在设备上）。
- Before starting a GitHub Actions firmware build or flashing, report the intended changes and ask the user for confirmation.
- Do not trigger a full firmware build for every small change; batch related fixes before building.

## Verified local build (2026-11-23, Windows)

- Use the venv Python by absolute path; `python` on PATH may not exist:
  `D:\Espressif\tools\python\v5.5.6\venv\Scripts\python.exe`
- Required environment (all of them; missing any one breaks the build):
  - `IDF_PATH=D:\esp\v5.5.6\esp-idf`
  - `IDF_TOOLS_PATH=D:\Espressif`
  - `IDF_PYTHON_ENV_PATH=D:\Espressif\tools\python\v5.5.6\venv`
  - `ESP_IDF_VERSION=5.5`  (must NOT be 5.5.6)
  - `ESP_ROM_ELF_DIR=D:\Espressif\tools\esp-rom-elfs\20260528\`
  - `IDF_PYTHON_CHECK_CONSTRAINTS=0`
  - `PYTHONUTF8=1`, `PYTHONIOENCODING=utf-8`
- Build command must include the response-file flag (Windows command line limit):
  `idf.py -DCMAKE_NINJA_FORCE_RESPONSE_FILE=ON build`
- Tool versions: cmake 3.30.2, ninja 1.12.1, riscv/xtensa-esp-elf 14.2.0_20260121,
  ccache at `D:\Espressif\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64`.

## Release rules

- **配置来源：只提交 `sdkconfig.defaults`，不提交 `sdkconfig`。**
  - `sdkconfig` 由 IDF 在每个工作区本地生成，已在 `.gitignore` 中；IDF 版本漂移
    （如 `IDF_INIT_VERSION`）因此不会进仓库，也不会造成分支合并冲突。
  - 任何有意的配置调整（内存开关、TCP 缓冲、LVGL 色深、分区表、唤醒词等）
    必须写进 `sdkconfig.defaults`，否则 CI 出的包和本地测的固件不一致。
  - **改配置时只手工改 `sdkconfig.defaults` 里对应的那几行，不要整份重写。**
    尤其不要在自己的 worktree 里跑 `idf.py save-defconfig` 再提交整份文件 ——
    该文件是每行一个符号，整份重写会让每个分支都产生 add/add 冲突，
    合并时只能人工逐行比对（2026-11-27 已因此冲突一次）。
  - 首次创建 defaults（仓库里还没有时）才用 `idf.py save-defconfig` 生成，
    生成后人工检查并入库；之后一律增量修改。
  - 若仍遇到 `sdkconfig.defaults` 冲突：它只是配置文件，直接取"想要的值"即可，
    不要保留冲突标记，改完 `git add` 再继续合并。
  - 新工作区或 CI 首次构建会自动按 defaults 生成 sdkconfig，无需手工拷贝。
- `ota_1` overflow warning (`Part 'ota_1' ... overflow`) is expected: partition layout
  `partitions/v1/32m_dual.csv` reserves ota_1 for another purpose.
- One merged flashable image is produced with `esptool merge_bin` from `build/flash_args`
  offsets; flashing always writes it at `0x0`.

## Known traps

- Opening COM12 triggers one `CHIP_USB_UART_RESET`; boot log gets truncated at
  re-enumeration. That is a capture artifact, not a firmware reboot loop.
- `Board::GetNetwork()` can return null on the ML307 board; any `network->CreateHttp()`
  without a null guard crashes with Guru Meditation and a reboot loop. Guards exist in
  `ota.cc`, `mcp_server.cc`, `protocols/websocket_protocol.cc`, `protocols/mqtt_protocol.cc`, `assets.cc`.
- `esp-sr` `movemodel.py` crashes under GBK console; `PYTHONUTF8=1` is required.
- Do not change the default boot network to 4G without hardware verification; it has
  caused reboot loops before. WiFi remains the default.
- Device credentials (`device_token`, `api_base`) live in NVS, never in source.
