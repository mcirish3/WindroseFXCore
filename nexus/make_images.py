"""Draw the Nexus gallery image (1920x1080) and header banner (1300x372) for WindroseFXCore."""
from PIL import Image, ImageDraw, ImageFont

D = "/usr/share/fonts/truetype/dejavu/"
def font(name, size): return ImageFont.truetype(D + name, size)

BG, PANEL, LINE = (16, 22, 30), (26, 35, 47), (52, 66, 84)
TEXT, MUTED, GOLD = (232, 236, 241), (150, 163, 179), (231, 176, 74)
TEAL, RED, BLUE = (72, 186, 168), (214, 96, 86), (96, 150, 222)


def centered(d, xy_w, y, s, f, fill):
    x0, w = xy_w
    tw = d.textlength(s, font=f)
    d.text((x0 + (w - tw) / 2, y), s, font=f, fill=fill)


def module_row(d, x, y, title, assign, note, note_fill):
    """Draw four FX modules (2 cores each) with thread labels per core."""
    d.text((x, y), title, font=font("DejaVuSans-Bold.ttf", 34), fill=TEXT)
    mw, mh, gap = 380, 190, 28
    for m in range(4):
        mx = x + m * (mw + gap)
        my = y + 60
        d.rounded_rectangle((mx, my, mx + mw, my + mh), 16, fill=PANEL, outline=LINE, width=3)
        centered(d, (mx, mw), my + 12, f"Module {m}  ·  shared FPU + L2", font("DejaVuSans.ttf", 22), MUTED)
        for c in range(2):
            cx = mx + 18 + c * 176
            cy = my + 52
            label, col = assign.get(m * 2 + c, ("idle / workers", LINE))
            d.rounded_rectangle((cx, cy, cx + 168, cy + 118), 12, fill=BG, outline=col, width=4)
            centered(d, (cx, 168), cy + 14, f"Core {m * 2 + c}", font("DejaVuSans.ttf", 22), MUTED)
            f = font("DejaVuSans-Bold.ttf", 24 if len(label) < 10 else 19)
            centered(d, (cx, 168), cy + 58, label, f, col if col != LINE else MUTED)
    d.text((x, y + 60 + mh + 16), note, font=font("DejaVuSans.ttf", 26), fill=note_fill)


def gallery():
    im = Image.new("RGB", (1920, 1080), BG)
    d = ImageDraw.Draw(im)
    d.rectangle((0, 0, 1920, 8), fill=GOLD)
    d.text((110, 70), "WindroseFXCore", font=font("DejaVuSans-Bold.ttf", 96), fill=TEXT)
    d.text((114, 190), "CPU optimizer for Windrose on older PCs", font=font("DejaVuSans.ttf", 44), fill=GOLD)
    d.text((114, 252), "No graphics settings changed  ·  No game files touched  ·  Works with FSR / DLSS / frame gen",
           font=font("DejaVuSans.ttf", 28), fill=MUTED)

    before = {0: ("GameThread", RED), 1: ("RenderThread", RED), 4: ("RHIThread", BLUE)}
    module_row(d, 110, 330, "Without the mod: Windows may stack critical threads on one module",
               before, "GameThread and RenderThread share one FPU, L2 and decoder → they slow each other down", RED)

    after = {0: ("GameThread", TEAL), 2: ("RenderThread", TEAL), 4: ("RHIThread", TEAL)}
    module_row(d, 110, 660, "With WindroseFXCore: each critical thread gets its own module",
               after, "Background streaming kept off those cores  ·  audio mixer prioritised  ·  0.5 ms timer", TEAL)

    d.text((110, 1020), "Open source (MIT)  ·  github.com/mcirish3/WindroseFXCore  ·  adapts to FX, older Intel Core, early Ryzen and hybrid CPUs",
           font=font("DejaVuSans.ttf", 24), fill=MUTED)
    im.save("gallery-1920x1080.png", optimize=True)


def header():
    im = Image.new("RGB", (1300, 372), BG)
    d = ImageDraw.Draw(im)
    d.rectangle((0, 0, 1300, 6), fill=GOLD)
    d.text((70, 92), "WindroseFXCore", font=font("DejaVuSans-Bold.ttf", 84), fill=TEXT)
    d.text((74, 200), "Smoother Windrose on older CPUs", font=font("DejaVuSans.ttf", 40), fill=GOLD)
    d.text((74, 262), "Runtime thread scheduling · no graphics changes · open source",
           font=font("DejaVuSans.ttf", 26), fill=MUTED)
    # small module motif on the right
    for i in range(4):
        x, y = 1000 + (i % 2) * 150, 80 + (i // 2) * 120
        d.rounded_rectangle((x, y, x + 130, y + 100), 12, fill=PANEL, outline=LINE, width=3)
        for c in range(2):
            col = TEAL if (c == 0 and i < 3) else LINE
            d.rounded_rectangle((x + 12 + c * 56, y + 22, x + 62 + c * 56, y + 78), 8, outline=col, width=4)
    im.save("header-1300x372.png", optimize=True)


gallery()
header()
print("ok")
