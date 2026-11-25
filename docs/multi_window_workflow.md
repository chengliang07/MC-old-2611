# 多窗口并行开发手册

一台电脑、一个仓库、一台设备，同时改多个模块。核心规则只有三条：

1. **一个窗口 = 一个目录 = 一个分支**（用 `git worktree` 实现，不要在同一目录切分支）
2. **编译互不影响**（每个 worktree 有自己的 `build/`）
3. **烧录必须串行**（一台设备一个串口，统一走 `scripts/flash-device.ps1`）

## 目录布局

```
E:\09-Soft_DEV\metalio-claw4     主窗口：集成 + 烧录（分支 main 或集成分支）
E:\09-Soft_DEV\wt-oral           口语窗口   feat/oral
E:\09-Soft_DEV\wt-pet            宠物窗口   feat/pet
E:\09-Soft_DEV\wt-podcast        播客窗口   feat/podcast
```

每个目录都是完整仓库（同样的代码、各自的 `build/`、各自的分支）。
未提交改动只存在于自己目录里，不会互相覆盖。

## 新开一个模块（三步）

```powershell
cd E:\09-Soft_DEV\metalio-claw4
git worktree add ..\wt-<模块名> -b feat/<模块名> HEAD
```

然后**新开一个对话窗口，打开 `E:\09-Soft_DEV\wt-<模块名>`**，直接说需求即可。

编译：

```powershell
cd E:\09-Soft_DEV\wt-<模块名>
# 编译器环境与主目录一致（IDF_PATH 等见 AGENTS.md）
idf.py -DCMAKE_NINJA_FORCE_RESPONSE_FILE=ON build
```

## 烧录（唯一入口，自带锁）

```powershell
# 在任意 worktree 里执行都可以，锁是全局的（E:\09-Soft_DEV\.flash.lock）
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/flash-device.ps1            # 增量编译 + 烧录
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/flash-device.ps1 -SkipBuild # 已编译，直接烧
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/flash-device.ps1 -Force     # 确认没人占用时强抢
```

被别人占用时会明确报错：

```
设备正在被另一个窗口使用（锁文件：E:\09-Soft_DEV\.flash.lock）
占用信息：{"pid":1234,"repo":"E:\\09-Soft_DEV\\wt-oral",...}
```

**不要**在两个窗口里同时敲 `idf.py flash` 或 `esptool write_flash`，那会互相踢掉串口。

## 合并回 main

模块做完后（在该 worktree 里提交并推送分支），回主窗口：

```powershell
cd E:\09-Soft_DEV\metalio-claw4
git fetch --all
git merge --no-ff feat/<模块名>
```

因为不同模块改的是不同文件，冲突概率极低。真冲突只发生在两个窗口改了同一个文件时。

## 常见问题

| 现象 | 原因 | 处理 |
|---|---|---|
| 两个窗口看到对方的改动 | 用了同一个目录 | 用 worktree，一窗一目录 |
| `Could not open COM12: PermissionError` | 别的进程占着串口（另一个窗口在烧/看串口） | 用 `flash-device.ps1`，或关掉那个窗口/拔插 USB |
| 编译报 `PermissionError ... build\CMakeCache.txt` | 工作区文件权限被沙箱挡住 | 让当前会话切到完全权限，或重跑权限修复 |
| 磁盘占用变大 | 每个 worktree 一份 `build/`（约 800MB） | 不用的 worktree 可以 `git worktree remove <目录>` |
| 想删掉某模块窗口 | — | `git worktree remove ..\wt-<模块名>`（先提交或丢弃改动） |

## 注意

- `sdkconfig` 每个 worktree 一份，互不影响；**不要提交**（仓库规矩）。
- 设备 token 放在 SD 卡 `podcast_token.txt`，卡里常驻即可，刷机抹掉 NVS 后开机会自动补回。
