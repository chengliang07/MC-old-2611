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

- Never commit `sdkconfig`: it is rewritten by the local IDF patch version and drifts
  (SOC symbol renames, ESP-Hosted target). Local builds carry build timestamps, so
  the official package must come from CI, not from a local build.
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
