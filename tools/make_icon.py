#!/usr/bin/env python3
"""生成应用图标：青色圆角方块上一条白色小鱼，鱼身上三道「文字行」。

用法：python3 tools/make_icon.py <输出 1024px PNG 路径>
再用 iconutil 打成 packaging/AppIcon.icns（见 README）。
"""
import sys

from PIL import Image, ImageDraw

SCALE = 4                 # 超采样倍数，最后缩回 1024 抗锯齿
SIZE = 1024
TOP = (52, 120, 128)      # 背景渐变上端
BOTTOM = (28, 70, 82)     # 背景渐变下端
FISH = (250, 250, 247)


def s(v):
    return int(round(v * SCALE))


def rounded_square_mask():
    # macOS 图标网格：1024 画布里主体 824×824，留出系统阴影的位置
    mask = Image.new("L", (s(SIZE), s(SIZE)), 0)
    ImageDraw.Draw(mask).rounded_rectangle((s(100), s(100), s(924), s(924)), radius=s(185), fill=255)
    return mask


def gradient():
    img = Image.new("RGB", (s(SIZE), s(SIZE)))
    draw = ImageDraw.Draw(img)
    for y in range(s(SIZE)):
        t = y / (s(SIZE) - 1)
        color = tuple(round(a + (b - a) * t) for a, b in zip(TOP, BOTTOM))
        draw.line((0, y, s(SIZE), y), fill=color)
    return img


def fish_mask():
    # 鱼身是两个等半径圆的交集（透镜形），左边是头，右边接一个三角形尾巴
    cx, cy = 470, 512
    half_len, sag = 220, 140
    radius = (half_len ** 2 + sag ** 2) / (2 * sag)
    offset = radius - sag

    upper = Image.new("L", (s(SIZE), s(SIZE)), 0)
    lower = Image.new("L", (s(SIZE), s(SIZE)), 0)
    ImageDraw.Draw(upper).ellipse(
        (s(cx - radius), s(cy + offset - radius), s(cx + radius), s(cy + offset + radius)), fill=255)
    ImageDraw.Draw(lower).ellipse(
        (s(cx - radius), s(cy - offset - radius), s(cx + radius), s(cy - offset + radius)), fill=255)
    mask = Image.composite(upper, Image.new("L", upper.size, 0), lower)

    tail = ImageDraw.Draw(mask)
    tail.polygon([(s(640), s(cy)), (s(800), s(cy - 112)), (s(778), s(cy)), (s(800), s(cy + 112))], fill=255)

    # 眼睛和三道文字行用背景色挖空
    cut = ImageDraw.Draw(mask)
    eye_r = 22
    cut.ellipse((s(330 - eye_r), s(488 - eye_r), s(330 + eye_r), s(488 + eye_r)), fill=0)
    for y, x_end in ((468, 600), (512, 616), (556, 572)):
        cut.rounded_rectangle((s(420), s(y - 8), s(x_end), s(y + 8)), radius=s(8), fill=0)
    return mask


def main():
    if len(sys.argv) != 2:
        sys.exit("用法：make_icon.py <输出 PNG>")
    canvas = Image.new("RGBA", (s(SIZE), s(SIZE)), (0, 0, 0, 0))
    canvas.paste(gradient(), (0, 0), rounded_square_mask())
    canvas.paste(Image.new("RGBA", canvas.size, FISH + (255,)), (0, 0), fish_mask())
    canvas.resize((SIZE, SIZE), Image.LANCZOS).save(sys.argv[1])


if __name__ == "__main__":
    main()
