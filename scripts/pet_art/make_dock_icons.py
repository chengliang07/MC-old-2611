#!/usr/bin/env python3
"""生成宠物页底部应用入口用的 64x64 图标。

来源是主屏主题图标（128x128），这里统一取主题 1 的版本、等比例缩到 64x64，
输出到 main/xingzhi-assets/ic_pet_dock_*.png，供 pet_screen 的四个圆按钮使用。
底部按钮不跟随主题切换：宠物页只有这一处图标，保持稳定比跟随主题更重要。
"""

from __future__ import annotations

import os

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.normpath(os.path.join(HERE, "..", "..", "main", "xingzhi-assets"))
SIZE = 64

# 输出名 -> 主屏图标后缀
ICONS = {
    "ic_pet_dock_english": "music",          # 一丢英语
    "ic_pet_dock_oral": "digital_people",    # 一丢口语
    "ic_pet_dock_podcast": "radio",          # 一丢播客
    "ic_pet_dock_math": "calculator",        # 一丢数学（占位）
}


def main() -> int:
    for out_name, suffix in ICONS.items():
        src = os.path.join(ASSETS, f"ic_app_home_theme1_{suffix}.png")
        img = Image.open(src).convert("RGBA")
        img = img.resize((SIZE, SIZE), Image.LANCZOS)
        img.save(os.path.join(ASSETS, out_name + ".png"))
        print("wrote", out_name + ".png")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
