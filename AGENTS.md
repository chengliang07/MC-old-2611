# Project Notes

## ESP32-P4 local flashing

- The reliable local flash command uses `uvx esptool`; do not invent Python paths.
- Example:
  `& 'C:\Users\shuocheng\.local\bin\uvx.exe' esptool --chip esp32p4 --port COM12 --baud 921600 write_flash 0x0 <merged-binary.bin>`
- `C:\Users\shuocheng\.local\bin\esptool.exe` is a broken uv trampoline and must not be used.
