# 一丢小猫头鹰像素素材

原创粉色像素风小猫头鹰，用于一丢宠物页面。

## 生成

```bash
python scripts/pet_art/generate_owl.py            # 只输出预览
python scripts/pet_art/generate_owl.py --install  # 同步到 main/xingzhi-assets
```

依赖：`Pillow`（仅美术生成需要，与固件依赖分离）。

## 规格

- 逻辑画布：72x72
- 输出：360x360 PNG，最近邻放大 5 倍
- 透明背景，无抗锯齿、无渐变
- 调色板：`palette.py`，共 17 色

## 状态

| 文件 | 用途 |
|---|---|
| `ic_pet_egg.png` | 未孵化的蛋 |
| `ic_pet_idle.png` | 待机 |
| `ic_pet_blink_half.png` | 眨眼半闭 |
| `ic_pet_blink.png` | 眨眼闭合 |
| `ic_pet_happy.png` | 开心 |
| `ic_pet_curious.png` | 好奇 |
| `ic_pet_think.png` | 思考 |
| `ic_pet_listening.png` | 倾听 |
| `ic_pet_sleep.png` | 睡觉 |

## 资源链路

`main/xingzhi-assets/*.png` 在构建时由 `scripts/spiffs_assets/spiffs_assets_gen.py`
按 16 像素切分并加上 `_SPNG__` 头，生成运行时使用的 `.spng`，
烧录进 `resources` 分区，运行时路径为 `A:ic_pet_xxx.spng`。
因此新增素材只需放入 `main/xingzhi-assets/`，保持同名即可。
