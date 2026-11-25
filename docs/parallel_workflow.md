# 多窗口并行协作约定（metalio-claw4）

> 目的：多个对话窗口同时改不同模块，**每次构建都包含全部最新改动**，**烧录不打架**，**互不覆盖**。
> 约束来源：ESP-IDF 的 `build/` 目录不是多进程安全的；整机只有一个烧录口 COM12。

## 一、核心结论（先看这个）

| 你的需求 | 对应机制 |
|---|---|
| 每次构建包含所有最新改动 | **单一工作区**（不建 worktree），所有窗口改同一棵树 |
| 互不覆盖 | **模块认领表** `docs/parallel/claims.json` + `check_scope.ps1` 自检 |
| 构建不打架 | **构建闸门** `scripts/parallel/build.ps1`（文件锁，串行化） |
| 烧录不打架 | **烧录闸门** `scripts/parallel/flash.ps1`（文件锁 + 端口重试） |
| 最后合并打包 | 集成窗口执行 `merge → 全量构建 → merge_bin → 烧录 → tag` |

**不要用 worktree 做日常并行**：worktree 之间必须 merge 才算"含全部改动"，中间态会出现"我这能编、你那编不过"，正好和你的第 1 条需求相反。
只有一种情况用 worktree：需要对两个完整版本做 A/B 对比验证，或做高风险重构隔离。那时各自 `idf.py -B build-xxx`，最后合并。

## 二、三条铁律

1. **一个窗口只改自己认领的路径**。要跨模块，先在 claims.json 里加路径并通知相关窗口，再改。
2. **构建只能走 `scripts/parallel/build.ps1`**，禁止直接 `idf.py build`。锁保证同一时刻只有一个 ninja；否则就是今晚的 `PermissionError: CMakeCache.txt` / `resources.bin 被占用`。
3. **烧录只能走 `scripts/parallel/flash.ps1`**，并且**只有集成窗口有烧录权**。其他窗口想验证，把固件路径给集成窗口，由它统一烧。串口只有一个，并发烧必有一方失败。

## 三、每个窗口第一条消息的模板（复制粘贴）

```
[模块] oral
[允许改动] main/display/screen/oral_screen/**, docs/oral_relay_contract.md
[只读] main/audio/**, main/application.cc
[构建] scripts/parallel/build.ps1 -Owner oral
[烧录] 无权限（集成窗口统一烧）
[并行中的其他窗口] podcast / audio / ui
[任务] 修 jitter buffer …
```

模块窗口的自我约束：

- 构建前先 `scripts/parallel/check_scope.ps1 -Owner oral`，越界就停下来问，不要硬改。
- 构建失败先看是不是别人的模块编译错误（日志里的路径），是的话不要修别人的文件，报告给对应窗口。
- 改完只 commit 自己的路径：`git add main/display/screen/oral_screen && git commit -m "fix(oral): ..."`。
- **不碰** `sdkconfig`、`CMakeLists.txt`、`partitions/`、`main/idf_component.yml`（这些归集成窗口）。

## 四、集成窗口第一条消息的模板

```
[模块] integration
[允许改动] CMakeLists.txt, main/CMakeLists.txt, sdkconfig, partitions/**, scripts/**, docs/**
[任务] 收口：合并 oral/podcast/audio 三个分支 → 全量构建 → 打包整包 bin → 烧录 → 打 tag
[顺序] 1 合并 2 冲突处理 3 构建 4 merge_bin 5 烧录 6 冒烟 7 tag
```

集成窗口职责：

1. `git merge --no-ff` 各模块分支，冲突只由集成窗口裁决。
2. `scripts/parallel/build.ps1 -Owner integration -Mode fullclean`（全量，避免增量漏编）。
3. `python scripts/release.py`（仓库已有）生成合并整包 `build_artifacts/Metalio_Claw4_<ver>.bin`。
4. `scripts/parallel/flash.ps1 -Owner integration -Mode merged -Image <整包>`。
5. 冒烟：串口看 `app_init App version` + 打开口语页一轮对话。
6. `git tag -a v<ver>`。

## 五、`sdkconfig` 的处理（必须做一次）

`sdkconfig` 被 git 跟踪，但本地构建每次都会重写它（AGENTS.md 明确"不要提交"）。多窗口下它会出现在每个人的 diff 里，并且 merge 必冲突。执行一次：

```powershell
git update-index --skip-worktree sdkconfig
```

之后本窗口的 `git status` 不再显示它，merge 也不参与。集成窗口需要改配置时临时：

```powershell
git update-index --no-skip-worktree sdkconfig
# 改完、构建验证完
git update-index --skip-worktree sdkconfig
```

## 六、典型并行节奏

```
窗口 A (oral)      ──改──> build.ps1 ──> commit ──┐
窗口 B (podcast)   ──改──> build.ps1 ──> commit ──┼──> 集成窗口: merge → full build → 打包 → 烧录 → tag
窗口 C (audio)     ──改──> build.ps1 ──> commit ──┘
```

- 三个窗口的 `build.ps1` 会排队，不会互相踩；每次构建看到的都是三个人当前的磁盘状态（满足"含全部最新改动"）。
- 如果有人正改到一半（编译不过），别人的构建也会失败——这是单工作区的代价。缓解：**小步提交 + 改完立刻构建**，失败就在自己窗口修好再放手。
- 需要"我这半成品不干扰别人"时，用分支：`git switch -c feat/oral-xxx`，改完再切回并合并。切分支前必须 `git stash` 自己的半成品，**且只在没有任何构建在跑时切**（`build.ps1` 会在锁里拒绝并发）。

## 七、成本参考

| 项 | 数值 |
|---|---|
| 单个 `build/` 体积 | ≈0.8 GB |
| 增量构建（改 1 个 cc） | 40–90 s |
| 全量构建 | 12–20 min |
| 烧录 | 25–40 s |
| 一次构建锁的等待 | 通常 <2 min（三个人轮流） |

## 八、工具清单

| 脚本 | 作用 |
|---|---|
| `scripts/parallel/build.ps1` | 串行构建，带锁、日志、错误摘要；`-DryRun` 只验锁与环境 |
| `scripts/parallel/flash.ps1` | 串行烧录，带锁、端口重试、hash 校验 |
| `scripts/parallel/check_scope.ps1` | 校验本次改动是否超出认领范围 |
| `docs/parallel/claims.json` | 模块 → 路径 认领表 |

锁与日志落在仓库根的 `.gates/`（已 gitignore），**不要**放 `build/`：
部分权限环境（含 DSH 沙箱）对 `build/` 不可写，会导致闸门自身拿不到锁。

`.ps1` 脚本一律**纯 ASCII**：Windows PowerShell 5.1 会把无 BOM 的 UTF-8
当 ANSI 解析，中文注释会把后面的引号吃掉（今晚踩过一次）。

已验证的闸门行为：

| 场景 | 结果 |
|---|---|
| 正常取得锁 | `build lock acquired` → 构建 → 释放，不留锁目录 |
| 锁被占用 | 排队等待，超时按 `-LockTimeoutSec` 退出码 3 |
| 崩溃残留锁（>6h） | 自动接管，无需手删 |
| `-DryRun` | 打印 gcc 路径与 IDF 版本，不构建 |
| `flash.ps1 -Mode merged` 缺 `-Image` | 退出码 2 |

