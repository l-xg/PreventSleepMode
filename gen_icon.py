# 生成应用图标 resources/app.ico
# 设计：蓝色渐变圆角底 + 白色鼠标(带滚轮) + 左侧移动线条 + 右上"禁止月亮"(拒绝休眠)
from PIL import Image, ImageDraw

S = 1024
img = Image.new('RGBA', (S, S), (0, 0, 0, 0))

# ---- 背景：垂直渐变圆角矩形 ----
top = (90, 167, 232)    # #5AA7E8
bot = (42, 100, 168)    # #2A64A8
grad = Image.new('RGB', (1, S))
for y in range(S):
    t = y / (S - 1)
    grad.putpixel((0, y), tuple(int(a + (b - a) * t) for a, b in zip(top, bot)))
grad = grad.resize((S, S))

mask = Image.new('L', (S, S), 0)
ImageDraw.Draw(mask).rounded_rectangle([28, 28, S - 28, S - 28], radius=230, fill=255)
img.paste(grad, (0, 0), mask)

d = ImageDraw.Draw(img)
bg = (48, 110, 180, 255)      # 线条/细节色（偏深的底色）

# ---- 左侧移动线条（表示鼠标被轻移） ----
for y0, x0, x1 in [(470, 150, 275), (560, 108, 275), (650, 150, 275)]:
    d.rounded_rectangle([x0, y0 - 15, x1, y0 + 15], radius=15, fill=(255, 255, 255, 235))

# ---- 鼠标：竖向胶囊形 + 分键线 + 滚轮 ----
d.rounded_rectangle([340, 350, 660, 780], radius=160, fill=(255, 255, 255, 255))
d.line([500, 366, 500, 430], fill=bg, width=18)
d.rounded_rectangle([472, 448, 528, 586], radius=28, fill=bg)

# ---- 右上：禁止月亮（黄色满月 + 红色禁止圈） ----
mx, my, mr = 792, 252, 104
d.ellipse([mx - mr, my - mr, mx + mr, my + mr], fill=(255, 213, 79, 255))
ring = 229, 72, 77, 255       # #E5484D
d.ellipse([656, 116, 928, 388], outline=ring, width=36)
d.line([692, 152, 892, 352], fill=ring, width=36)

# ---- 导出 ----
sizes = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]
img.save('resources/app.ico', format='ICO', sizes=sizes)
img.resize((256, 256), Image.LANCZOS).save('icon_preview.png')
img.resize((32, 32), Image.LANCZOS).save('icon_preview_32.png')

# 报告 ico 内部条目格式（BMP 或 PNG）
import struct
with open('resources/app.ico', 'rb') as f:
    data = f.read()
count = struct.unpack_from('<H', data, 4)[0]
print(f'ICO entries: {count}')
for i in range(count):
    w, h, _, _, bpp, size, off = struct.unpack_from('<BBBBHHI', data, 6 + 16 * i)
    png = data[off:off + 4] == b'\x89PNG'
    print(f'  {w}x{h} bpp={bpp} {"PNG" if png else "BMP"} bytes={size}')
