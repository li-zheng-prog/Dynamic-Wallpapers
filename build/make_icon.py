"""从 logo.png 生成应用图标 app.ico（exe / 托盘 / 窗口 / 安装程序共用这一份）。

裁切形状 = 三星 One UI 应用图标的官方方圆形（squircle）：连续曲率、整条边都是曲线、
无直线段，坐标取自 One UI Design Guidelines「App icons in One UI」图标底板的矢量路径
（归一化 0..1，One UI 7.0 默认形状与之一致），不做任何缩放改动。
小尺寸（≤32px）缩放后加一道轻锐化：箭头和时钟会糊，托盘在 200% 缩放下系统取 32px，
小档画质直接决定托盘观感。
换图标：替换项目根目录的 logo.png（正方形、建议 512 以上），重新跑 build/build.sh 即可。
logo 周边别留太多空白：图标要顶到边，四周大块留白会显得图标很小。

用法: python make_icon.py <输出路径.ico> [logo.png 路径]
"""
import io
import os
import struct
import sys

from PIL import Image, ImageDraw, ImageFilter

SIZES = (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)

# One UI 方圆形路径，归一化 0..1 坐标（起点不另记，每段 = x1,y1 c1x,c1y c2x,c2y x2,y2）。
# 段间有零长度控制柄属于官方路径原样，三次贝塞尔求值不受影响。
_SQUIRCLE_SEGS = (
    (0.988307, 0.351697, 0.976083, 0.259311, 0.943369, 0.175610, 0.885730, 0.114288),
    (0.885730, 0.114288, 0.824390, 0.056631, 0.740690, 0.023935, 0.648284, 0.011693),
    (0.648284, 0.011693, 0.560076, 0.000000, 0.499982, 0.002878, 0.499982, 0.002878),
    (0.499982, 0.002878, 0.439905, 0.000000, 0.351679, 0.011693, 0.351679, 0.011693),
    (0.351679, 0.011693, 0.259310, 0.023935, 0.175610, 0.056631, 0.114288, 0.114288),
    (0.114288, 0.114288, 0.056612, 0.175610, 0.023935, 0.259311, 0.011693, 0.351697),
    (0.011693, 0.351697, 0.000000, 0.439905, 0.002895, 0.500000, 0.002895, 0.500000),
    (0.002895, 0.500000, 0.002895, 0.500000, 0.000000, 0.560076, 0.011693, 0.648303),
    (0.011693, 0.648303, 0.023935, 0.740672, 0.056612, 0.824390, 0.114288, 0.885712),
    (0.114288, 0.885712, 0.175610, 0.943369, 0.259310, 0.976065, 0.351679, 0.988307),
    (0.351679, 0.988307, 0.439905, 1.000000, 0.499982, 0.997123, 0.499982, 0.997123),
    (0.499982, 0.997123, 0.560076, 1.000000, 0.648284, 0.988307, 0.648284, 0.988307),
    (0.648284, 0.988307, 0.740690, 0.976065, 0.824390, 0.943369, 0.885694, 0.885731),
    (0.885694, 0.885731, 0.943369, 0.824390, 0.976083, 0.740672, 0.988307, 0.648303),
    (0.988307, 0.648303, 1.000000, 0.560076, 0.997122, 0.500000, 0.997122, 0.500000),
    (0.997122, 0.500000, 0.997122, 0.500000, 1.000000, 0.439905, 0.988307, 0.351697),
)

_SQUIRCLE_BASE_PX = 1024      # 大蒙版一次画好，各档尺寸从它缩下去（小档等效高倍超采样）
_squircleBase = None
_maskCache = {}


def squircle_mask(size: int) -> Image.Image:
    """One UI 方圆形蒙版（L 模，形状内为 255）。PIL 的多边形没有自带抗锯齿，
    先在 1024px 上把矢量路径展成多边形画好，再 LANCZOS 缩到目标尺寸，圆角边缘才平滑。"""
    global _squircleBase
    if size not in _maskCache:
        if _squircleBase is None:
            pts = []
            s = _SQUIRCLE_BASE_PX
            x1, y1 = _SQUIRCLE_SEGS[0][0], _SQUIRCLE_SEGS[0][1]
            pts.append((x1 * s, y1 * s))
            for (ax, ay, cx1, cy1, cx2, cy2, bx, by) in _SQUIRCLE_SEGS:
                for i in range(1, 33):
                    t = i / 32.0
                    u = 1.0 - t
                    x = u * u * u * ax + 3 * u * u * t * cx1 + 3 * u * t * t * cx2 + t * t * t * bx
                    y = u * u * u * ay + 3 * u * u * t * cy1 + 3 * u * t * t * cy2 + t * t * t * by
                    pts.append((x * s, y * s))
            big = Image.new("L", (s, s), 0)
            ImageDraw.Draw(big).polygon(pts, fill=255)
            _squircleBase = big
        _maskCache[size] = _squircleBase.resize((size, size), Image.LANCZOS)
    return _maskCache[size]


def make(size: int, src: Image.Image) -> Image.Image:
    img = src.resize((size, size), Image.LANCZOS)
    if size <= 32:                        # 小尺寸：轻锐化，减轻缩放糊
        img = img.filter(ImageFilter.UnsharpMask(radius=1.2, percent=90, threshold=2))
    out = img.convert("RGBA")
    out.putalpha(squircle_mask(size))
    return out


def write_ico(entries, path):
    """手写 ICO（每档用 PNG 存）"""
    pngs = []
    for size, img in entries:
        buf = io.BytesIO()
        img.convert("RGBA").save(buf, format="PNG")
        pngs.append((size, buf.getvalue()))
    header = struct.pack("<HHH", 0, 1, len(pngs))
    offset = 6 + 16 * len(pngs)
    dir_bytes, blob = b"", b""
    for size, data in pngs:
        wh = 0 if size >= 256 else size
        dir_bytes += struct.pack("<BBBBHHII", wh, wh, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
        blob += data
    with open(path, "wb") as f:
        f.write(header + dir_bytes + blob)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "app.ico"
    here = os.path.dirname(os.path.abspath(__file__))
    logo = sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, "..", "logo.png")
    logo = os.path.normpath(logo)
    if not os.path.exists(logo):
        print("!! 找不到 logo.png:", logo)
        sys.exit(1)
    src = Image.open(logo).convert("RGBA")
    if src.width != src.height:                       # 非正方形：居中裁成正方形
        side = min(src.width, src.height)
        src = src.crop(((src.width - side) // 2, (src.height - side) // 2,
                        (src.width - side) // 2 + side, (src.height - side) // 2 + side))
    entries = [(s, make(s, src)) for s in SIZES]
    write_ico(entries, out)
    # 顺便写一份方圆形 PNG 到同目录（app.rc / setup.rc 把它作为 IDR_LOGO_PNG 打进 exe，
    # 主界面和安装界面标题栏左上角的小标记就是它）
    ui_png = os.path.join(os.path.dirname(os.path.abspath(out)), "logo_ui.png")
    make(128, src).save(ui_png)
    print(f"icon written: {out} sizes: {list(SIZES)} (from {logo}, {src.width}px)")
    print(f"ui mark written: {ui_png}")


if __name__ == "__main__":
    main()
