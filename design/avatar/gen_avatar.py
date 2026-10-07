"""生成 WESR-STOCK 小程序头像候选（144x144 PNG，双色，跟板子 1-bit 屏一个调性）。

三个候选都只画黑底白线：小程序导航栏本来就是 #000000 + 白字（app.json）。
几何用 0~1 的分数写，所以同一个函数既能出 144（提交用）也能出更大的预览。

用法：python design/avatar/gen_avatar.py
产物：avatar-a/b/c.png（144x144，提交用）+ preview.png（细节 + 圆形裁切的小尺寸观感）
"""
import os
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
BLACK = (0, 0, 0)
WHITE = (255, 255, 255)
BG = (245, 245, 245)
SS = 4                      # 超采样倍数：先画大再 LANCZOS 缩，边缘才干净


def _pts(frac, size):
    return [(x * size, y * size) for x, y in frac]


def _stroke(draw, frac, size, width_frac):
    """连折线 + 自己补圆头（line 的 joint 只管拐点，端点是平的）"""
    w = max(1, round(width_frac * size))
    draw.line(_pts(frac, size), fill=WHITE, width=w, joint="curve")
    r = w / 2.0
    for x, y in _pts(frac, size):
        draw.ellipse([x - r, y - r, x + r, y + r], fill=WHITE)


# ---- 候选 A：分时线 + 量柱（板子第 3 页的构图）----
# 折点刻意少：36px 下 8 个折点会糊成一条抖动的线，5 个才看得出"上涨"
A_LINE = [(0.160, 0.650), (0.330, 0.520), (0.470, 0.600), (0.660, 0.375), (0.850, 0.245)]
A_BARS = [(0.208, 0.760), (0.347, 0.720), (0.486, 0.792), (0.625, 0.705), (0.764, 0.745)]
A_BAR_W, A_BAR_BOT = 0.095, 0.855


def draw_a(d, size):
    _stroke(d, A_LINE, size, 0.076)
    half = A_BAR_W / 2 * size
    for cx, top in A_BARS:
        x = cx * size
        d.rectangle([x - half, top * size, x + half, A_BAR_BOT * size], fill=WHITE)


# ---- 候选 B：三根阳线 ----
B_CANDLES = [  # (中心 x, 实体上, 实体下, 影线上, 影线下)
    (0.278, 0.583, 0.722, 0.514, 0.778),
    (0.500, 0.417, 0.667, 0.347, 0.722),
    (0.722, 0.236, 0.556, 0.167, 0.625),
]
B_BODY_W, B_WICK_W = 0.139, 0.035


def draw_b(d, size):
    body, wick = B_BODY_W / 2 * size, B_WICK_W / 2 * size
    for cx, t, b, wt, wb in B_CANDLES:
        x = cx * size
        d.rectangle([x - wick, wt * size, x + wick, wb * size], fill=WHITE)
        d.rectangle([x - body, t * size, x + body, b * size], fill=WHITE)


# ---- 候选 C：折线走成 W（品牌首字母 + 行情双关）----
C_W = [(0.167, 0.278), (0.333, 0.722), (0.500, 0.389), (0.667, 0.722), (0.833, 0.278)]


def draw_c(d, size):
    _stroke(d, C_W, size, 0.090)


def _scale(frac, f):
    return [(0.5 + (x - 0.5) * f, 0.5 + (y - 0.5) * f) for x, y in frac]


# ---- 候选 D：圆角边框里放 W（标准"应用图标"做法，边框掉了也还剩 W）
# 边框必须明显小于圆形裁切，否则四个角会被切掉、看着像两个圈套在一起
def draw_d(d, size):
    w = round(0.050 * size)
    d.rounded_rectangle([0.155 * size, 0.155 * size, 0.845 * size, 0.845 * size],
                        radius=0.22 * size, outline=WHITE, width=w)
    _stroke(d, _scale(C_W, 0.78), size, 0.072)


CANDS = [("A", "分时线 + 量柱", draw_a), ("B", "三根阳线", draw_b),
         ("C", "折线 W", draw_c), ("D", "W + 圆角边框", draw_d)]

CHOSEN = "D"    # 定稿：WESR桌面屏助手 的小程序头像（见 miniprogram/PUBLISH.md）


def render(fn, size):
    big = size * SS
    img = Image.new("RGB", (big, big), BLACK)
    fn(ImageDraw.Draw(img), big)
    return img.resize((size, size), Image.LANCZOS)


def circle(img, bg=BG):
    """按微信实际展示的圆形裁一下，用来看小尺寸下还认不认得出"""
    big = img.size[0] * SS
    mask = Image.new("L", (big, big), 0)
    ImageDraw.Draw(mask).ellipse([0, 0, big - 1, big - 1], fill=255)
    mask = mask.resize(img.size, Image.LANCZOS)
    out = Image.new("RGB", img.size, bg)
    out.paste(img, (0, 0), mask)
    return out


def font(sz):
    for p in (r"C:\Windows\Fonts\msyh.ttc", r"C:\Windows\Fonts\segoeui.ttf",
              r"C:\Windows\Fonts\arial.ttf"):
        if os.path.exists(p):
            try:
                return ImageFont.truetype(p, sz)
            except OSError:
                pass
    return ImageFont.load_default()


def main():
    for tag, _desc, fn in CANDS:
        img = render(fn, 144)
        path = os.path.join(HERE, "avatar-%s.png" % tag.lower())
        img.save(path, "PNG", optimize=True)
        print("%s  %s  %d 字节%s" % (path, img.size, os.path.getsize(path),
                                    "   <-- 定稿，提交用这个" if tag == CHOSEN else ""))

    # 对比图：每行一个候选 —— 288 看细节 / 144 圆形 / 72 / 36（小图放大成最近邻，保持像素感）
    pad, label_w = 20, 210
    slots = [(288, "288 原样", 1), (144, "144 圆形（提交尺寸）", 1),
             (72, "72 圆形 →2x", 2), (36, "36 圆形 →4x", 4)]
    row_h = 288
    sheet_w = label_w + sum(sz * z + pad for sz, _c, z in slots)
    sheet_h = pad + len(CANDS) * (row_h + pad)
    sheet = Image.new("RGB", (sheet_w, sheet_h), (250, 250, 250))
    d = ImageDraw.Draw(sheet)
    f24, f14 = font(24), font(14)

    # 表头
    for i, (_tag, _desc, fn) in enumerate(CANDS):
        y = pad + i * (row_h + pad)
        d.text((pad, y + 120), "%s  %s" % (_tag, _desc), fill=(20, 20, 20), font=f24)
        x = label_w
        for sz, cap, zoom in slots:
            im = render(fn, 288) if sz == 288 else circle(render(fn, sz))
            if zoom > 1:
                im = im.resize((sz * zoom, sz * zoom), Image.NEAREST)
            sheet.paste(im, (x, y + (row_h - im.size[1]) // 2))
            d.text((x, y + 2), cap, fill=(120, 120, 120), font=f14)
            x += sz * zoom + pad
    out = os.path.join(HERE, "preview.png")
    sheet.save(out, "PNG", optimize=True)
    print("%s  %s  %d 字节" % (out, sheet.size, os.path.getsize(out)))


if __name__ == "__main__":
    main()
