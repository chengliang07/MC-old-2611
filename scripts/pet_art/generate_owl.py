#!/usr/bin/env python3
"""生成一丢小猫头鹰的像素素材。

流程：
1. 在 72x72 逻辑画布上用固定调色板逐像素绘制。
2. 使用最近邻算法放大 5 倍，输出 360x360 透明 PNG。
3. 输出到 scripts/pet_art/output/，可选用 --install 同步到
   main/xingzhi-assets/（固件构建时由 spiffs_assets_gen.py 转成 .spng）。

不使用抗锯齿、模糊或连续渐变；所有形状都由离散矩形/椭圆像素构成。
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from palette import CANVAS, PALETTE, SCALE  # noqa: E402

OUTPUT_DIR = os.path.join(HERE, "output")
INSTALL_DIR = os.path.normpath(
    os.path.join(HERE, "..", "..", "main", "xingzhi-assets")
)

C = {name: rgba[:3] for name, rgba in PALETTE.items()}
SHADOW_ALPHA = PALETTE["shadow"][3]


class PixelCanvas:
    """72x72 逻辑像素画布，透明背景。"""

    def __init__(self) -> None:
        self.size = CANVAS
        self.pixels = [[None for _ in range(self.size)] for _ in range(self.size)]

    def set(self, x: int, y: int, color: str | tuple[int, int, int], alpha: int = 255) -> None:
        if 0 <= x < self.size and 0 <= y < self.size:
            rgb = C[color] if isinstance(color, str) else color
            self.pixels[y][x] = (rgb[0], rgb[1], rgb[2], alpha)

    def get(self, x: int, y: int):
        if 0 <= x < self.size and 0 <= y < self.size:
            return self.pixels[y][x]
        return None

    def ellipse(self, cx: float, cy: float, rx: float, ry: float,
                color: str, alpha: int = 255) -> None:
        x0, x1 = int(cx - rx - 1), int(cx + rx + 1)
        y0, y1 = int(cy - ry - 1), int(cy + ry + 1)
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                dx = (x + 0.5 - cx) / rx
                dy = (y + 0.5 - cy) / ry
                if dx * dx + dy * dy <= 1.0:
                    self.set(x, y, color, alpha)

    def ellipse_outline(self, cx: float, cy: float, rx: float, ry: float,
                        color: str, thickness: float = 1.0) -> None:
        x0, x1 = int(cx - rx - 2), int(cx + rx + 2)
        y0, y1 = int(cy - ry - 2), int(cy + ry + 2)
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                dx = (x + 0.5 - cx) / rx
                dy = (y + 0.5 - cy) / ry
                d = dx * dx + dy * dy
                if (1.0 - thickness / max(rx, ry)) < d <= 1.0:
                    self.set(x, y, color)

    def rect(self, x0: int, y0: int, x1: int, y1: int, color: str,
             alpha: int = 255) -> None:
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                self.set(x, y, color, alpha)

    def hline(self, x0: int, x1: int, y: int, color: str) -> None:
        for x in range(min(x0, x1), max(x0, x1) + 1):
            self.set(x, y, color)

    def line(self, x0: int, y0: int, x1: int, y1: int, color: str) -> None:
        dx = abs(x1 - x0)
        dy = -abs(y1 - y0)
        sx = 1 if x0 < x1 else -1
        sy = 1 if y0 < y1 else -1
        err = dx + dy
        while True:
            self.set(x0, y0, color)
            if x0 == x1 and y0 == y1:
                break
            e2 = 2 * err
            if e2 >= dy:
                err += dy
                x0 += sx
            if e2 <= dx:
                err += dx
                y0 += sy

    def to_image(self) -> Image.Image:
        img = Image.new("RGBA", (self.size, self.size), (0, 0, 0, 0))
        px = img.load()
        for y in range(self.size):
            for x in range(self.size):
                if self.pixels[y][x] is not None:
                    px[x, y] = self.pixels[y][x]
        return img.resize((self.size * SCALE, self.size * SCALE), Image.NEAREST)


def draw_sprout(c: PixelCanvas) -> None:
    """头顶绿色小芽，一丢的识别标志。"""
    c.rect(35, 5, 36, 9, "green_dark")
    c.rect(35, 6, 35, 9, "green")
    c.ellipse(32.0, 4.5, 3.0, 2.2, "green")
    c.ellipse(31.5, 4.0, 1.6, 1.2, "green_light")
    c.ellipse(40.0, 4.5, 3.0, 2.2, "green")
    c.ellipse(39.5, 4.0, 1.6, 1.2, "green_light")


def draw_ear_tufts(c: PixelCanvas, tall: bool = False) -> None:
    top = 8 if tall else 11
    for side, cx in ((-1, 23), (1, 49)):
        for i in range(4 if tall else 3):
            half = max(0, (4 if tall else 3) - i - 1)
            c.hline(cx - half, cx + half, top + i, "brown_dark")
        c.set(cx, top - 1, "outline")


def draw_body(c: PixelCanvas) -> None:
    c.ellipse(36.0, 64.5, 16.0, 2.6, (59, 39, 24), SHADOW_ALPHA)
    c.ellipse(36.0, 46.5, 17.0, 14.0, "outline")
    c.ellipse(36.0, 46.5, 16.0, 13.0, "brown")
    c.ellipse(36.0, 50.5, 10.5, 8.5, "cream")
    # 底部阴影增强体块感
    c.ellipse(36.0, 57.5, 12.0, 3.0, "brown_dark")
    c.ellipse(36.0, 50.5, 10.5, 7.6, "cream")


def draw_wings(c: PixelCanvas) -> None:
    c.ellipse(17.0, 47.0, 5.4, 9.5, "outline")
    c.ellipse(17.0, 47.0, 4.4, 8.5, "brown_dark")
    c.ellipse(16.0, 44.0, 2.4, 3.4, "brown")
    c.ellipse(55.0, 47.0, 5.4, 9.5, "outline")
    c.ellipse(55.0, 47.0, 4.4, 8.5, "brown_dark")
    c.ellipse(56.0, 44.0, 2.4, 3.4, "brown")


def draw_feet(c: PixelCanvas) -> None:
    for x0 in (27, 39):
        c.rect(x0, 59, x0 + 5, 62, "outline")
        c.rect(x0 + 1, 60, x0 + 4, 62, "orange")
        c.hline(x0 + 1, x0 + 4, 63, "orange_dark")


def draw_head(c: PixelCanvas) -> None:
    c.ellipse(36.0, 27.0, 19.5, 17.5, "outline")
    c.ellipse(36.0, 27.0, 18.5, 16.5, "brown")
    # 面部奶油色圆盘
    c.ellipse(29.0, 29.0, 10.5, 9.5, "cream")
    c.ellipse(43.0, 29.0, 10.5, 9.5, "cream")
    c.ellipse(36.0, 33.0, 6.0, 4.5, "cream")
    # 顶部高光块
    c.ellipse(33.0, 14.0, 7.0, 2.4, "brown_light")


def draw_eye(c: PixelCanvas, cx: float, cy: float, mood: str) -> None:
    if mood in ("blink", "blink_half", "sleep"):
        c.ellipse(cx, cy, 7.2, 6.8, "outline")
        c.ellipse(cx, cy, 6.2, 5.8, "cream")
        ix, iy = int(cx), int(cy)
        if mood == "sleep":
            c.hline(ix - 4, ix + 4, iy + 2, "eye_black")
            c.set(ix - 5, iy + 1, "eye_black")
            c.set(ix + 5, iy + 1, "eye_black")
        elif mood == "blink_half":
            c.hline(ix - 5, ix + 5, iy + 2, "eye_black")
            c.hline(ix - 4, ix + 4, iy + 3, "eye_black")
            c.hline(ix - 6, ix + 6, iy - 1, "cream_dark")
        else:
            c.hline(ix - 5, ix + 5, iy + 1, "eye_black")
            c.hline(ix - 4, ix + 4, iy + 2, "eye_black")
        return

    c.ellipse(cx, cy, 7.4, 7.0, "outline")
    c.ellipse(cx, cy, 6.4, 6.0, "eye_white")

    pupil_dx, pupil_dy, pupil_r = 0.0, 0.0, 3.4
    if mood == "curious":
        pupil_dy = -0.8
        pupil_r = 3.9
    elif mood == "think":
        pupil_dx, pupil_dy = -1.2, -1.4
        pupil_r = 3.0
    elif mood == "listening":
        pupil_r = 3.8
    elif mood == "happy":
        # 开心：弯月眼
        c.ellipse(cx, cy, 6.4, 6.0, "cream")
        c.line(int(cx - 5), int(cy + 3), int(cx - 1), int(cy - 2), "eye_black")
        c.line(int(cx - 1), int(cy - 2), int(cx + 3), int(cy + 3), "eye_black")
        c.line(int(cx - 5), int(cy + 4), int(cx - 1), int(cy - 1), "eye_black")
        c.line(int(cx - 1), int(cy - 1), int(cx + 3), int(cy + 4), "eye_black")
        return

    c.ellipse(cx + pupil_dx, cy + pupil_dy, pupil_r, pupil_r, "eye_black")
    c.ellipse(cx + pupil_dx - 1.2, cy + pupil_dy - 1.2, 1.1, 1.1, "eye_blue")
    c.set(int(cx + pupil_dx + 0.5), int(cy + pupil_dy + 1.5), "eye_white")


def draw_eyes(c: PixelCanvas, mood: str) -> None:
    draw_eye(c, 29.0, 29.0, mood)
    draw_eye(c, 43.0, 29.0, mood)


def draw_beak(c: PixelCanvas, open_beak: bool = False) -> None:
    if open_beak:
        c.rect(34, 36, 38, 39, "orange_dark")
        c.rect(35, 37, 37, 41, "orange")
        c.hline(35, 37, 40, "outline")
        return
    c.rect(35, 36, 37, 38, "orange_dark")
    c.rect(35, 36, 37, 37, "orange")
    c.set(36, 39, "orange_dark")


def draw_blush(c: PixelCanvas) -> None:
    c.rect(21, 35, 24, 36, "blush")
    c.rect(48, 35, 51, 36, "blush")


def draw_zzz(c: PixelCanvas) -> None:
    # 5x5 像素小 Z，避免依赖字体。
    c.rect(56, 12, 60, 12, "cream_light")
    c.set(59, 13, "cream_light")
    c.set(58, 14, "cream_light")
    c.set(57, 15, "cream_light")
    c.rect(56, 16, 60, 16, "cream_light")


def draw_sound_waves(c: PixelCanvas) -> None:
    for i, r in enumerate((9, 12)):
        for dy in range(-r, r + 1):
            x = int(round(58 + (r * r - dy * dy) ** 0.5)) if abs(dy) <= r else 0
            if i == 0:
                c.set(x, 27 + dy, "cream_dark")
        for dy in range(-r, r + 1):
            x = int(round(58 + (r * r - dy * dy) ** 0.5)) if abs(dy) <= r else 0
            if i == 1:
                c.set(x, 27 + dy, "cream_dark")


def draw_egg(c: PixelCanvas) -> None:
    c.ellipse(36.0, 63.0, 14.0, 2.4, (59, 39, 24), SHADOW_ALPHA)
    c.ellipse(36.0, 40.0, 16.5, 21.0, "outline")
    c.ellipse(36.0, 40.0, 15.5, 20.0, "cream")
    c.ellipse(31.0, 33.0, 5.0, 7.0, "cream_light")
    # 棕色斑点
    for (x, y, w, h) in ((28, 46, 3, 2), (40, 33, 3, 3), (43, 45, 2, 2),
                         (33, 52, 2, 2), (24, 38, 2, 2)):
        c.rect(x, y, x + w - 1, y + h - 1, "brown_light")
    draw_sprout(c)


def build_state(mood: str) -> PixelCanvas:
    c = PixelCanvas()
    if mood == "egg":
        draw_egg(c)
        return c

    tall_ears = mood == "listening"
    draw_ear_tufts(c, tall=tall_ears)
    draw_body(c)
    draw_feet(c)
    draw_wings(c)
    draw_head(c)
    draw_eyes(c, mood)
    draw_beak(c, open_beak=(mood == "happy"))
    if mood == "happy":
        draw_blush(c)
    if mood == "listening":
        draw_sound_waves(c)
    if mood == "sleep":
        draw_zzz(c)
    if mood == "think":
        for i, x in enumerate((53, 56, 59)):
            c.rect(x, 20 + (i % 2), x + 1, 21 + (i % 2), "cream_light")
    draw_sprout(c)
    return c


STATES = [
    ("egg", "ic_pet_egg"),
    ("idle", "ic_pet_idle"),
    ("blink_half", "ic_pet_blink_half"),
    ("blink", "ic_pet_blink"),
    ("happy", "ic_pet_happy"),
    ("curious", "ic_pet_curious"),
    ("think", "ic_pet_think"),
    ("listening", "ic_pet_listening"),
    ("sleep", "ic_pet_sleep"),
]


def make_contact_sheet(images: list[tuple[str, Image.Image]], path: str) -> None:
    cols = 3
    cell = 360
    pad = 16
    rows = (len(images) + cols - 1) // cols
    sheet = Image.new("RGBA", (cols * (cell + pad) + pad,
                               rows * (cell + pad) + pad), (11, 16, 32, 255))
    for i, (name, img) in enumerate(images):
        x = pad + (i % cols) * (cell + pad)
        y = pad + (i // cols) * (cell + pad)
        sheet.alpha_composite(img, (x, y))
    sheet.save(path)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--install", action="store_true",
                        help="同步到 main/xingzhi-assets（供固件构建打包）")
    args = parser.parse_args()

    os.makedirs(OUTPUT_DIR, exist_ok=True)
    rendered: list[tuple[str, Image.Image]] = []
    for mood, asset in STATES:
        img = build_state(mood).to_image()
        img.save(os.path.join(OUTPUT_DIR, asset + ".png"))
        rendered.append((mood, img))

    make_contact_sheet(rendered, os.path.join(OUTPUT_DIR, "owl_contact_sheet.png"))

    if args.install:
        os.makedirs(INSTALL_DIR, exist_ok=True)
        for _, asset in STATES:
            shutil.copyfile(os.path.join(OUTPUT_DIR, asset + ".png"),
                            os.path.join(INSTALL_DIR, asset + ".png"))
        print("installed to", INSTALL_DIR)

    print("generated", len(rendered), "images in", OUTPUT_DIR)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
